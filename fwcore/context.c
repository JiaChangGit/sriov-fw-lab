/* Core context 不持有 OS 資源；時間、裝置快照與 gate 操作都由呼叫者提供的 HAL 執行。 */
#include "fw_internal.h"

void fw_zero(void *ptr, fw_size size)
{
    fw_u8 *p = (fw_u8 *)ptr;
    fw_size i;
    for (i = 0; i < size; ++i) p[i] = 0;
}

void fw_copy(void *dst, const void *src, fw_size size)
{
    fw_u8 *d = (fw_u8 *)dst;
    const fw_u8 *s = (const fw_u8 *)src;
    fw_size i;
    for (i = 0; i < size; ++i) d[i] = s[i];
}

fw_status fw_hw(const struct fw_context *ctx, fw_u16 vf, struct fw_hw_snapshot *out)
{
    fw_status rc;
    if (!ctx || !out || vf >= FW_VFS || !ctx->hal.read_hw) return FW_INVAL;
    fw_zero(out, sizeof(*out));
    rc = ctx->hal.read_hw(ctx->hal.opaque, vf, out);
    if (rc != FW_OK) return rc;
    if (out->session != ctx->session || out->epoch < ctx->vf[vf].epoch) return FW_STATE;
    return FW_OK;
}

void fw_note(struct fw_context *ctx, const char *message, fw_u16 vf, fw_u64 value)
{
    if (ctx->hal.log) ctx->hal.log(ctx->hal.opaque, message, vf, value);
}

fw_status fw_deadline(const struct fw_context *ctx, fw_u64 delta, fw_u64 *out)
{
    fw_u64 now = ctx->hal.now_tick(ctx->hal.opaque);
    if (now > FW_U64_MAX - delta) return FW_OVERFLOW;
    *out = now + delta;
    return FW_OK;
}

/* 先在區域變數建立完整 context；任一 VF 快照不符時，不覆寫呼叫者的舊 context。 */
fw_status fw_init(struct fw_context *ctx, const struct fw_hal *hal)
{
    struct fw_context fresh;
    struct fw_hw_snapshot hw;
    fw_u16 i;
    fw_status rc;
    if (!ctx || !hal || !hal->now_tick || !hal->read_hw || !hal->cancel_io ||
        !hal->clear_queue || !hal->set_gate || !hal->kill || !hal->owners_closed) return FW_INVAL;
    fw_zero(&fresh, sizeof(fresh));
    fresh.hal = *hal;
    for (i = 0; i < FW_VFS; ++i) {
        fw_zero(&hw, sizeof(hw));
        rc = hal->read_hw(hal->opaque, i, &hw);
        if (rc != FW_OK) return rc;
        if (!hw.session || !hw.epoch || !hw.fenced || hw.config_valid || hw.outstanding)
            return FW_STATE;
        if (i == 0) fresh.session = hw.session;
        if (hw.session != fresh.session) return FW_STATE;
        fresh.vf[i].epoch = hw.epoch;
    }
    *ctx = fresh;
    return FW_OK;
}

/* 讀完所有 VF 才複製到 out，讓呼叫者不會收到半份成功的 snapshot。 */
fw_status fw_snapshot(const struct fw_context *ctx, struct fw_snapshot *out)
{
    struct fw_snapshot snap;
    fw_u16 i;
    fw_status rc;
    if (!ctx || !out) return FW_INVAL;
    fw_zero(&snap, sizeof(snap));
    snap.session=ctx->session; snap.round=ctx->round;
    snap.used_queues=ctx->used_queues; snap.used_vectors=ctx->used_vectors;
    snap.used_credit=ctx->used_credit; snap.last_seq=ctx->last_seq;
    snap.negotiated=ctx->negotiated; snap.cache_valid=ctx->cache_valid;
    for (i=0; i<FW_VFS; ++i) {
        snap.vf[i]=ctx->vf[i];
        rc=fw_hw(ctx,i,&snap.hw[i]);
        if (rc!=FW_OK) return rc;
    }
    *out=snap;
    return FW_OK;
}

/* 由各 VF 重新加總 ledger，並檢查 FREE/open-gate 等跨欄位條件。 */
fw_status fw_check_invariants(const struct fw_context *ctx)
{
    fw_u32 queues=0, credit=0;
    fw_u16 i;
    struct fw_hw_snapshot hw;
    fw_status rc;
    if (!ctx || !ctx->session) return FW_INVAL;
    for (i=0; i<FW_VFS; ++i) {
        if (ctx->vf[i].state >= FW_STATE_COUNT || ctx->vf[i].reserve > FW_CAP_CREDIT) return FW_STATE;
        rc=fw_hw(ctx,i,&hw);
        if (rc!=FW_OK) return rc;
        if (hw.outstanding>1) return FW_STATE;
        if (ctx->vf[i].state==FW_FREE && (ctx->vf[i].reserve || hw.config_valid || hw.outstanding)) return FW_STATE;
        if (ctx->vf[i].reserve) { ++queues; credit+=ctx->vf[i].reserve; }
        if (!hw.fenced && (hw.killed || !hw.config_valid || !hw.bme || ctx->vf[i].state!=FW_ACTIVE)) return FW_STATE;
    }
    if (credit>FW_CAP_CREDIT || queues!=ctx->used_queues || 2U*queues!=ctx->used_vectors || credit!=ctx->used_credit)
        return FW_STATE;
    return FW_OK;
}

fw_status fw_global_recover(struct fw_context *ctx, fw_u64 new_session, const fw_u64 epochs[FW_VFS])
{
    struct fw_hw_snapshot hw;
    fw_u16 i;
    fw_status rc;
    if (!ctx || !epochs || !new_session) return FW_INVAL;
    if (ctx->session==FW_U64_MAX) return FW_OVERFLOW;
    if (new_session<=ctx->session || !ctx->hal.owners_closed(ctx->hal.opaque, FW_GLOBAL)) return FW_STATE;
    for (i=0; i<FW_VFS; ++i) {
        if (ctx->vf[i].epoch==FW_U64_MAX) return FW_OVERFLOW;
        rc=ctx->hal.read_hw(ctx->hal.opaque,i,&hw);
        if (rc!=FW_OK) return rc;
        if (epochs[i]<=ctx->vf[i].epoch || hw.epoch!=epochs[i] || hw.session!=new_session ||
            !hw.fenced || hw.config_valid || hw.outstanding || hw.pci_present) return FW_STATE;
    }
    /* 只有完成全部前置驗證後才提交，kill 仍由硬體保留到 end_global。 */
    for (i=0; i<FW_VFS; ++i) {
        fw_zero(&ctx->vf[i],sizeof(ctx->vf[i]));
        ctx->vf[i].epoch=epochs[i];
    }
    ctx->session=new_session; ctx->round=0;
    ctx->used_queues=0; ctx->used_vectors=0; ctx->used_credit=0;
    ctx->last_seq=0; ctx->negotiated=0; ctx->cache_valid=0;
    ctx->mailbox_pending=0; ctx->pending_epoch=0; ctx->pending_deadline=0;
    fw_zero(&ctx->cached_request,sizeof(ctx->cached_request));
    fw_zero(&ctx->cached_reply,sizeof(ctx->cached_reply));
    fw_note(ctx,"global_recovered",FW_GLOBAL,new_session);
    return FW_OK;
}

fw_status fw_poll_deadlines(struct fw_context *ctx)
{
    fw_u16 i;
    fw_u64 now;
    fw_status rc=FW_OK, one;
    struct fw_hw_snapshot hw;
    if (!ctx) return FW_INVAL;
    now=ctx->hal.now_tick(ctx->hal.opaque);
    for (i=0; i<FW_VFS; ++i) {
        enum fw_state s=ctx->vf[i].state;
        if ((s==FW_QUIESCING || s==FW_RESETTING || s==FW_RECOVERING) && now>=ctx->vf[i].deadline) {
            /* PCI 移除後已無 READY 端點；等待 DESTROY，保留 quota 與 fence。 */
            if (s==FW_RECOVERING) {
                one=fw_hw(ctx,i,&hw);
                if (one!=FW_OK) { rc=one; continue; }
                if (!hw.pci_present) continue;
            }
            one=fw_vf_event(ctx,i,s==FW_RECOVERING?FW_VERIFY_FAIL:FW_EV_TIMEOUT);
            if (one!=FW_OK) rc=one;
        }
    }
    return rc;
}
