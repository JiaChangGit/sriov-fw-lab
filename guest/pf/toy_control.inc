/* 本檔由 toy_pf.c 納入；所有管理操作均使用同一 admin_mutex。 */
static struct toy_uapi_header toy_header(size_t size)
{
    return (struct toy_uapi_header){ size, TOY_UAPI_MAJOR, TOY_UAPI_MINOR };
}

static int toy_header_valid(const struct toy_uapi_header *hdr, size_t size)
{
    if (hdr->size != size)
        return -EINVAL;
    if (hdr->abi_major != TOY_UAPI_MAJOR || hdr->abi_minor != TOY_UAPI_MINOR)
        return -EPROTONOSUPPORT;
    return 0;
}

static bool toy_reserved_zero(const u32 *reserved, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i)
        if (reserved[i])
            return false;
    return true;
}

static void toy_snapshot_vf(struct toy_pf *pf, struct toy_file *ctx,
                            unsigned int vf, struct toy_vf_info *out)
{
    u32 off = TOY_VF_VIEW(vf, 0);
    memset(out, 0, sizeof(*out));
    out->hdr = toy_header(sizeof(*out));
    out->vf = vf;
    out->session = toy_read64(pf, TOY_PF_SESSION_LO);
    out->epoch = toy_read64(pf, off + TOY_PF_VF_VIEW_EPOCH_LO);
    out->state = toy_read(pf, off + TOY_PF_VF_VIEW_STATE);
    out->hw_flags = toy_read(pf, off + TOY_PF_VF_VIEW_HW_STATUS);
    out->reserve = toy_read(pf, off + TOY_PF_VF_VIEW_RESERVE);
    out->outstanding = toy_read(pf, off + TOY_PF_VF_VIEW_OUTSTANDING);
    out->ready_gen = toy_read(pf, off + TOY_PF_VF_VIEW_READY_GEN);
    if (ctx && pf->leases[vf].owner == ctx)
        out->lease_token = pf->leases[vf].token;
}

static bool toy_has_leases(struct toy_pf *pf)
{
    unsigned int i;
    for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i)
        if (pf->leases[i].token)
            return true;
    return false;
}

/* fanout 只在 admin_mutex 下執行；IRQ 不接觸 context list。 */
static void toy_publish(struct toy_pf *pf, u32 type, unsigned int vf, u32 detail,
                        const struct toy_vf_info *snapshot)
{
    struct toy_file *ctx;
    list_for_each_entry(ctx, &pf->files, link) {
        struct toy_event event = { .hdr = toy_header(sizeof(event)), .type = type,
                                   .vf = vf, .detail = detail };
        unsigned long flags;
        if (snapshot) {
            event.session = snapshot->session;
            event.epoch = snapshot->epoch;
            event.ready_gen = snapshot->ready_gen;
            event.state = snapshot->state;
            event.hw_flags = snapshot->hw_flags;
        }
        if (vf < TOY_PCI_TOTAL_VFS && pf->leases[vf].owner == ctx)
            event.lease_token = pf->leases[vf].token;
        spin_lock_irqsave(&ctx->event_lock, flags);
        if (ctx->count == TOY_EVENT_CAPACITY) {
            ctx->head = (ctx->head + 1) % TOY_EVENT_CAPACITY;
            --ctx->count;
            if (ctx->lost != U32_MAX)
                ++ctx->lost;
        }
        ctx->events[(ctx->head + ctx->count) % TOY_EVENT_CAPACITY] = event;
        ++ctx->count;
        spin_unlock_irqrestore(&ctx->event_lock, flags);
        wake_up_interruptible(&ctx->event_wait);
    }
}

static void toy_notify_work(struct work_struct *work)
{
    struct toy_pf *pf = container_of(work, struct toy_pf, notify_work);
    unsigned int i;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying)
        goto out;
    /* 一次 coherent snapshot 覆蓋合併 IRQ；只報實際觀察到的 tuple 變化。 */
    atomic_xchg(&pf->pending_events, 0);
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i) {
        struct toy_vf_info now;
        u32 fault;
        toy_snapshot_vf(pf, NULL, i, &now);
        fault = toy_read(pf, TOY_VF_VIEW(i, TOY_PF_VF_VIEW_LAST_FAULT));
        if (pf->leases[i].token) {
            if (now.epoch != pf->observed[i].epoch ||
                now.session != pf->observed[i].session) {
                pf->leases[i].fence_valid = false;
                toy_publish(pf, TOY_EVENT_FLR, i, 0, &now);
            }
            if ((now.state == TOY_STATES_FAILED && pf->observed[i].state != now.state) ||
                fault != pf->observed_fault[i] ||
                ((now.hw_flags ^ pf->observed[i].hw_flags) & TOY_HW_FLAGS_KILLED))
                toy_publish(pf, TOY_EVENT_FAULT, i, fault, &now);
        }
        pf->observed[i] = now;
        pf->observed_fault[i] = fault;
    }
out:
    mutex_unlock(&pf->admin_mutex);
}

/* 一次 open 建立一個 context；dup 共用同一 struct file，不會建立第二個 lease owner。 */
static int toy_open(struct inode *inode, struct file *file)
{
    struct miscdevice *misc = file->private_data;
    struct toy_pf *pf = container_of(misc, struct toy_pf, misc);
    struct toy_file *ctx;
    int ret = 0;
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    ctx->pf = pf;
    spin_lock_init(&ctx->event_lock);
    init_waitqueue_head(&ctx->event_wait);
    mutex_init(&ctx->read_mutex);
    mutex_lock(&pf->admin_mutex);
    if (!pf->accepting || pf->dying)
        ret = -ENODEV;
    else {
        kref_get(&pf->ref);
        list_add_tail(&ctx->link, &pf->files);
        file->private_data = ctx;
        nonseekable_open(inode, file);
    }
    mutex_unlock(&pf->admin_mutex);
    if (ret)
        kfree(ctx);
    return ret;
}

/* 控制 fd 關閉不代表 VFIO owner 已退出；遺留 lease 標為 orphaned，不能直接當作已釋放。 */
static int toy_release_file(struct inode *inode, struct file *file)
{
    struct toy_file *ctx = file->private_data;
    struct toy_pf *pf = ctx->pf;
    unsigned int i;
    mutex_lock(&pf->admin_mutex);
    list_del(&ctx->link);
    for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i)
        if (pf->leases[i].owner == ctx) {
            if (!pf->dying && toy_fence(pf, i))
                toy_kill(pf, i);
            pf->leases[i].owner = NULL;
            pf->leases[i].orphaned = true;
            pf->leases[i].fence_valid = false;
            pr_err("TOY_LEASE_ORPHAN vf=%u token=%llu\n", i, pf->leases[i].token);
        }
    mutex_unlock(&pf->admin_mutex);
    kfree(ctx);
    kref_put(&pf->ref, toy_release_ref);
    return 0;
}

static void toy_ctl_snapshot(struct toy_pf *pf, struct toy_file *ctx,
                              struct toy_vf_ctl *ctl)
{
    struct toy_vf_info snapshot;
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    ctl->session = toy_read64(pf, TOY_PF_SESSION_LO);
    if (ctl->vf == TOY_CTL_GLOBAL)
        return;
    toy_snapshot_vf(pf, ctx, ctl->vf, &snapshot);
    ctl->epoch = snapshot.epoch;
    ctl->lease_token = snapshot.lease_token;
    ctl->ready_gen = snapshot.ready_gen;
    ctl->state = snapshot.state;
    ctl->hw_flags = snapshot.hw_flags;
    ctl->outstanding = snapshot.outstanding;
}

static int toy_ctl_shape(unsigned int cmd, const struct toy_vf_ctl *ctl)
{
    int ret = toy_header_valid(&ctl->hdr, sizeof(*ctl));
    if (ret)
        return ret;
    if (!ctl->operation || !toy_reserved_zero(ctl->reserved, 3) || ctl->state ||
        ctl->hw_flags || ctl->outstanding || ctl->fw_status ||
        (cmd != TOY_REARM_VF && ctl->ready_gen))
        return -EINVAL;
    if (ctl->flags != (cmd == TOY_LEASE_RELEASE ? TOY_CTL_OWNER_CLOSED : 0))
        return -EINVAL;
    if (cmd == TOY_GLOBAL_RECOVER)
        return ctl->vf == TOY_CTL_GLOBAL && !ctl->epoch && !ctl->lease_token ? 0 : -EINVAL;
    if (ctl->vf >= TOY_PCI_TOTAL_VFS || (cmd == TOY_LEASE_ACQUIRE && ctl->lease_token))
        return -EINVAL;
    return 0;
}

static int toy_global_recover(struct toy_pf *pf, u32 *fw_status)
{
    const u32 args[4] = { TOY_PROTOCOL_MAJOR, TOY_PROTOCOL_MINOR, TOY_PROTOCOL_MINOR, 0 };
    struct toy_vf_info before[TOY_PCI_TOTAL_VFS], after;
    u32 reply[4];
    unsigned int i;
    int ret;
    if (toy_has_leases(pf) || pf->enabled || pci_num_vf(pf->pdev))
        return -EBUSY;
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i)
        toy_snapshot_vf(pf, NULL, i, &before[i]);
    toy_fence_all(pf);
    toy_write(pf, TOY_PF_CONTROL, TOY_PF_CONTROL_GLOBAL_RESET);
    *fw_status = toy_read(pf, TOY_PF_HW_RESULT);
    ret = toy_status_errno(*fw_status);
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    if (ret)
        return ret;
    for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i) {
        toy_snapshot_vf(pf, NULL, i, &after);
        if (after.session <= before[i].session || after.epoch <= before[i].epoch ||
            after.state != TOY_STATES_FREE || after.reserve || after.outstanding ||
            after.hw_flags != TOY_HW_FLAGS_FENCED) {
            toy_fence_all(pf);
            return -EIO;
        }
        pf->observed[i] = after;
        pf->observed_fault[i] = 0;
    }
    /* 新 session 的 mailbox seq 可重起；host lease/fence/operation 都不歸零。 */
    pf->seq = 0;
    ret = toy_mailbox_status(pf, TOY_OPCODES_VERSION_NEG, TOY_GLOBAL, args, reply, fw_status);
    return !ret && reply[0] != TOY_ABI_VERSION ? -EPROTO : ret;
}

static int toy_execute_ctl(struct toy_file *ctx, unsigned int cmd,
                           const struct toy_vf_ctl *input, struct toy_vf_ctl *result)
{
    struct toy_pf *pf = ctx->pf;
    struct toy_lease *lease;
    struct toy_vf_info now;
    u32 args[4];
    int ret;
    *result = *input;
    toy_ctl_snapshot(pf, ctx, result);
    if (pf->transaction)
        return -EBUSY;
    if (input->session != result->session || input->epoch != result->epoch)
        return -ESTALE;
    if (cmd == TOY_GLOBAL_RECOVER) {
        ret = toy_global_recover(pf, &result->fw_status);
        toy_ctl_snapshot(pf, ctx, result);
        return ret;
    }
    lease = &pf->leases[input->vf];
    if (cmd == TOY_LEASE_ACQUIRE) {
        if (lease->token || input->vf >= pf->enabled ||
            (result->hw_flags & (TOY_HW_FLAGS_FENCED | TOY_HW_FLAGS_PCI_PRESENT |
                                TOY_HW_FLAGS_KILLED)) !=
                (TOY_HW_FLAGS_FENCED | TOY_HW_FLAGS_PCI_PRESENT) ||
            result->state == TOY_STATES_FREE || result->state == TOY_STATES_FAILED)
            return -EBUSY;
        if (pf->next_lease == U64_MAX) {
            toy_kill(pf, input->vf);
            return -EOVERFLOW;
        }
        lease->owner = ctx;
        lease->token = ++pf->next_lease;
        lease->orphaned = false;
        lease->fence_valid = false;
        result->lease_token = lease->token;
        toy_snapshot_vf(pf, NULL, input->vf, &pf->observed[input->vf]);
        pf->observed_fault[input->vf] = toy_read(pf,
            TOY_VF_VIEW(input->vf, TOY_PF_VF_VIEW_LAST_FAULT));
        return 0;
    }
    if (lease->owner != ctx || !input->lease_token || input->lease_token != lease->token)
        return -EPERM;
    switch (cmd) {
    case TOY_LEASE_RELEASE:
        if (!(result->hw_flags & TOY_HW_FLAGS_FENCED) || result->outstanding)
            return -EBUSY;
        memset(lease, 0, sizeof(*lease));
        result->lease_token = 0;
        return 0;
    case TOY_FENCE_VF:
        lease->fence_valid = false;
        ret = toy_fence_status(pf, input->vf, &result->fw_status);
        toy_ctl_snapshot(pf, ctx, result);
        if (!ret && (result->epoch <= input->epoch || result->session != input->session))
            ret = -EIO;
        if (!ret) {
            lease->fence_valid = true;
            lease->fence_epoch = result->epoch;
            lease->fence_session = result->session;
        }
        break;
    case TOY_RESET_VF:
        if (!lease->fence_valid || lease->fence_epoch != input->epoch ||
            lease->fence_session != input->session)
            return -ESTALE;
        if (result->state != TOY_STATES_ACTIVE || !(result->hw_flags & TOY_HW_FLAGS_FENCED))
            return -EBUSY;
        /* 即使 ACK 遺失也消耗 fence；重送由原請求 cache 處理。 */
        lease->fence_valid = false;
        ret = toy_mailbox_status(pf, TOY_OPCODES_QUIESCE, input->vf, NULL, NULL,
                                 &result->fw_status);
        if (!ret)
            ret = toy_mailbox_status(pf, TOY_OPCODES_RESET, input->vf, NULL, NULL,
                                     &result->fw_status);
        if (ret)
            toy_kill(pf, input->vf);
        toy_ctl_snapshot(pf, ctx, result);
        if (!ret && (result->state != TOY_STATES_RECOVERING || result->outstanding ||
            !(result->hw_flags & TOY_HW_FLAGS_FENCED) ||
            (result->hw_flags & (TOY_HW_FLAGS_CONFIG_VALID | TOY_HW_FLAGS_KILLED)) ||
            result->session != input->session || result->epoch != input->epoch))
            ret = -EIO;
        toy_snapshot_vf(pf, NULL, input->vf, &now);
        toy_publish(pf, ret ? TOY_EVENT_FAULT : TOY_EVENT_RESET, input->vf, -ret, &now);
        break;
    case TOY_REARM_VF:
        if (!input->ready_gen || input->ready_gen != result->ready_gen)
            return -ESTALE;
        if (result->outstanding ||
            (result->state != TOY_STATES_ACTIVE && result->state != TOY_STATES_RECOVERING) ||
            (result->hw_flags & (TOY_HW_FLAGS_READY_PENDING | TOY_HW_FLAGS_CONFIG_VALID |
             TOY_HW_FLAGS_BME | TOY_HW_FLAGS_FENCED | TOY_HW_FLAGS_KILLED)) !=
            (TOY_HW_FLAGS_READY_PENDING | TOY_HW_FLAGS_CONFIG_VALID |
             TOY_HW_FLAGS_BME | TOY_HW_FLAGS_FENCED))
            return -EBUSY;
        args[0] = input->epoch;
        args[1] = input->epoch >> 32;
        args[2] = input->ready_gen;
        args[3] = 0;
        ret = toy_mailbox_status(pf, TOY_OPCODES_REARM, input->vf, args, NULL,
                                 &result->fw_status);
        toy_ctl_snapshot(pf, ctx, result);
        if (!ret && (result->state != TOY_STATES_ACTIVE ||
            (result->hw_flags & (TOY_HW_FLAGS_FENCED | TOY_HW_FLAGS_KILLED)) ||
            !(result->hw_flags & TOY_HW_FLAGS_CONFIG_VALID) ||
            result->epoch != input->epoch || result->session != input->session)) {
            toy_kill(pf, input->vf);
            toy_ctl_snapshot(pf, ctx, result);
            ret = -EIO;
        }
        lease->fence_valid = false;
        break;
    default:
        return -ENOTTY;
    }
    toy_snapshot_vf(pf, NULL, input->vf, &pf->observed[input->vf]);
    return ret;
}

/* 先比對 operation/request 的重送，再執行 live-state 檢查；重試可取回已產生副作用的結果。 */
static long toy_control_ioctl(struct toy_file *ctx, unsigned int cmd, void __user *user)
{
    struct toy_vf_ctl input;
    int ret;
    if (copy_from_user(&input, user, sizeof(input)))
        return -EFAULT;
    ret = toy_ctl_shape(cmd, &input);
    if (ret)
        return ret;
    if (ctx->replay_valid && input.operation <= ctx->request.operation) {
        if (input.operation < ctx->request.operation)
            return -ESTALE;
        if (cmd != ctx->replay_cmd || memcmp(&input, &ctx->request, sizeof(input)))
            return -EPROTO;
    } else {
        ctx->request = input;
        ctx->replay_cmd = cmd;
        ctx->result_errno = toy_execute_ctl(ctx, cmd, &input, &ctx->result);
        ctx->replay_valid = true;
        pr_info("TOY_CTL operation=%llu cmd=%u vf=%u errno=%d fw_status=%u session=%llu epoch=%llu flags=%#x\n",
            input.operation, _IOC_NR(cmd), input.vf, ctx->result_errno,
            ctx->result.fw_status, ctx->result.session, ctx->result.epoch, ctx->result.hw_flags);
    }
    /* 負 errno 也複製完整結果；EFAULT 不可取消已執行操作或清掉 cache。 */
    if (copy_to_user(user, &ctx->result, sizeof(ctx->result)))
        return -EFAULT;
    return ctx->result_errno;
}

static long toy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct toy_file *ctx = file->private_data;
    struct toy_pf *pf = ctx->pf;
    void __user *user = (void __user *)arg;
    struct toy_info info;
    struct toy_vf_info vf;
    unsigned int index;
    int ret = 0;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying) {
        ret = -ENODEV;
        goto out;
    }
    switch (cmd) {
    case TOY_GET_INFO:
        if (copy_from_user(&info, user, sizeof(info))) { ret = -EFAULT; break; }
        ret = toy_header_valid(&info.hdr, sizeof(info));
        if (ret || !toy_reserved_zero(info.reserved, 4)) { ret = ret ?: -EINVAL; break; }
        memset(&info, 0, sizeof(info));
        info.hdr = toy_header(sizeof(info));
        toy_write(pf, TOY_PF_SNAPSHOT, 1);
        info.session = toy_read64(pf, TOY_PF_SESSION_LO);
        info.caps_vfs = toy_read(pf, TOY_PF_CAP_VFS);
        info.caps_queues = toy_read(pf, TOY_PF_CAP_QUEUES);
        info.caps_vectors = toy_read(pf, TOY_PF_CAP_VECTORS);
        info.caps_credits = toy_read(pf, TOY_PF_CAP_CREDIT);
        info.features = toy_read(pf, TOY_PF_CAP_FEATURES);
        info.enabled_vfs = pf->enabled;
        if (copy_to_user(user, &info, sizeof(info))) ret = -EFAULT;
        break;
    case TOY_GET_VF:
        if (copy_from_user(&vf, user, sizeof(vf))) { ret = -EFAULT; break; }
        ret = toy_header_valid(&vf.hdr, sizeof(vf));
        if (ret || !toy_reserved_zero(vf.reserved, 4) || vf.vf >= TOY_PCI_TOTAL_VFS) {
            ret = ret ?: -EINVAL; break;
        }
        index = vf.vf;
        toy_write(pf, TOY_PF_SNAPSHOT, 1);
        toy_snapshot_vf(pf, ctx, index, &vf);
        if (copy_to_user(user, &vf, sizeof(vf))) ret = -EFAULT;
        break;
    case TOY_LEASE_ACQUIRE: case TOY_LEASE_RELEASE: case TOY_FENCE_VF:
    case TOY_RESET_VF: case TOY_REARM_VF: case TOY_GLOBAL_RECOVER:
        ret = toy_control_ioctl(ctx, cmd, user);
        break;
    default:
        ret = -ENOTTY;
    }
out:
    mutex_unlock(&pf->admin_mutex);
    return ret;
}

static bool toy_event_available(struct toy_file *ctx)
{
    return READ_ONCE(ctx->stash_valid) || READ_ONCE(ctx->lost) || READ_ONCE(ctx->count);
}

/* read_mutex 序列化讀者；event_lock 只保護 queue/stash，copy_to_user 必須在 spinlock 外執行。 */
static ssize_t toy_read_file(struct file *file, char __user *buffer,
                             size_t length, loff_t *offset)
{
    struct toy_file *ctx = file->private_data;
    unsigned long flags;
    int ret;
    if (READ_ONCE(ctx->pf->dying))
        return -ENODEV;
    if (length < sizeof(struct toy_event))
        return -EINVAL;
    ret = mutex_lock_interruptible(&ctx->read_mutex);
    if (ret)
        return ret;
    while (!toy_event_available(ctx)) {
        if (READ_ONCE(ctx->pf->dying)) { ret = -ENODEV; goto out; }
        if (file->f_flags & O_NONBLOCK) { ret = -EAGAIN; goto out; }
        ret = wait_event_interruptible(ctx->event_wait,
            READ_ONCE(ctx->pf->dying) || toy_event_available(ctx));
        if (ret)
            goto out;
    }
    if (READ_ONCE(ctx->pf->dying)) { ret = -ENODEV; goto out; }
    spin_lock_irqsave(&ctx->event_lock, flags);
    if (!ctx->stash_valid) {
        if (ctx->event_seq == U64_MAX) {
            spin_unlock_irqrestore(&ctx->event_lock, flags);
            ret = -EOVERFLOW;
            goto out;
        }
        if (ctx->lost) {
            memset(&ctx->stash, 0, sizeof(ctx->stash));
            ctx->stash.hdr = toy_header(sizeof(ctx->stash));
            ctx->stash.type = TOY_EVENT_LOST_EVENTS;
            ctx->stash.vf = TOY_CTL_GLOBAL;
            ctx->stash.detail = ctx->lost;
            ctx->lost = 0;
        } else {
            ctx->stash = ctx->events[ctx->head];
            ctx->head = (ctx->head + 1) % TOY_EVENT_CAPACITY;
            --ctx->count;
        }
        ctx->stash.event_seq = ++ctx->event_seq;
        ctx->stash_valid = true;
    }
    spin_unlock_irqrestore(&ctx->event_lock, flags);
    ret = copy_to_user(buffer, &ctx->stash, sizeof(ctx->stash)) ? -EFAULT : sizeof(ctx->stash);
    if (ret > 0) {
        spin_lock_irqsave(&ctx->event_lock, flags);
        ctx->stash_valid = false;
        spin_unlock_irqrestore(&ctx->event_lock, flags);
    }
    if (READ_ONCE(ctx->pf->dying))
        ret = -ENODEV;
out:
    mutex_unlock(&ctx->read_mutex);
    return ret;
}

static __poll_t toy_poll(struct file *file, poll_table *wait)
{
    struct toy_file *ctx = file->private_data;
    poll_wait(file, &ctx->event_wait, wait);
    if (READ_ONCE(ctx->pf->dying))
        return EPOLLHUP | EPOLLERR;
    return toy_event_available(ctx) ? EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations toy_fops = {
    .owner = THIS_MODULE, .open = toy_open, .release = toy_release_file,
    .unlocked_ioctl = toy_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = compat_ptr_ioctl,
#endif
    .read = toy_read_file, .poll = toy_poll,
};
