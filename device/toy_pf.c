/* PF 的 PCI/MMIO 呈現與 mailbox 排程；VF 的 FSM/ledger 交由 fwcore 處理。 */
#include "qemu/osdep.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_sriov.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "toy.h"

static uint32_t read_slot(ToyPF *pf, unsigned vf, uint32_t reg)
{
    ToyView *s = &pf->views[vf];
    struct fw_vf *snap = &pf->snapshot.vf[vf];
    switch (reg) {
    case TOY_PF_VF_VIEW_HW_STATUS: return s->hw_flags;
    case TOY_PF_VF_VIEW_EPOCH_LO: return (uint32_t)pf->snapshot.hw[vf].epoch;
    case TOY_PF_VF_VIEW_EPOCH_HI: return pf->snapshot.hw[vf].epoch >> 32;
    case TOY_PF_VF_VIEW_STATE: return snap->state;
    case TOY_PF_VF_VIEW_RESERVE: return snap->reserve;
    case TOY_PF_VF_VIEW_OUTSTANDING: return s->outstanding;
    case TOY_PF_VF_VIEW_STALE_DESC: return s->stale_desc;
    case TOY_PF_VF_VIEW_STALE_CPL: return s->stale_cpl;
    case TOY_PF_VF_VIEW_FENCE_BLOCKED: return s->fence_blocked;
    case TOY_PF_VF_VIEW_DMA_FAULT: return s->dma_fault;
    case TOY_PF_VF_VIEW_LAST_FAULT: return s->last_fault;
    case TOY_PF_VF_VIEW_SERVICED_LO: return (uint32_t)snap->serviced;
    case TOY_PF_VF_VIEW_SERVICED_HI: return snap->serviced >> 32;
    case TOY_PF_VF_VIEW_IRQ_SUPPRESSED: return s->irq_suppressed;
    case TOY_PF_VF_VIEW_READY_GEN: return s->ready_gen;
    default: return 0;
    }
}

/* Driver 持鎖發出 SNAPSHOT 後，後續寄存器讀取使用同一份快照。 */
fw_status toy_snapshot(ToyPF *pf)
{
    struct fw_snapshot core;
    ToyView views[FW_VFS];
    fw_status rc=fw_snapshot(&pf->core, &core);
    if (rc!=FW_OK) { return rc; }
    /* 全部在同一 BQL 區段取樣，成功後才一次提交；MMIO 讀取不再混入 live 值。 */
    for (unsigned i=0; i<FW_VFS; i++) {
        ToySlot *s=&pf->slots[i];
        views[i]=(ToyView) {
            .hw_flags=toy_hw_flags(pf,i), .outstanding=s->pending.valid,
            .stale_desc=s->stale_desc, .stale_cpl=s->stale_cpl,
            .fence_blocked=s->fence_blocked, .dma_fault=s->dma_fault,
            .last_fault=s->last_fault, .irq_suppressed=s->irq_suppressed,
            .ready_gen=s->ready_gen,
        };
    }
    pf->snapshot=core;
    memcpy(pf->views,views,sizeof(views));
    return FW_OK;
}

static uint64_t read_pf(void *opaque, hwaddr addr, unsigned size)
{
    ToyPF *pf = opaque;
    if (size != 4 || (addr & 3)) {
        toy_saturate(&pf->bad_mmio);
        return 0;
    }
    if (addr >= TOY_PF_VF_BASE && addr < TOY_PF_VF_BASE + FW_VFS * TOY_PF_VF_STRIDE) {
        return read_slot(pf, (addr - TOY_PF_VF_BASE) / TOY_PF_VF_STRIDE,
                         (addr - TOY_PF_VF_BASE) % TOY_PF_VF_STRIDE);
    }
    if (addr >= TOY_PF_MBX_ARG0 && addr <= TOY_PF_MBX_ARG3) {
        return pf->mbx_staging.arg[(addr - TOY_PF_MBX_ARG0) / 4];
    }
    if (addr >= TOY_PF_MBX_REPLY0 && addr <= TOY_PF_MBX_REPLY3) {
        return pf->mbx_reply.value[(addr - TOY_PF_MBX_REPLY0) / 4];
    }
    switch (addr) {
    case TOY_PF_MAGIC: return TOY_PF_MAGIC_VALUE;
    case TOY_PF_ABI: return TOY_ABI_VERSION;
    case TOY_PF_CAP_VFS: return FW_VFS;
    case TOY_PF_CAP_QUEUES: return FW_VFS;
    case TOY_PF_CAP_VECTORS: return 16;
    case TOY_PF_CAP_CREDIT: return FW_CAP_CREDIT;
    case TOY_PF_CAP_FEATURES: return TOY_FEATURES_DMA_FENCE | TOY_FEATURES_VF_KILL | TOY_FEATURES_MODELED_FLR;
    case TOY_PF_SESSION_LO: return (uint32_t)pf->snapshot.session;
    case TOY_PF_SESSION_HI: return pf->snapshot.session >> 32;
    case TOY_PF_EVENT: return pf->event;
    case TOY_PF_BAD_MMIO: return pf->bad_mmio;
    case TOY_PF_FENCE_TOKEN: return pf->fence_token;
    case TOY_PF_HW_RESULT: return pf->hw_result;
    case TOY_PF_MBX_SEQ: return pf->mbx_staging.seq;
    case TOY_PF_MBX_OPCODE: return pf->mbx_staging.opcode;
    case TOY_PF_MBX_TARGET: return pf->mbx_staging.target;
    case TOY_PF_MBX_STATUS: return pf->mbx_status;
    case TOY_PF_MBX_RESULT: return pf->mbx_reply.status;
    case TOY_PF_MBX_ACK_SEQ: return pf->mbx_reply.seq;
    default: return 0;
    }
}

/* 發布回覆與通知；故障注入可丟一次回覆，用來檢查原 request 的重送行為。 */
static void mailbox_complete(ToyPF *pf)
{
    if (pf->drop_mbx_reply_once) {
        pf->drop_mbx_reply_once = false;
        pf->mbx_status = TOY_MBX_STATUS_IDLE;
        fprintf(stderr, "TOY_FAULT_ACK kind=drop_mbx_reply_once seq=%u\n", pf->mbx_reply.seq);
        memset(&pf->mbx_reply, 0, sizeof(pf->mbx_reply));
        return;
    }
    pf->mbx_status = TOY_MBX_STATUS_DONE;
    pf->event |= TOY_PF_EVENTS_MBX_DONE;
    msix_notify(PCI_DEVICE(pf), 0);
}

void toy_mbx_poll(ToyPF *pf)
{
    unsigned vf=pf->mbx_current.target;
    if (!pf->core.mailbox_pending || pf->fw_hang_all ||
        (vf<FW_VFS && pf->fw_hang_vf && toy_fault_target(pf,vf))) { return; }
    if (fw_poll_mailbox(&pf->core,&pf->mbx_reply)!=FW_BUSY) { mailbox_complete(pf); }
}

void toy_mbx_work(void *opaque)
{
    ToyPF *pf = opaque;
    struct fw_request *req = &pf->mbx_current;
    /* BH 已取出；故障注入可能保留 BUSY，但不代表仍有排程中的 BH。 */
    pf->mbx_pending = false;
    if (pf->fw_hang_all || (pf->fw_hang_vf && req->target < FW_VFS && toy_fault_target(pf, req->target))) {
        fprintf(stderr, "TOY_FAULT_ACK kind=mailbox_hang vf=%u seq=%u\n", req->target, req->seq);
        return;
    }
    fw_handle_mailbox(&pf->core, req, &pf->mbx_reply);
    toy_schedule(pf);
    if (!pf->core.mailbox_pending) { mailbox_complete(pf); }
}

static void write_pf(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ToyPF *pf = opaque;
    uint32_t val = value;
    if (size != 4 || (addr & 3)) {
        toy_saturate(&pf->bad_mmio);
        return;
    }
    if (addr >= TOY_PF_VF_BASE && addr < TOY_PF_VF_BASE + FW_VFS * TOY_PF_VF_STRIDE) {
        unsigned vf = (addr - TOY_PF_VF_BASE) / TOY_PF_VF_STRIDE;
        unsigned reg = (addr - TOY_PF_VF_BASE) % TOY_PF_VF_STRIDE;
        if (reg != TOY_PF_VF_VIEW_HW_CTL || !val || (val & ~3u)) {
            toy_saturate(&pf->bad_mmio);
            pf->hw_result = FW_INVAL;
            return;
        }
        pf->hw_result = FW_OK;
        if (val & TOY_VF_HW_CONTROL_FENCE_BEGIN) {
            pf->hw_result = toy_fence(pf, vf, pf->fence_token, true);
        }
        if (val & TOY_VF_HW_CONTROL_KILL) {
            toy_kill(pf, vf);
            toy_schedule(pf);
        }
        return;
    }
    if (addr >= TOY_PF_MBX_ARG0 && addr <= TOY_PF_MBX_ARG3) {
        pf->mbx_staging.arg[(addr - TOY_PF_MBX_ARG0) / 4] = val;
        return;
    }
    if (addr >= TOY_CAPSULE_STAGE_OFFSET && addr < TOY_CAPSULE_STAGE_OFFSET + TOY_CAPSULE_STAGE_LENGTH) {
        pf->hw_result = FW_UNSUP;
        return;
    }
    switch (addr) {
    case TOY_PF_SNAPSHOT:
        if (val != 1) { break; }
        pf->hw_result = toy_snapshot(pf);
        return;
    case TOY_PF_EVENT:
        if (val & ~15u) { break; }
        pf->event &= ~val;
        return;
    case TOY_PF_CONTROL:
        if (!val || (val & ~3u)) { break; }
        if (val & TOY_PF_CONTROL_FENCE_ALL) {
            pf->hw_result = FW_OK;
            for (unsigned i = 0; i < FW_VFS; i++) {
                fw_status status = toy_fence(pf, i, 0, false);
                if (status != FW_OK) { pf->hw_result = status; }
            }
        }
        if (val & TOY_PF_CONTROL_GLOBAL_RESET) {
            pf->hw_result = toy_global_reset(pf);
        }
        return;
    case TOY_PF_FENCE_TOKEN: pf->fence_token = val; return;
    case TOY_PF_MBX_SEQ: pf->mbx_staging.seq = val; return;
    case TOY_PF_MBX_OPCODE: pf->mbx_staging.opcode = val; return;
    case TOY_PF_MBX_TARGET: pf->mbx_staging.target = val; return;
    case TOY_PF_MBX_SUBMIT:
        if (val != 1) { break; }
        if (pf->mbx_status == TOY_MBX_STATUS_BUSY) {
            pf->hw_result = FW_BUSY;
            return;
        }
        pf->mbx_current = pf->mbx_staging;
        pf->mbx_current.session = pf->session;
        pf->mbx_status = TOY_MBX_STATUS_BUSY;
        pf->mbx_pending = true;
        qemu_bh_schedule(pf->mbx_bh);
        return;
    case TOY_PF_MBX_ACK:
        if (val != 1) { break; }
        if (pf->mbx_status == TOY_MBX_STATUS_DONE) {
            pf->mbx_status = TOY_MBX_STATUS_IDLE;
        }
        return;
    default: break;
    }
    toy_saturate(&pf->bad_mmio);
}

static const MemoryRegionOps pf_mmio_ops = {
    .read = read_pf, .write = write_pf, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void pf_config_write(PCIDevice *dev, uint32_t address, uint32_t value, int length)
{
    pci_default_write_config(dev, address, value, length);
    pcie_cap_flr_write_config(dev, address, value, length);
}

static void pf_realize(PCIDevice *dev, Error **errp)
{
    ToyPF *pf = TOY_PF(dev);
    int result;
    if (pf->total_vfs != FW_VFS) {
        error_setg(errp, "total-vfs must be 8 for ABI 1.0");
        return;
    }
    dev->config_write = pf_config_write;
    memory_region_init_io(&pf->bar0, OBJECT(pf), &pf_mmio_ops, pf, "toy-pf-mmio", TOY_LIMITS_PF_BAR0_BYTES);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_64, &pf->bar0);
    memory_region_init(&pf->bar2, OBJECT(pf), "toy-pf-msix", TOY_LIMITS_BAR2_BYTES);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &pf->bar2);
    result = pcie_endpoint_cap_init(dev, 0x80);
    if (result < 0) {
        error_setg(errp, "PF PCIe capability initialization failed");
        return;
    }
    result = msix_init(dev, 2, &pf->bar2, 2, 0, &pf->bar2, 2, 0x1000, 0x50, errp);
    if (result) {
        pcie_cap_exit(dev);
        return;
    }
    msix_vector_use(dev, 0);
    msix_vector_use(dev, 1);
    pcie_cap_flr_init(dev);
    pcie_ari_init(dev, 0x100);
    pcie_sriov_pf_init(dev, 0x140, TYPE_TOY_VF, TOY_PCI_VF, FW_VFS, FW_VFS, 1, 1);
    pcie_sriov_pf_init_vf_bar(dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_64, TOY_LIMITS_VF_BAR0_BYTES);
    pcie_sriov_pf_init_vf_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, TOY_LIMITS_BAR2_BYTES);
    /* 沒有 function-to-function P2P；endpoint ACS optional bits 為 0。 */
    /* v9.2.0 的 SR-IOV reset 會以 quad 清至 0x183；保留間距避免覆蓋 ACS。 */
    pcie_acs_init(dev, 0x200);
    pf->session = 1;
    for (unsigned i = 0; i < FW_VFS; i++) {
        pf->slots[i].epoch = pf->slots[i].token = 1;
        pf->slots[i].fenced = true;
    }
    toy_adapter_init(pf);
    pf->worker = timer_new_ms(QEMU_CLOCK_VIRTUAL, toy_worker, pf);
    pf->mbx_bh = qemu_bh_new(toy_mbx_work, pf);
    pf->initialized = true;
    pf->hw_result = toy_snapshot(pf);
}

static void pf_reset(Object *obj, ResetType type)
{
    ToyPF *pf = TOY_PF(obj);
    if (!pf->initialized) { return; }
    /* machine/PF reset 先消除 PCI presentation，再做全域 session transaction。 */
    for (unsigned i = 0; i < FW_VFS; i++) { toy_kill(pf, i); }
    pcie_sriov_pf_reset(PCI_DEVICE(pf));
    qemu_bh_cancel(pf->mbx_bh);
    pf->mbx_pending = false;
    timer_del(pf->worker);
    pf->mbx_status = TOY_MBX_STATUS_IDLE;
    pf->event = 0;
    pf->hw_result = toy_global_reset(pf);
    if (toy_snapshot(pf) != FW_OK) {
        pf->hw_result = FW_IO;
    }
}

static void pf_exit(PCIDevice *dev)
{
    ToyPF *pf = TOY_PF(dev);
    if (pf->initialized) {
        for (unsigned i = 0; i < FW_VFS; i++) { toy_kill(pf, i); }
        timer_del(pf->worker);
        timer_free(pf->worker);
        pf->worker = NULL;
        qemu_bh_cancel(pf->mbx_bh);
        pf->mbx_pending = false;
        qemu_bh_delete(pf->mbx_bh);
        pf->mbx_bh = NULL;
        pcie_sriov_pf_exit(dev);
        pf->initialized = false;
    }
    msix_unuse_all_vectors(dev);
    msix_uninit(dev, &pf->bar2, &pf->bar2);
    pcie_cap_exit(dev);
}

static void telemetry_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    uint32_t value = 0;
    /* 只讀實際物件與工作狀態，不透過 fault setter 也不更動裝置。 */
    if (!strcmp(name, "live-vfs")) {
        for (unsigned i = 0; i < FW_VFS; i++) {
            value += pf->slots[i].vf != NULL;
        }
    } else if (!strcmp(name, "pending-completions")) {
        for (unsigned i = 0; i < FW_VFS; i++) {
            value += pf->slots[i].pending.valid;
        }
    } else if (!strcmp(name, "worker-pending")) {
        value = pf->worker && timer_pending(pf->worker);
    } else if (!strcmp(name, "mailbox-pending")) {
        value = pf->mbx_pending;
    }
    visit_type_uint32(v, name, &value, errp);
}

static void virtual_tick_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    uint64_t value = toy_tick();
    /* controller 只觀察 device virtual time；不能用 host sleep 當成故障時鐘。 */
    visit_type_uint64(v, name, &value, errp);
}

static void fault_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    uint32_t value = 0;
    if (!strcmp(name, "fault-vf")) { value = pf->fault_vf; }
    else if (!strcmp(name, "fault-after")) { value = pf->fault_after; }
    else if (!strcmp(name, "delay-cpl-ticks")) { value = pf->delay_cpl_ticks; }
    else if (!strcmp(name, "fault-ack")) { value = pf->fault_ack; }
    visit_type_uint32(v, name, &value, errp);
}

static void fault_set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) { return; }
    if (!strcmp(name, "fault-vf")) {
        if (value >= FW_VFS) { error_setg(errp, "fault-vf outside 0..7"); return; }
        pf->fault_vf = value;
    } else if (!strcmp(name, "fault-after")) {
        pf->fault_after = value;
    } else if (!strcmp(name, "delay-cpl-ticks")) {
        if (value > 60000) { error_setg(errp, "delay-cpl-ticks exceeds 60000"); return; }
        pf->delay_cpl_ticks = value;
    }
    toy_saturate(&pf->fault_ack);
    fprintf(stderr, "TOY_FAULT_ACK property=%s value=%u ack=%u\n", name, value, pf->fault_ack);
}

static void bool_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    bool *field = (bool *)((char *)obj + (uintptr_t)opaque);
    visit_type_bool(v, name, field, errp);
}

static void bool_set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    bool value;
    bool *field = (bool *)((char *)obj + (uintptr_t)opaque);
    if (!visit_type_bool(v, name, &value, errp)) { return; }
    *field = value;
    toy_saturate(&pf->fault_ack);
    if (!value) { toy_schedule(pf); }
    fprintf(stderr, "TOY_FAULT_ACK property=%s value=%u ack=%u\n", name, value, pf->fault_ack);
}

static bool trigger_get(Object *obj, Error **errp) { return false; }

static void stale_set(Object *obj, bool value, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    if (value) {
        ToyOperation copy = pf->slots[pf->fault_vf].retired;
        bool was_valid = copy.valid;
        toy_complete(pf, pf->fault_vf, &copy);
        toy_saturate(&pf->fault_ack);
        /* callback 會消耗副本；分別保存呼叫前有效性與呼叫後消耗結果。 */
        fprintf(stderr, "TOY_FAULT_ACK property=try-stale-cpl valid=%u ack=%u consumed=%u\n",
                was_valid, pf->fault_ack, !copy.valid);
    }
}

static void irq_set(Object *obj, bool value, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    if (value) {
        toy_irq(pf, pf->fault_vf, 0, &pf->slots[pf->fault_vf].retired);
        toy_saturate(&pf->fault_ack);
        fprintf(stderr, "TOY_FAULT_ACK property=try-irq-fenced ack=%u\n", pf->fault_ack);
    }
}

static const char *checkpoint_names[FW_CP_COUNT] = {
    "provision.before_create", "provision.after_create", "provision.before_commit",
    "provision.abort_release", "destroy.release",
};

fw_status toy_checkpoint(ToyPF *pf, unsigned vf, enum fw_checkpoint point)
{
    for (unsigned i = 0; i < ARRAY_SIZE(pf->failpoints); i++) {
        ToyFailpoint *slot = &pf->failpoints[i];
        if (slot->point != point || slot->fired ||
            (slot->vf != UINT32_MAX && slot->vf != vf)) {
            continue;
        }
        toy_saturate(&slot->seen);
        if (slot->seen == slot->hit) {
            slot->fired = 1;
            fprintf(stderr, "TOY_FAILPOINT_HIT slot=%s name=%s vf=%u hit=%u\n",
                    i ? "unwind" : "forward", checkpoint_names[point], vf, slot->hit);
            return FW_IO;
        }
    }
    return FW_OK;
}

static void failpoint_get(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ToyFailpoint *slot = &TOY_PF(obj)->failpoints[(uintptr_t)opaque];
    const char *field = strrchr(name, '-') + 1;
    uint32_t value;
    if (!strcmp(field, "name")) {
        char *value = (char *)(slot->point < 0 ? "" : checkpoint_names[slot->point]);
        visit_type_str(v, name, &value, errp);
        return;
    }
    value = !strcmp(field, "hit") ? slot->hit : !strcmp(field, "vf") ? slot->vf :
            !strcmp(field, "seen") ? slot->seen : slot->fired;
    visit_type_uint32(v, name, &value, errp);
}

static void failpoint_set(Object *obj, Visitor *v, const char *name, void *opaque, Error **errp)
{
    ToyPF *pf = TOY_PF(obj);
    ToyFailpoint *slot = &pf->failpoints[(uintptr_t)opaque];
    const char *field = strrchr(name, '-') + 1;
    if (!strcmp(field, "name")) {
        char *value = NULL;
        int point = -1;
        if (!visit_type_str(v, name, &value, errp)) { return; }
        for (unsigned i = 0; i < FW_CP_COUNT; i++) {
            if (!strcmp(value, checkpoint_names[i])) { point = i; }
        }
        if (*value && point < 0) {
            error_setg(errp, "Unknown named failpoint: %s", value);
            g_free(value);
            return;
        }
        g_free(value);
        slot->point = point;
        slot->seen = slot->fired = 0;
    } else {
        uint32_t value;
        if (!visit_type_uint32(v, name, &value, errp)) { return; }
        if (!strcmp(field, "hit")) {
            if (!value) { error_setg(errp, "failpoint-hit must be nonzero"); return; }
            slot->hit = value;
        } else {
            if (value >= FW_VFS && value != UINT32_MAX) {
                error_setg(errp, "failpoint-vf must be 0..7 or UINT32_MAX"); return;
            }
            slot->vf = value;
        }
    }
    toy_saturate(&pf->fault_ack);
    fprintf(stderr, "TOY_FAULT_ACK property=%s ack=%u\n", name, pf->fault_ack);
}

static void pf_instance_init(Object *obj)
{
    ToyPF *pf = TOY_PF(obj);
    static const struct { const char *name; size_t offset; } boolean[] = {
        { "fw-hang-vf", offsetof(ToyPF, fw_hang_vf) },
        { "fw-hang-all", offsetof(ToyPF, fw_hang_all) },
        { "drop-mbx-reply-once", offsetof(ToyPF, drop_mbx_reply_once) },
        { "ignore-quiesce", offsetof(ToyPF, ignore_quiesce) },
        { "ignore-reset", offsetof(ToyPF, ignore_reset) },
    };
    const char *numbers[] = { "fault-vf", "fault-after", "delay-cpl-ticks" };
    const char *telemetry[] = { "live-vfs", "pending-completions", "worker-pending", "mailbox-pending" };
    for (unsigned i = 0; i < ARRAY_SIZE(boolean); i++) {
        object_property_add(obj, boolean[i].name, "bool", bool_get, bool_set, NULL,
                            (void *)(uintptr_t)boolean[i].offset);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(numbers); i++) {
        object_property_add(obj, numbers[i], "uint32", fault_get, fault_set, NULL, NULL);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(telemetry); i++) {
        object_property_add(obj, telemetry[i], "uint32", telemetry_get, NULL, NULL, NULL);
    }
    object_property_add(obj, "fault-ack", "uint32", fault_get, NULL, NULL, NULL);
    object_property_add(obj, "virtual-tick", "uint64", virtual_tick_get, NULL, NULL, NULL);
    object_property_add_bool(obj, "try-stale-cpl", trigger_get, stale_set);
    object_property_add_bool(obj, "try-irq-fenced", trigger_get, irq_set);
    for (unsigned i = 0; i < ARRAY_SIZE(pf->failpoints); i++) {
        const char *fields[] = { "name", "hit", "vf", "seen", "fired" };
        pf->failpoints[i] = (ToyFailpoint){ .point = -1, .hit = 1, .vf = UINT32_MAX };
        for (unsigned j = 0; j < ARRAY_SIZE(fields); j++) {
            g_autofree char *name = g_strdup_printf("%sfailpoint-%s", i ? "unwind-" : "", fields[j]);
            object_property_add(obj, name, j ? "uint32" : "str", failpoint_get,
                                j < 3 ? failpoint_set : NULL, NULL, (void *)(uintptr_t)i);
        }
    }
}

static Property properties[] = {
    DEFINE_PROP_UINT32("total-vfs", ToyPF, total_vfs, FW_VFS),
    DEFINE_PROP_END_OF_LIST(),
};

static void pf_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    pc->realize = pf_realize;
    pc->exit = pf_exit;
    pc->vendor_id = TOY_PCI_VENDOR;
    pc->device_id = TOY_PCI_PF;
    pc->revision = TOY_PCI_REVISION;
    pc->class_id = TOY_PCI_CLASS_CODE >> 8;
    RESETTABLE_CLASS(klass)->phases.hold = pf_reset;
    dc->desc = "SR-IOV Controller Firmware Lab PF (migration unsupported)";
    device_class_set_props(dc, properties);
}

static const TypeInfo pf_type = {
    .name = TYPE_TOY_PF, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ToyPF), .instance_init = pf_instance_init, .class_init = pf_class_init,
    .interfaces = (InterfaceInfo[]) { { INTERFACE_PCIE_DEVICE }, { } },
};

static void register_pf(void) { type_register_static(&pf_type); }
type_init(register_pf)
