/* QEMU 裝置的 DMA、完成通知與取消路徑。資料 DMA 使用 VF requester；DMA/IRQ 均檢查 gate。 */
#include "qemu/osdep.h"
#include "hw/pci/msix.h"
#include "toy.h"

void toy_saturate(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        (*counter)++;
    }
}

uint64_t toy_tick(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

bool toy_fault_target(ToyPF *pf, unsigned vf)
{
    return vf == pf->fault_vf && pf->operations >= pf->fault_after;
}

uint32_t toy_hw_flags(ToyPF *pf, unsigned vf)
{
    ToySlot *s = &pf->slots[vf];
    return (s->fenced ? TOY_HW_FLAGS_FENCED : 0) |
           (s->killed ? TOY_HW_FLAGS_KILLED : 0) |
           (s->vf ? TOY_HW_FLAGS_PCI_PRESENT : 0) |
           (s->vf && (pci_get_word(s->vf->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER) ? TOY_HW_FLAGS_BME : 0) |
           (s->config_valid ? TOY_HW_FLAGS_CONFIG_VALID : 0) |
           (s->ready_pending ? TOY_HW_FLAGS_READY_PENDING : 0);
}

/* 保留舊 operation 的副本供 stale-callback 測試；移出 pending 後不再由正常 worker 完成。 */
static void cancel_pending(ToySlot *s)
{
    if (s->pending.valid) {
        s->retired = s->pending;
        s->pending.valid = false;
    }
}

static void clear_pending_irq(ToySlot *s)
{
    if (s->vf) {
        msix_clr_pending(PCI_DEVICE(s->vf), 0);
        msix_clr_pending(PCI_DEVICE(s->vf), 1);
    }
    s->irq_cause = 0;
}

fw_status toy_fence(ToyPF *pf, unsigned vf, uint32_t token, bool deduplicate)
{
    ToySlot *s = &pf->slots[vf];
    if (deduplicate) {
        if (!token) {
            return FW_INVAL;
        }
        if (token < s->last_fence_token) {
            return FW_STALE_SEQ;
        }
        if (token == s->last_fence_token) {
            s->fenced = true;
            cancel_pending(s);
            clear_pending_irq(s);
            return FW_OK;
        }
    }
    /* 順序不可依賴 firmware 回應；同一 BQL 區段先關 gate 再換代。 */
    s->fenced = true;
    cancel_pending(s);
    clear_pending_irq(s);
    if (s->epoch == UINT64_MAX || s->token == UINT64_MAX) {
        s->killed = true;
        return FW_OVERFLOW;
    }
    s->epoch++;
    s->token++;
    if (deduplicate) {
        s->last_fence_token = token;
        s->last_fence_epoch = s->epoch;
    }
    return FW_OK;
}

void toy_kill(ToyPF *pf, unsigned vf)
{
    ToySlot *s = &pf->slots[vf];
    s->fenced = s->killed = true;
    if (s->token != UINT64_MAX) {
        s->token++;
    }
    cancel_pending(s);
    clear_pending_irq(s);
}

void toy_clear_queue(ToyPF *pf, unsigned vf)
{
    ToySlot *s = &pf->slots[vf];
    cancel_pending(s);
    clear_pending_irq(s);
    memset(&s->config, 0, sizeof(s->config));
    memset(&s->staging, 0, sizeof(s->staging));
    s->config_valid = s->ready_pending = false;
    s->sq_head = s->sq_tail = s->cq_head = s->cq_tail = 0;
    s->queue_status = TOY_QUEUE_STATUS_UNCONFIGURED;
    s->queue_error = FW_OK;
    s->dma_trace_seen = 0;
}

void toy_vf_flr(ToyPF *pf, unsigned vf)
{
    fw_status status = toy_fence(pf, vf, 0, false);
    toy_clear_queue(pf, vf);
    if (status == FW_OK && pf->core.vf[vf].state != FW_FREE) {
        status = fw_vf_event(&pf->core, vf, FW_FLR);
    }
    if (status != FW_OK) {
        toy_kill(pf, vf);
    }
    pf->event |= TOY_PF_EVENTS_FLR_EVENT;
    msix_notify(PCI_DEVICE(pf), 1);
    toy_schedule(pf);
    fprintf(stderr, "TOY_FLR vf=%u epoch=%" PRIu64 " status=%u\n", vf, pf->slots[vf].epoch, status);
}

void toy_fail(ToyPF *pf, unsigned vf, uint32_t reason)
{
    ToySlot *s = &pf->slots[vf];
    fw_status status;
    s->last_fault = reason;
    status = toy_fence(pf, vf, 0, false);
    toy_kill(pf, vf);
    /* 不虛構 timeout：沿已定義的 fence/drain/reset/verify-fail 事件收斂。 */
    if (status == FW_OK && pf->core.vf[vf].state == FW_ACTIVE) {
        status = fw_vf_event(&pf->core, vf, FW_FENCE);
    }
    if (status == FW_OK && pf->core.vf[vf].state == FW_QUIESCING) {
        status = fw_vf_event(&pf->core, vf, FW_DRAIN_OK);
    }
    if (status == FW_OK && pf->core.vf[vf].state == FW_RESETTING) {
        status = fw_vf_event(&pf->core, vf, FW_RESET_DONE);
    }
    if (status == FW_OK && pf->core.vf[vf].state == FW_RECOVERING) {
        status = fw_vf_event(&pf->core, vf, FW_VERIFY_FAIL);
    }
    s->queue_status = TOY_QUEUE_STATUS_ERROR;
    s->queue_error = reason == TOY_LAST_FAULT_DMA ? FW_DMA : FW_BAD_QUEUE;
    pf->event |= TOY_PF_EVENTS_VF_HEALTH;
    msix_notify(PCI_DEVICE(pf), 1);
    fprintf(stderr, "TOY_FAULT vf=%u rid=%04x reason=%u core_status=%u\n", vf,
            s->vf ? pci_get_bdf(PCI_DEVICE(s->vf)) : 0xffff, reason, status);
}

static bool same_generation(ToyPF *pf, ToySlot *s, const ToyOperation *op)
{
    return !op || (op->session == pf->session && op->epoch == s->epoch && op->token == s->token);
}

/* 每次 DMA/IRQ 前重查存取資格；op 為 NULL 時沒有舊工作 tuple，仍需檢查目前硬體狀態。 */
static bool gate(ToyPF *pf, unsigned vf, const ToyOperation *op)
{
    ToySlot *s = &pf->slots[vf];
    bool allowed = s->vf && !s->fenced && !s->killed && s->config_valid &&
                   pf->core.vf[vf].state == FW_ACTIVE && same_generation(pf, s, op) &&
                   (pci_get_word(s->vf->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER);
    if (!allowed) {
        toy_saturate(&s->fence_blocked);
    }
    return allowed;
}

enum ToyDmaRole { DMA_SQ, DMA_PAYLOAD_READ, DMA_PAYLOAD_WRITE, DMA_CQ, DMA_CQ_PHASE };

static bool dma(ToyPF *pf, unsigned vf, const ToyOperation *op,
                uint64_t addr, void *buffer, uint32_t length, bool write,
                enum ToyDmaRole role)
{
    ToySlot *s = &pf->slots[vf];
    MemTxResult result;
    if (!gate(pf, vf, op)) {
        return false;
    }
    /* 一律以該 VF 的 requester 進入 PCI DMA helper。 */
    result = write ? pci_dma_write(PCI_DEVICE(s->vf), addr, buffer, length) :
                     pci_dma_read(PCI_DEVICE(s->vf), addr, buffer, length);
    if (result != MEMTX_OK) {
        toy_saturate(&s->dma_fault);
        fprintf(stderr, "TOY_DMA_ERROR vf=%u rid=%04x addr=%" PRIx64 " length=%u write=%u\n",
                vf, pci_get_bdf(PCI_DEVICE(s->vf)), addr, length, write);
        return false;
    }
    /* 每次配置僅記錄各用途首筆成功 DMA，requester 與實際 helper 使用同一 VF。 */
    if (!(s->dma_trace_seen & (1u << role))) {
        static const char *const names[] = { "sq", "payload_read", "payload_write", "cq", "cq_phase" };
        s->dma_trace_seen |= 1u << role;
        fprintf(stderr, "TOY_DMA_OK vf=%u rid=%04x role=%s addr=%" PRIx64 " length=%u write=%u epoch=%" PRIu64 "\n",
                vf, pci_get_bdf(PCI_DEVICE(s->vf)), names[role], addr, length, write, s->epoch);
    }
    return true;
}

void toy_irq(ToyPF *pf, unsigned vf, unsigned vector, const ToyOperation *op)
{
    ToySlot *s = &pf->slots[vf];
    if (!gate(pf, vf, op)) {
        toy_saturate(&s->irq_suppressed);
        return;
    }
    s->irq_cause |= 1u << vector;
    msix_notify(PCI_DEVICE(s->vf), vector);
}

/* 延遲工作帶著提交當時的 session/epoch/token；先拒絕舊世代，再碰 payload 或 CQ。 */
void toy_complete(ToyPF *pf, unsigned vf, ToyOperation *op)
{
    ToySlot *s = &pf->slots[vf];
    uint8_t payload[4096], completion[32];
    uint64_t address;
    uint32_t done = 0;
    bool payload_fault = false;
    struct fw_cq cq;
    if (!op->valid) {
        return;
    }
    if (!same_generation(pf, s, op)) {
        toy_saturate(&s->stale_cpl);
        fprintf(stderr, "TOY_STALE_CPL vf=%u old_session=%" PRIu64 " live_session=%" PRIu64
                " old_epoch=%" PRIu64 " live_epoch=%" PRIu64 " old_token=%" PRIu64
                " live_token=%" PRIu64 " count=%u\n", vf, op->session, pf->session,
                op->epoch, s->epoch, op->token, s->token, s->stale_cpl);
        op->valid = false;
        return;
    }
    if (!gate(pf, vf, op)) {
        op->valid = false;
        return;
    }
    if (op->status == FW_OK) {
        if (op->sq.op == TOY_SQ_OPS_FILL) {
            memset(payload, (uint8_t)op->sq.arg, op->sq.len);
        }
        while (done < op->sq.len) {
            uint32_t chunk = MIN(op->sq.len - done, 4096 - ((op->sq.addr + done) & 4095));
            if (!dma(pf, vf, op, op->sq.addr + done, payload + done, chunk,
                     op->sq.op == TOY_SQ_OPS_FILL,
                     op->sq.op == TOY_SQ_OPS_FILL ? DMA_PAYLOAD_WRITE : DMA_PAYLOAD_READ)) {
                payload_fault = true;
                op->status = FW_DMA;
                break;
            }
            done += chunk;
        }
        if (!payload_fault && op->sq.op == TOY_SQ_OPS_CRC32) {
            op->result = fw_crc32(payload, op->sq.len);
        }
    }
    memset(&cq, 0, sizeof(cq));
    cq.id = op->sq.id;
    cq.status = op->status;
    cq.len = op->status == FW_BAD_DESC ? 0 : op->sq.len;
    cq.epoch = op->epoch;
    cq.result = op->result;
    cq.phase = fw_ring_phase(op->cq_index, s->config.cq_entries);
    if (fw_encode_cq(&cq, completion) != FW_OK) {
        toy_fail(pf, vf, TOY_LAST_FAULT_BAD_DESC);
        op->valid = false;
        return;
    }
    address = s->config.cq_base + (op->cq_index & (s->config.cq_entries - 1)) * 32;
    /* phase 為獨立最後一筆 DMA；CQ_TAIL 及 IRQ 僅在其成功後發布。 */
    if (!dma(pf, vf, op, address, completion, 24, true, DMA_CQ) ||
        !dma(pf, vf, op, address + 25, completion + 25, 7, true, DMA_CQ) ||
        !dma(pf, vf, op, address + 24, completion + 24, 1, true, DMA_CQ_PHASE)) {
        toy_fail(pf, vf, TOY_LAST_FAULT_DMA);
        op->valid = false;
        return;
    }
    s->cq_tail++;
    toy_irq(pf, vf, 0, op);
    op->valid = false;
    if (payload_fault) {
        toy_fail(pf, vf, TOY_LAST_FAULT_DMA);
    }
}

/* 每個 VF 最多保留一筆 delayed operation；CQ 已滿時不能再 fetch SQ，避免覆寫未消費結果。 */
static bool service_one(ToyPF *pf, unsigned vf)
{
    ToySlot *s = &pf->slots[vf];
    uint8_t bytes[32];
    ToyOperation op = { 0 };
    fw_status status;
    if (!gate(pf, vf, NULL) || s->pending.valid || s->sq_head == s->sq_tail ||
        (uint32_t)(s->cq_tail - s->cq_head) >= s->config.cq_entries) {
        return false;
    }
    if (!dma(pf, vf, NULL, s->config.sq_base + (s->sq_head & (s->config.sq_entries - 1)) * 32,
             bytes, sizeof(bytes), false, DMA_SQ)) {
        toy_fail(pf, vf, TOY_LAST_FAULT_DMA);
        return false;
    }
    s->sq_head++;
    op.valid = true;
    op.session = pf->session;
    op.epoch = s->epoch;
    op.token = s->token;
    op.cq_index = s->cq_tail;
    status = fw_decode_sq(bytes, &op.sq);
    /* epoch 在任何 payload/CQ 寫入前比較，與格式錯誤分開計數。 */
    if (ldq_le_p(bytes + 16) != s->epoch) {
        toy_saturate(&s->stale_desc);
        return true;
    }
    if (status != FW_OK) {
        op.sq.id = lduw_le_p(bytes + 14);
        op.sq.len = ldl_le_p(bytes + 8);
        op.status = FW_BAD_DESC;
        s->last_fault = TOY_LAST_FAULT_BAD_DESC;
    }
    pf->operations++;
    if (pf->delay_cpl_ticks && toy_fault_target(pf, vf)) {
        op.due = toy_tick() + pf->delay_cpl_ticks;
        s->pending = op;
        fprintf(stderr, "TOY_PENDING vf=%u epoch=%" PRIu64 " token=%" PRIu64
                " cq_index=%u due=%" PRIu64 "\n", vf, op.epoch, op.token, op.cq_index, op.due);
    } else {
        toy_complete(pf, vf, &op);
    }
    return true;
}

void toy_schedule(ToyPF *pf)
{
    if (pf->worker && !timer_pending(pf->worker)) {
        timer_mod(pf->worker, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

static void observe_kill(ToyPF *pf, unsigned vf)
{
    fw_status status = FW_OK;
    enum fw_state state = pf->core.vf[vf].state;
    if (!pf->slots[vf].killed || state == FW_FREE || state == FW_FAILED) {
        return;
    }
    /* MMIO KILL 只關硬體 gate；正常運作的 firmware 另以合法事件保留 ledger。 */
    if (state == FW_PROVISIONING) {
        status = fw_vf_event(&pf->core, vf, FW_ABORT_FAIL);
    } else if (state == FW_DESTROYING) {
        status = fw_vf_event(&pf->core, vf, FW_RELEASE_FAIL);
    } else {
        if (state == FW_ACTIVE) {
            /* KILL 可能未換 epoch；取得新 fence 才能送出 FENCE 事件。 */
            status = toy_fence(pf, vf, 0, false);
            if (status == FW_OK) {
                status = fw_vf_event(&pf->core, vf, FW_FENCE);
            }
        }
        if (status == FW_OK && pf->core.vf[vf].state == FW_QUIESCING) {
            status = fw_vf_event(&pf->core, vf, FW_DRAIN_OK);
        }
        if (status == FW_OK && pf->core.vf[vf].state == FW_RESETTING) {
            status = fw_vf_event(&pf->core, vf, FW_RESET_DONE);
        }
        if (status == FW_OK && pf->core.vf[vf].state == FW_RECOVERING) {
            status = fw_vf_event(&pf->core, vf, FW_VERIFY_FAIL);
        }
    }
    pf->event |= TOY_PF_EVENTS_VF_HEALTH;
    msix_notify(PCI_DEVICE(pf), 1);
    fprintf(stderr, "TOY_KILL_OBSERVED vf=%u status=%u state=%u\n", vf, status,
            pf->core.vf[vf].state);
}

void toy_worker(void *opaque)
{
    ToyPF *pf = opaque;
    fw_u32 backlog[FW_VFS] = { 0 }, service[FW_VFS] = { 0 };
    bool more = false;
    unsigned start = pf->core.round % FW_VFS;
    fw_status status;
    if (pf->fw_hang_all) {
        return;
    }
    for (unsigned i = 0; i < FW_VFS; i++) {
        ToySlot *s = &pf->slots[i];
        if (pf->fw_hang_vf && toy_fault_target(pf, i)) {
            continue;
        }
        observe_kill(pf, i);
        if (s->pending.valid) {
            if (s->pending.due <= toy_tick()) {
                toy_complete(pf, i, &s->pending);
            } else {
                more = true;
                continue;
            }
        }
        if (s->vf && !s->fenced && !s->killed && s->config_valid && pf->core.vf[i].state == FW_ACTIVE) {
            uint32_t pending = s->sq_tail - s->sq_head;
            uint32_t used = s->cq_tail - s->cq_head;
            if (pending > s->config.sq_entries || used > s->config.cq_entries) {
                toy_fail(pf, i, TOY_LAST_FAULT_BAD_QUEUE);
                continue;
            }
            backlog[i] = MIN(pending, s->config.cq_entries - used);
            if (pf->delay_cpl_ticks && toy_fault_target(pf, i)) {
                backlog[i] = MIN(backlog[i], 1);
            }
        }
    }
    status = fw_schedule_round(&pf->core, backlog, service);
    if (status != FW_OK) {
        fprintf(stderr, "TOY_SCHEDULER_ERROR status=%u\n", status);
        for (unsigned i = 0; i < FW_VFS; i++) {
            toy_kill(pf, i);
        }
        return;
    }
    for (unsigned round = 0; round < 64; round++) {
        for (unsigned offset = 0; offset < FW_VFS; offset++) {
            unsigned i = (start + offset) % FW_VFS;
            if (round < service[i]) {
                service_one(pf, i);
            }
        }
    }
    for (unsigned i = 0; i < FW_VFS; i++) {
        ToySlot *s = &pf->slots[i];
        more |= s->pending.valid || (!s->fenced && !s->killed && s->config_valid &&
                s->sq_head != s->sq_tail && s->cq_tail - s->cq_head < s->config.cq_entries &&
                !(pf->fw_hang_vf && toy_fault_target(pf, i)));
        more |= pf->core.vf[i].state == FW_QUIESCING || pf->core.vf[i].state == FW_RESETTING ||
                (s->vf && pf->core.vf[i].state == FW_RECOVERING);
    }
    status = fw_poll_deadlines(&pf->core);
    if (status != FW_OK && status != FW_TIMEOUT) {
        fprintf(stderr, "TOY_DEADLINE_ERROR status=%u\n", status);
    }
    toy_mbx_poll(pf);
    if (more) {
        toy_schedule(pf);
    }
}
