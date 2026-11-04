// SPDX-License-Identifier: GPL-2.0
/* PF 只協調 host transaction；VF FSM 與 ledger 唯一由裝置持有。 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/compat.h>
#include <linux/poll.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include "toy_regs.h"
#include "toy_uapi.h"

#define TOY_MBX_TIMEOUT_MS 5000
#define TOY_PROTOCOL_MAJOR (TOY_ABI_VERSION >> 16)
#define TOY_PROTOCOL_MINOR (TOY_ABI_VERSION & 0xffffu)

struct toy_file;
struct toy_lease {
    struct toy_file *owner;
    u64 token;
    u64 fence_epoch;
    u64 fence_session;
    bool fence_valid;
    bool orphaned;
};

struct toy_failpoint {
    char name[64];
    u32 hit;
    u32 seen;
    bool fired;
};

struct toy_file {
    struct toy_pf *pf;
    struct list_head link;
    spinlock_t event_lock;
    wait_queue_head_t event_wait;
    struct mutex read_mutex;
    struct toy_event events[TOY_EVENT_CAPACITY];
    struct toy_event stash;
    unsigned int head, count;
    u32 lost;
    u64 event_seq;
    bool stash_valid;
    bool replay_valid;
    unsigned int replay_cmd;
    struct toy_vf_ctl request, result;
    int result_errno;
};

struct toy_pf {
    struct pci_dev *pdev;
    void __iomem *bar;
    struct mutex admin_mutex;
    struct mutex mbx_mutex;
    struct completion mbx_done;
    struct completion transaction_done;
    struct kref ref;
    struct miscdevice misc;
    bool accepting;
    bool dying;
    unsigned int irq_count;
    u32 seq;
    u32 fence_token;
    unsigned int enabled;
    bool transaction;
    u64 next_lease;
    struct toy_lease leases[TOY_PCI_TOTAL_VFS];
    struct toy_vf_info observed[TOY_PCI_TOTAL_VFS];
    u32 observed_fault[TOY_PCI_TOTAL_VFS];
    struct list_head files;
    struct work_struct notify_work;
    atomic_t pending_events;
    struct dentry *debugfs;
    struct toy_failpoint failpoints[2];
};

static char forward_failpoint[64];
static char unwind_failpoint[64];
static uint forward_hit = 1, unwind_hit = 1;
static atomic_t forward_seen = ATOMIC_INIT(0), unwind_seen = ATOMIC_INIT(0);
module_param_string(forward_failpoint, forward_failpoint, sizeof(forward_failpoint), 0400);
module_param_string(unwind_failpoint, unwind_failpoint, sizeof(unwind_failpoint), 0400);
module_param(forward_hit, uint, 0400);
module_param(unwind_hit, uint, 0400);

static const char *const toy_fail_names[] = {
    "probe.alloc_ctx", "probe.enable_mem", "probe.request_regions", "probe.map_bar",
    "probe.validate_caps", "probe.alloc_vectors", "probe.request_irq0", "probe.request_irq1",
    "probe.version_neg", "probe.misc_register", "provision.host_record",
    "provision.pci_enable", "provision.after_pci_enable", "unwind.destroy",
};

static bool toy_fail_name_valid(const char *name)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(toy_fail_names); ++i)
        if (!strcmp(name, toy_fail_names[i]))
            return true;
    return false;
}

/* 所有呼叫點皆由 probe 序列化或持有 admin_mutex；雙槽獨立計次。 */
static bool toy_fail(struct toy_pf *pf, const char *name)
{
    unsigned int i;
    bool hit = false;
    if (!strcmp(name, forward_failpoint) && atomic_inc_return(&forward_seen) == forward_hit)
        hit = true;
    if (!strcmp(name, unwind_failpoint) && atomic_inc_return(&unwind_seen) == unwind_hit)
        hit = true;
    if (pf)
        for (i = 0; i < ARRAY_SIZE(pf->failpoints); ++i) {
            struct toy_failpoint *fp = &pf->failpoints[i];
            if (!fp->fired && !strcmp(fp->name, name) && ++fp->seen == fp->hit) {
                fp->fired = true;
                hit = true;
            }
        }
    if (hit)
        pr_info("TOY_FAILPOINT_HIT name=%s\n", name);
    return hit;
}

static int toy_status_errno(u32 status)
{
    switch (status) {
    case TOY_STATUSES_OK: return 0;
    case TOY_STATUSES_INVAL: return -EINVAL;
    case TOY_STATUSES_NOSPC: return -ENOSPC;
    case TOY_STATUSES_STATE: return -EBUSY;
    case TOY_STATUSES_FENCED: return -EACCES;
    case TOY_STATUSES_DMA: return -EIO;
    case TOY_STATUSES_TIMEOUT: return -ETIMEDOUT;
    case TOY_STATUSES_UNSUP: return -EOPNOTSUPP;
    case TOY_STATUSES_NO_NEG: return -EPROTO;
    case TOY_STATUSES_SEQ_CONFLICT: return -EPROTO;
    case TOY_STATUSES_STALE_SEQ: return -ESTALE;
    case TOY_STATUSES_BUSY: return -EBUSY;
    case TOY_STATUSES_OVERFLOW: return -EOVERFLOW;
    case TOY_STATUSES_BAD_DESC: return -EINVAL;
    case TOY_STATUSES_BAD_QUEUE: return -EINVAL;
    case TOY_STATUSES_IO: return -EIO;
    default: return -EPROTO;
    }
}

static u32 toy_read(struct toy_pf *pf, u32 reg)
{
    return ioread32(pf->bar + reg);
}

static void toy_write(struct toy_pf *pf, u32 reg, u32 value)
{
    iowrite32(value, pf->bar + reg);
}

static u64 toy_read64(struct toy_pf *pf, u32 low)
{
    /* 呼叫者必須已在 admin_mutex 下完成 SNAPSHOT。 */
    u32 lo = toy_read(pf, low);
    u32 hi = toy_read(pf, low + 4);
    return ((u64)hi << 32) | lo;
}

static void toy_fence_all(struct toy_pf *pf)
{
    unsigned int vf;
    toy_write(pf, TOY_PF_CONTROL, TOY_PF_CONTROL_FENCE_ALL);
    (void)toy_read(pf, TOY_PF_HW_RESULT);
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    for (vf = 0; vf < TOY_PCI_TOTAL_VFS; ++vf) {
        u32 flags = toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_HW_STATUS));
        if (!(flags & TOY_HW_FLAGS_FENCED))
            dev_err(&pf->pdev->dev, "FENCE_ALL_READBACK vf=%u flags=%#x\n", vf, flags);
    }
}

static void toy_kill(struct toy_pf *pf, unsigned int vf);

static int toy_fence_status(struct toy_pf *pf, unsigned int vf, u32 *fw_status)
{
    u32 result, flags;
    if (pf->fence_token == U32_MAX) {
        toy_kill(pf, vf);
        *fw_status = TOY_STATUSES_OVERFLOW;
        return -EOVERFLOW;
    }
    toy_write(pf, TOY_PF_FENCE_TOKEN, ++pf->fence_token);
    toy_write(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_HW_CTL), TOY_VF_HW_CONTROL_FENCE_BEGIN);
    result = toy_read(pf, TOY_PF_HW_RESULT);
    *fw_status = result;
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    flags = toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_HW_STATUS));
    if (result)
        return toy_status_errno(result);
    return flags & TOY_HW_FLAGS_FENCED ? 0 : -EIO;
}

static int toy_fence(struct toy_pf *pf, unsigned int vf)
{
    u32 status;
    return toy_fence_status(pf, vf, &status);
}

static void toy_kill(struct toy_pf *pf, unsigned int vf)
{
    toy_write(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_HW_CTL), TOY_VF_HW_CONTROL_KILL);
    (void)toy_read(pf, TOY_PF_HW_RESULT);
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    if (!(toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_HW_STATUS)) & TOY_HW_FLAGS_KILLED))
        dev_err(&pf->pdev->dev, "KILL_READBACK_FAILED vf=%u\n", vf);
}

/* 呼叫者持有 admin_mutex，再取得 mbx_mutex。IRQ 只完成通知，不拿這兩把 mutex。 */
static int toy_mailbox_status(struct toy_pf *pf, u32 opcode, u32 target,
                             const u32 args[4], u32 reply[4], u32 *fw_status)
{
    unsigned long deadline;
    u32 seq, status;
    unsigned int i, retries = 0;
    int ret = -ETIMEDOUT;
    /* IRQ completion 不取得 admin/mbx 鎖，poll 是掉 IRQ 時的必要後備。 */
    mutex_lock(&pf->mbx_mutex);
    *fw_status = TOY_STATUSES_TIMEOUT;
    if (pf->seq == U32_MAX) {
        ret = -EOVERFLOW;
        *fw_status = TOY_STATUSES_OVERFLOW;
        toy_fence_all(pf);
        goto out;
    }
    status = toy_read(pf, TOY_PF_MBX_STATUS);
    if (status == TOY_MBX_STATUS_BUSY) {
        ret = -EBUSY;
        *fw_status = TOY_STATUSES_BUSY;
        goto out;
    }
    if (status == TOY_MBX_STATUS_DONE)
        toy_write(pf, TOY_PF_MBX_ACK, 1);
    seq = ++pf->seq;
    reinit_completion(&pf->mbx_done);
    toy_write(pf, TOY_PF_MBX_SEQ, seq);
    toy_write(pf, TOY_PF_MBX_OPCODE, opcode);
    toy_write(pf, TOY_PF_MBX_TARGET, target);
    for (i = 0; i < 4; ++i)
        toy_write(pf, TOY_PF_MBX_ARG0 + 4 * i, args ? args[i] : 0);
    toy_write(pf, TOY_PF_MBX_SUBMIT, 1);
    deadline = jiffies + msecs_to_jiffies(TOY_MBX_TIMEOUT_MS);
    do {
        status = toy_read(pf, TOY_PF_MBX_STATUS);
        if (status == TOY_MBX_STATUS_DONE && toy_read(pf, TOY_PF_MBX_ACK_SEQ) == seq) {
            u32 fw_result = toy_read(pf, TOY_PF_MBX_RESULT);
            *fw_status = fw_result;
            if (reply)
                for (i = 0; i < 4; ++i)
                    reply[i] = toy_read(pf, TOY_PF_MBX_REPLY0 + 4 * i);
            toy_write(pf, TOY_PF_MBX_ACK, 1);
            ret = toy_status_errno(fw_result);
            break;
        }
        /* ACK 遺失且裝置已回 IDLE 時，重送完全相同的 request，不重做 fence。 */
        if (status == TOY_MBX_STATUS_IDLE && retries < 2) {
            ++retries;
            toy_write(pf, TOY_PF_MBX_SUBMIT, 1);
            dev_info(&pf->pdev->dev, "MAILBOX_RETRY seq=%u attempt=%u\n", seq, retries);
        }
        wait_for_completion_timeout(&pf->mbx_done, msecs_to_jiffies(1));
    } while (time_before(jiffies, deadline));
    if (ret)
        dev_err(&pf->pdev->dev, "MAILBOX_ERROR seq=%u opcode=%u target=%u errno=%d\n",
                seq, opcode, target, ret);
out:
    mutex_unlock(&pf->mbx_mutex);
    return ret;
}

static int toy_mailbox(struct toy_pf *pf, u32 opcode, u32 target,
                       const u32 args[4], u32 reply[4])
{
    u32 status;
    return toy_mailbox_status(pf, opcode, target, args, reply, &status);
}

/* 中斷脈絡只 ACK、喚醒 mailbox 等待者及排 work；需要管理鎖的通知整理留給 workqueue。 */
static irqreturn_t toy_irq(int irq, void *data)
{
    struct toy_pf *pf = data;
    u32 events = toy_read(pf, TOY_PF_EVENT);
    if (!events)
        return IRQ_NONE;
    toy_write(pf, TOY_PF_EVENT, events);
    if (events & TOY_PF_EVENTS_MBX_DONE)
        complete(&pf->mbx_done);
    if (events & (TOY_PF_EVENTS_FLR_EVENT | TOY_PF_EVENTS_VF_HEALTH)) {
        atomic_or(events, &pf->pending_events);
        if (!READ_ONCE(pf->dying))
            schedule_work(&pf->notify_work);
    }
    return IRQ_HANDLED;
}

static int toy_verify_free(struct toy_pf *pf, unsigned int vf)
{
    u32 offset = TOY_VF_VIEW(vf, 0);
    u32 state, reserve, outstanding, flags;
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    state = toy_read(pf, offset + TOY_PF_VF_VIEW_STATE);
    reserve = toy_read(pf, offset + TOY_PF_VF_VIEW_RESERVE);
    outstanding = toy_read(pf, offset + TOY_PF_VF_VIEW_OUTSTANDING);
    flags = toy_read(pf, offset + TOY_PF_VF_VIEW_HW_STATUS);
    if (state != TOY_STATES_FREE || reserve || outstanding ||
        (flags & (TOY_HW_FLAGS_CONFIG_VALID | TOY_HW_FLAGS_PCI_PRESENT)) ||
        !(flags & TOY_HW_FLAGS_FENCED)) {
        dev_err(&pf->pdev->dev,
                "FREE_VERIFY_FAILED vf=%u state=%u reserve=%u outstanding=%u flags=%#x\n",
                vf, state, reserve, outstanding, flags);
        return -EIO;
    }
    dev_info(&pf->pdev->dev, "FREE_VERIFIED vf=%u reserve=0 outstanding=0 flags=%#x\n", vf, flags);
    return 0;
}

static int toy_destroy_slot(struct toy_pf *pf, unsigned int vf)
{
    int ret = toy_fence(pf, vf);
    if (!ret && toy_fail(pf, "unwind.destroy"))
        ret = -EIO;
    if (!ret)
        ret = toy_mailbox(pf, TOY_OPCODES_DESTROY, vf, NULL, NULL);
    if (!ret)
        ret = toy_verify_free(pf, vf);
    if (ret) {
        unsigned long deadline = jiffies + msecs_to_jiffies(TOY_MBX_TIMEOUT_MS);
        toy_kill(pf, vf);
        /* firmware 存活時會觀察 KILL 並以合法事件收斂；逾時仍保留配額。 */
        do {
            toy_write(pf, TOY_PF_SNAPSHOT, 1);
            if (toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_STATE)) == TOY_STATES_FAILED)
                break;
            usleep_range(1000, 2000);
        } while (time_before(jiffies, deadline));
        dev_err(&pf->pdev->dev, "DESTROY_RETAINED vf=%u errno=%d\n", vf, ret);
    }
    return ret;
}

static int toy_sriov_configure(struct pci_dev *pdev, int count)
{
    struct toy_pf *pf = pci_get_drvdata(pdev);
    const u32 args[4] = { 8, 0, 0, 0 };
    unsigned int i, provisioned = 0;
    int ret = 0, cleanup_ret;
    if (!pf || count < 0 || count > TOY_PCI_TOTAL_VFS)
        return -EINVAL;
    mutex_lock(&pf->admin_mutex);
    if (!pf->accepting || pf->dying) {
        ret = -ENODEV;
        goto out;
    }
    if (pf->transaction) {
        ret = -EBUSY;
        goto out;
    }
    if (count && pf->enabled) {
        ret = count == pf->enabled ? count : -EBUSY;
        goto out;
    }
    if (!count) {
        for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i)
            if (pf->leases[i].token) {
                ret = -EBUSY;
                goto out;
            }
        if (pci_vfs_assigned(pdev)) {
            ret = -EBUSY;
            goto out;
        }
        for (i = 0; i < pf->enabled; ++i) {
            ret = toy_fence(pf, i);
            if (ret)
                goto out;
        }
        pf->transaction = true;
        reinit_completion(&pf->transaction_done);
        /* PCI/VFIO 可能等待 owner；不能持有 worker 或控制 fd 需要的鎖。 */
        mutex_unlock(&pf->admin_mutex);
        pci_disable_sriov(pdev);
        mutex_lock(&pf->admin_mutex);
        if (pf->dying) {
            ret = -ENODEV;
            goto transaction_done;
        }
        for (i = pf->enabled; i > 0; --i) {
            cleanup_ret = toy_destroy_slot(pf, i - 1);
            if (cleanup_ret && !ret)
                ret = cleanup_ret;
        }
        pf->enabled = 0;
        goto transaction_done;
    }
    pf->transaction = true;
    reinit_completion(&pf->transaction_done);
    for (i = 0; i < count; ++i) {
        ret = toy_mailbox(pf, TOY_OPCODES_PROVISION, i, args, NULL);
        if (ret)
            goto unwind;
        ++provisioned;
        if (toy_fail(pf, "provision.host_record")) {
            ret = -ENOMEM;
            goto unwind;
        }
    }
    if (toy_fail(pf, "provision.pci_enable")) {
        ret = -EIO;
        goto unwind;
    }
    mutex_unlock(&pf->admin_mutex);
    ret = pci_enable_sriov(pdev, count);
    mutex_lock(&pf->admin_mutex);
    if (ret)
        goto unwind;
    if (pf->dying || toy_fail(pf, "provision.after_pci_enable")) {
        ret = pf->dying ? -ENODEV : -EIO;
        mutex_unlock(&pf->admin_mutex);
        pci_disable_sriov(pdev);
        mutex_lock(&pf->admin_mutex);
        goto unwind;
    }
    pf->enabled = count;
    ret = count;
    goto transaction_done;
unwind:
    /* PCI core 的失敗返回已完成枚舉 unwind；ledger 依反向次序撤銷。 */
    while (provisioned) {
        cleanup_ret = toy_destroy_slot(pf, --provisioned);
        if (cleanup_ret)
            dev_err(&pdev->dev, "PROVISION_UNWIND_RETAINED vf=%u errno=%d\n",
                    provisioned, cleanup_ret);
    }
transaction_done:
    pf->transaction = false;
    complete_all(&pf->transaction_done);
out:
    mutex_unlock(&pf->admin_mutex);
    return ret;
}

static void toy_release_ref(struct kref *ref)
{
    struct toy_pf *pf = container_of(ref, struct toy_pf, ref);
    kfree(pf);
}

#include "toy_control.inc"

/* inode 只保存 PF；已開啟的 debugfs fd 以 kref 跨越 remove。 */
static int toy_debug_open(struct inode *inode, struct file *file)
{
    struct toy_pf *pf = inode->i_private;
    int ret = 0;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying)
        ret = -ENODEV;
    else {
        kref_get(&pf->ref);
        file->private_data = pf;
    }
    mutex_unlock(&pf->admin_mutex);
    return ret;
}

static int toy_debug_release(struct inode *inode, struct file *file)
{
    kref_put(&((struct toy_pf *)file->private_data)->ref, toy_release_ref);
    return 0;
}

static unsigned int toy_debug_slot(struct file *file)
{
    return !strcmp(file->f_path.dentry->d_name.name, "unwind");
}

static ssize_t toy_debug_read(struct file *file, char __user *buffer,
                              size_t length, loff_t *offset)
{
    struct toy_pf *pf = file->private_data;
    struct toy_failpoint *fp = &pf->failpoints[toy_debug_slot(file)];
    char record[160];
    int size;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying) {
        mutex_unlock(&pf->admin_mutex);
        return -ENODEV;
    }
    size = scnprintf(record, sizeof(record), "name=%s hit=%u seen=%u fired=%u\n",
                      fp->name[0] ? fp->name : "off", fp->hit, fp->seen, fp->fired);
    mutex_unlock(&pf->admin_mutex);
    return simple_read_from_buffer(buffer, length, offset, record, size);
}

static ssize_t toy_debug_write(struct file *file, const char __user *buffer,
                               size_t length, loff_t *offset)
{
    struct toy_pf *pf = file->private_data;
    struct toy_failpoint *fp = &pf->failpoints[toy_debug_slot(file)];
    char record[96], name[64], trailing;
    unsigned int hit;
    int ret = length;
    if (!length || length >= sizeof(record))
        return -EINVAL;
    if (copy_from_user(record, buffer, length))
        return -EFAULT;
    if (memchr(record, 0, length))
        return -EINVAL;
    record[length] = 0;
    if (sscanf(record, "%63s %u %c", name, &hit, &trailing) != 2 || !hit ||
        (strcmp(name, "off") && !toy_fail_name_valid(name)))
        return -EINVAL;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying)
        ret = -ENODEV;
    else if (pf->transaction)
        ret = -EBUSY;
    else {
        memset(fp, 0, sizeof(*fp));
        if (strcmp(name, "off"))
            strscpy(fp->name, name, sizeof(fp->name));
        fp->hit = hit;
        dev_info(&pf->pdev->dev, "FAILPOINT_ARM slot=%u name=%s hit=%u\n",
                 toy_debug_slot(file), name, hit);
    }
    mutex_unlock(&pf->admin_mutex);
    return ret;
}

static const struct file_operations toy_debug_fops = {
    .owner = THIS_MODULE, .open = toy_debug_open, .release = toy_debug_release,
    .read = toy_debug_read, .write = toy_debug_write, .llseek = default_llseek,
};

/* 只讀生命週期觀測；不清空 queue 或等待 worker，避免讓量測改變結果。 */
static ssize_t toy_lifecycle_read(struct file *file, char __user *buffer,
                                  size_t length, loff_t *offset)
{
    struct toy_pf *pf = file->private_data;
    struct toy_file *ctx;
    unsigned int files = 0, leases = 0, orphaned = 0, events = 0, i;
    char record[256];
    int size;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying) {
        mutex_unlock(&pf->admin_mutex);
        return -ENODEV;
    }
    list_for_each_entry(ctx, &pf->files, link) {
        unsigned long flags;
        ++files;
        spin_lock_irqsave(&ctx->event_lock, flags);
        events += ctx->count + !!ctx->lost + ctx->stash_valid;
        spin_unlock_irqrestore(&ctx->event_lock, flags);
    }
    for (i = 0; i < TOY_PCI_TOTAL_VFS; ++i) {
        leases += !!pf->leases[i].token;
        orphaned += pf->leases[i].orphaned;
    }
    size = scnprintf(record, sizeof(record),
        "contexts=%u leases=%u orphaned=%u events=%u work_busy=%u pending_irqs=%u enabled=%u transaction=%u\n",
        files, leases, orphaned, events, work_busy(&pf->notify_work),
        (unsigned int)atomic_read(&pf->pending_events), pf->enabled, pf->transaction);
    mutex_unlock(&pf->admin_mutex);
    return simple_read_from_buffer(buffer, length, offset, record, size);
}

static const struct file_operations toy_lifecycle_fops = {
    .owner = THIS_MODULE, .open = toy_debug_open, .release = toy_debug_release,
    .read = toy_lifecycle_read, .llseek = default_llseek,
};

static ssize_t toy_stats_read(struct file *file, char __user *buffer,
                              size_t length, loff_t *offset)
{
    struct toy_pf *pf = file->private_data;
    char record[1536];
    int used = 0;
    unsigned int vf;
    mutex_lock(&pf->admin_mutex);
    if (pf->dying) {
        mutex_unlock(&pf->admin_mutex);
        return -ENODEV;
    }
    /* 同一個硬體 snapshot 提供所有 VF 的唯讀健康計數。 */
    toy_write(pf, TOY_PF_SNAPSHOT, 1);
    for (vf = 0; vf < TOY_PCI_TOTAL_VFS; ++vf)
        used += scnprintf(record + used, sizeof(record) - used,
            "vf=%u stale_desc=%u stale_cpl=%u dma_fault=%u fence_blocked=%u irq_suppressed=%u\n",
            vf, toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_STALE_DESC)),
            toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_STALE_CPL)),
            toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_DMA_FAULT)),
            toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_FENCE_BLOCKED)),
            toy_read(pf, TOY_VF_VIEW(vf, TOY_PF_VF_VIEW_IRQ_SUPPRESSED)));
    mutex_unlock(&pf->admin_mutex);
    return simple_read_from_buffer(buffer, length, offset, record, used);
}

static const struct file_operations toy_stats_fops = {
    .owner = THIS_MODULE, .open = toy_debug_open, .release = toy_debug_release,
    .read = toy_stats_read, .llseek = default_llseek,
};

/* 依序取得 PCI、BAR、IRQ 與控制節點；每個 goto 都只釋放此前已成功取得的資源。 */
static int toy_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct toy_pf *pf;
    const u32 version_args[4] = { TOY_PROTOCOL_MAJOR, TOY_PROTOCOL_MINOR, TOY_PROTOCOL_MINOR, 0 };
    u32 version_reply[4];
    int ret, i;
    if (!forward_hit || !unwind_hit ||
        (forward_failpoint[0] && !toy_fail_name_valid(forward_failpoint)) ||
        (unwind_failpoint[0] && !toy_fail_name_valid(unwind_failpoint)))
        return -EINVAL;
    if (toy_fail(NULL, "probe.alloc_ctx"))
        return -ENOMEM;
    pf = kzalloc(sizeof(*pf), GFP_KERNEL);
    if (!pf)
        return -ENOMEM;
    pf->pdev = pdev;
    mutex_init(&pf->admin_mutex);
    mutex_init(&pf->mbx_mutex);
    init_completion(&pf->mbx_done);
    init_completion(&pf->transaction_done);
    complete_all(&pf->transaction_done);
    INIT_LIST_HEAD(&pf->files);
    INIT_WORK(&pf->notify_work, toy_notify_work);
    atomic_set(&pf->pending_events, 0);
    pf->failpoints[0].hit = pf->failpoints[1].hit = 1;
    kref_init(&pf->ref);
    ret = toy_fail(pf, "probe.enable_mem") ? -EIO : pci_enable_device_mem(pdev);
    if (ret)
        goto free_context;
    ret = toy_fail(pf, "probe.request_regions") ? -EBUSY : pci_request_regions(pdev, "toy_pf");
    if (ret)
        goto disable_device;
    if (pci_resource_len(pdev, 0) != TOY_LIMITS_PF_BAR0_BYTES ||
        !(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
        ret = -ENODEV;
        goto release_regions;
    }
    if (!toy_fail(pf, "probe.map_bar"))
        pf->bar = pci_iomap(pdev, 0, 0);
    if (!pf->bar) {
        ret = -ENOMEM;
        goto release_regions;
    }
    if (toy_fail(pf, "probe.validate_caps") ||
        toy_read(pf, TOY_PF_MAGIC) != TOY_PF_MAGIC_VALUE ||
        toy_read(pf, TOY_PF_ABI) != TOY_ABI_VERSION ||
        toy_read(pf, TOY_PF_CAP_VFS) != TOY_PCI_TOTAL_VFS ||
        toy_read(pf, TOY_PF_CAP_QUEUES) != TOY_LIMITS_QUEUES ||
        toy_read(pf, TOY_PF_CAP_VECTORS) != TOY_LIMITS_VF_VECTORS ||
        toy_read(pf, TOY_PF_CAP_CREDIT) != TOY_LIMITS_CREDITS) {
        ret = -EPROTO;
        goto unmap;
    }
    /* 同一 machine 的 driver rebind 不可從零重用去重序號。 */
    pf->seq = toy_read(pf, TOY_PF_MBX_ACK_SEQ);
    pf->fence_token = toy_read(pf, TOY_PF_FENCE_TOKEN);
    ret = toy_fail(pf, "probe.alloc_vectors") ? -ENOMEM :
          pci_alloc_irq_vectors(pdev, 2, 2, PCI_IRQ_MSIX);
    if (ret < 0)
        goto unmap;
    for (i = 0; i < 2; ++i) {
        ret = toy_fail(pf, i ? "probe.request_irq1" : "probe.request_irq0") ? -EIO :
              request_irq(pci_irq_vector(pdev, i), toy_irq, 0, "toy_pf", pf);
        if (ret)
            goto free_irqs;
        ++pf->irq_count;
    }
    pci_set_master(pdev);
    pci_set_drvdata(pdev, pf);
    mutex_lock(&pf->admin_mutex);
    ret = toy_fail(pf, "probe.version_neg") ? -EIO :
          toy_mailbox(pf, TOY_OPCODES_VERSION_NEG, TOY_GLOBAL, version_args, version_reply);
    if (!ret && version_reply[0] != TOY_ABI_VERSION)
        ret = -EPROTO;
    mutex_unlock(&pf->admin_mutex);
    if (ret)
        goto clear_master;
    pf->misc.minor = MISC_DYNAMIC_MINOR;
    pf->misc.name = "toy_pf0";
    pf->misc.fops = &toy_fops;
    pf->misc.mode = 0600;
    pf->misc.parent = &pdev->dev;
    pf->accepting = true;
    ret = toy_fail(pf, "probe.misc_register") ? -EIO : misc_register(&pf->misc);
    if (ret)
        goto clear_master;
    pf->debugfs = debugfs_create_dir("toy_pf0", NULL);
    debugfs_create_file("forward", 0600, pf->debugfs, pf, &toy_debug_fops);
    debugfs_create_file("unwind", 0600, pf->debugfs, pf, &toy_debug_fops);
    debugfs_create_file("lifecycle", 0400, pf->debugfs, pf, &toy_lifecycle_fops);
    debugfs_create_file("vf-stats", 0400, pf->debugfs, pf, &toy_stats_fops);
    dev_info(&pdev->dev, "TOY_PF_READY abi=1.0 vectors=2 total_vfs=8\n");
    return 0;
clear_master:
    pf->accepting = false;
    toy_fence_all(pf);
    pci_clear_master(pdev);
    pci_set_drvdata(pdev, NULL);
free_irqs:
    WRITE_ONCE(pf->dying, true);
    while (pf->irq_count) {
        --pf->irq_count;
        free_irq(pci_irq_vector(pdev, pf->irq_count), pf);
    }
    pci_free_irq_vectors(pdev);
    cancel_work_sync(&pf->notify_work);
unmap:
    pci_iounmap(pdev, pf->bar);
release_regions:
    pci_release_regions(pdev);
disable_device:
    pci_disable_device(pdev);
free_context:
    kref_put(&pf->ref, toy_release_ref);
    return ret;
}

/* 先拒絕新操作並 fence，再同步 worker/IRQ 與解除 BAR；舊 fd 持有的 context 由 kref 延後釋放。 */
static void toy_remove(struct pci_dev *pdev)
{
    struct toy_pf *pf = pci_get_drvdata(pdev);
    unsigned int i;
    if (!pf)
        return;
    mutex_lock(&pf->admin_mutex);
    pf->accepting = false;
    WRITE_ONCE(pf->dying, true);
    toy_fence_all(pf);
    toy_publish(pf, TOY_EVENT_REMOVE, TOY_CTL_GLOBAL, 0, NULL);
    dev_info(&pdev->dev, "TOY_REMOVE_FENCED enabled=%u\n", pf->enabled);
    mutex_unlock(&pf->admin_mutex);
    misc_deregister(&pf->misc);
    debugfs_remove_recursive(pf->debugfs);
    cancel_work_sync(&pf->notify_work);
    /* PCI core 的 device lock 序列化 sriov_configure/remove；owner 自行關 fd。 */
    pci_disable_sriov(pdev);
    mutex_lock(&pf->admin_mutex);
    for (i = pf->enabled; i > 0; --i)
        toy_destroy_slot(pf, i - 1);
    pf->enabled = 0;
    mutex_unlock(&pf->admin_mutex);
    for (i = 0; i < pf->irq_count; ++i)
        disable_irq(pci_irq_vector(pdev, i));
    while (pf->irq_count) {
        --pf->irq_count;
        synchronize_irq(pci_irq_vector(pdev, pf->irq_count));
        free_irq(pci_irq_vector(pdev, pf->irq_count), pf);
    }
    pci_free_irq_vectors(pdev);
    pci_clear_master(pdev);
    pci_iounmap(pdev, pf->bar);
    pf->bar = NULL;
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pci_set_drvdata(pdev, NULL);
    dev_info(&pdev->dev, "TOY_PF_REMOVED\n");
    kref_put(&pf->ref, toy_release_ref);
}

static const struct pci_device_id toy_ids[] = {
    { PCI_DEVICE(TOY_PCI_VENDOR, TOY_PCI_PF) },
    { }
};
MODULE_DEVICE_TABLE(pci, toy_ids);
static struct pci_driver toy_driver = {
    .name = "toy_pf",
    .id_table = toy_ids,
    .probe = toy_probe,
    .remove = toy_remove,
    .sriov_configure = toy_sriov_configure,
};
module_pci_driver(toy_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SR-IOV lab PF mailbox and lifecycle adapter");
