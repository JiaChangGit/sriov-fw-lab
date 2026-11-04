/* 將 portable core 的 HAL 接到 QEMU 裝置欄位；這層不執行 guest 的 VFIO ioctl。 */
#include "qemu/osdep.h"
#include "hw/pci/msix.h"
#include "toy.h"

static uint64_t hal_now(void *opaque)
{
    return toy_tick();
}

static void hal_log(void *opaque, const char *message, fw_u16 vf, fw_u64 value)
{
    /* 英文事件保存於 QEMU stderr；guest serial 另有獨立紀錄。 */
    fprintf(stderr, "TOY_CORE event=%s vf=%u value=%" PRIu64 "\n", message, vf, value);
}

static fw_status hal_read(void *opaque, fw_u16 vf, struct fw_hw_snapshot *out)
{
    ToyPF *pf = opaque;
    ToySlot *s;
    if (vf >= FW_VFS || !out) {
        return FW_INVAL;
    }
    s = &pf->slots[vf];
    memset(out, 0, sizeof(*out));
    out->session = pf->session;
    out->epoch = s->epoch;
    out->token = s->token;
    out->fenced = s->fenced;
    out->killed = s->killed;
    out->config_valid = s->config_valid;
    out->ready_pending = s->ready_pending;
    out->ready_gen = s->ready_gen;
    out->outstanding = s->pending.valid;
    out->pci_present = s->vf != NULL;
    out->bme = s->vf && (pci_get_word(s->vf->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER);
    return FW_OK;
}

static fw_status hal_cancel(void *opaque, fw_u16 vf)
{
    ToyPF *pf = opaque;
    ToySlot *s = &pf->slots[vf];
    if (!s->fenced) {
        return FW_STATE;
    }
    if (s->pending.valid) {
        s->retired = s->pending;
        s->pending.valid = false;
    }
    return FW_OK;
}

static fw_status hal_clear(void *opaque, fw_u16 vf)
{
    ToyPF *pf = opaque;
    if (!pf->slots[vf].fenced) {
        return FW_STATE;
    }
    toy_clear_queue(pf, vf);
    return FW_OK;
}

static fw_status hal_gate(void *opaque, fw_u16 vf, fw_u8 open)
{
    ToyPF *pf = opaque;
    ToySlot *s = &pf->slots[vf];
    if (open && (s->killed || !s->vf || !s->config_valid || !s->ready_pending ||
                 !(pci_get_word(s->vf->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER))) {
        return FW_STATE;
    }
    s->fenced = !open;
    if (open) {
        s->queue_status = 2;
        s->ready_pending = false;
        toy_schedule(pf);
    }
    return FW_OK;
}

static fw_status hal_kill(void *opaque, fw_u16 vf)
{
    toy_kill(opaque, vf);
    return FW_OK;
}

/* 裝置只能觀察 VF 是否仍存在，無法直接讀取 guest 的 fd 或 lease；兩層需共同滿足回收契約。 */
static fw_u8 hal_closed(void *opaque, fw_u16 vf)
{
    ToyPF *pf = opaque;
    unsigned i;
    if (vf == FW_GLOBAL) {
        for (i = 0; i < FW_VFS; i++) {
            if (pf->slots[i].vf) {
                return 0;
            }
        }
        return 1;
    }
    /* lease/fd 結束由可信 PF driver 保證；硬體再要求 PCI VF 已移除。 */
    return vf < FW_VFS && !pf->slots[vf].vf;
}

/* 所有 VF 移除後才換全域 session；先驗溢位，保持 fenced，最後由 end_global 解除 KILL。 */
static fw_status hal_begin_global(void *opaque, fw_u64 *session, fw_u64 epochs[FW_VFS])
{
    ToyPF *pf = opaque;
    unsigned i;
    if (!hal_closed(pf, FW_GLOBAL)) {
        return FW_BUSY;
    }
    for (i = 0; i < FW_VFS; i++) {
        pf->slots[i].fenced = true;
    }
    if (pf->session == UINT64_MAX) {
        for (i = 0; i < FW_VFS; i++) {
            toy_kill(pf, i);
        }
        return FW_OVERFLOW;
    }
    for (i = 0; i < FW_VFS; i++) {
        if (pf->slots[i].epoch == UINT64_MAX || pf->slots[i].token == UINT64_MAX) {
            for (unsigned j = 0; j < FW_VFS; j++) {
                toy_kill(pf, j);
            }
            return FW_OVERFLOW;
        }
    }
    pf->session++;
    for (i = 0; i < FW_VFS; i++) {
        ToySlot *s = &pf->slots[i];
        s->epoch++;
        s->token++;
        s->last_fence_token = 0;
        toy_clear_queue(pf, i);
        epochs[i] = s->epoch;
    }
    *session = pf->session;
    return FW_OK;
}

static fw_status hal_end_global(void *opaque)
{
    ToyPF *pf = opaque;
    unsigned i;
    for (i = 0; i < FW_VFS; i++) {
        pf->slots[i].killed = false;
        pf->slots[i].fenced = true;
    }
    return FW_OK;
}

static fw_status hal_checkpoint(void *opaque, fw_u16 vf, enum fw_checkpoint point)
{
    return toy_checkpoint(opaque,vf,point);
}

static fw_status hal_progress(void *opaque, fw_u16 vf, fw_u32 opcode)
{
    ToyPF *pf=opaque;
    if (toy_fault_target(pf,vf) &&
        ((opcode==FW_QUIESCE && pf->ignore_quiesce) || (opcode==FW_RESET && pf->ignore_reset))) {
        if (!pf->core.mailbox_pending) {
            fprintf(stderr,"TOY_FAULT_ACK kind=mailbox_deferred vf=%u opcode=%u tick=%" PRIu64 " deadline=%" PRIu64 "\n",
                    vf,opcode,toy_tick(),pf->core.vf[vf].deadline);
        }
        return FW_BUSY;
    }
    return FW_OK;
}

fw_status toy_global_reset(ToyPF *pf)
{
    uint64_t session, epochs[FW_VFS];
    fw_status status = hal_begin_global(pf, &session, epochs);
    if (status == FW_OK) {
        status = fw_global_recover(&pf->core, session, epochs);
    }
    if (status == FW_OK) {
        status = hal_end_global(pf);
    }
    if (status == FW_OK) {
        if (pf->mbx_bh) { qemu_bh_cancel(pf->mbx_bh); }
        pf->mbx_pending=false;
        pf->mbx_status=TOY_MBX_STATUS_IDLE;
        pf->event &= ~TOY_PF_EVENTS_MBX_DONE;
        memset(&pf->mbx_current,0,sizeof(pf->mbx_current));
        memset(&pf->mbx_reply,0,sizeof(pf->mbx_reply));
    }
    return status;
}

void toy_adapter_init(ToyPF *pf)
{
    const struct fw_hal hal = {
        .opaque = pf, .now_tick = hal_now, .log = hal_log, .read_hw = hal_read,
        .cancel_io = hal_cancel, .clear_queue = hal_clear, .set_gate = hal_gate,
        .kill = hal_kill, .owners_closed = hal_closed,
        .begin_global = hal_begin_global, .end_global = hal_end_global,
        .checkpoint = hal_checkpoint, .mailbox_progress = hal_progress,
    };
    fw_status status = fw_init(&pf->core, &hal);
    assert(status == FW_OK);
}
