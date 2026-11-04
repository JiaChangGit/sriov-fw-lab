/* 資源准入與重新開啟 DMA gate。reserve 記在 core ledger，硬體條件由 HAL snapshot 提供。 */
#include "fw_internal.h"

/* 先確認 VF 與硬體都可配置，再一次扣除 queue、vector 和 credit 配額。 */
fw_status fw_create(struct fw_context *ctx, fw_u16 vf, fw_u32 reserve)
{
    struct fw_hw_snapshot hw;
    fw_status rc;
    if (!ctx || vf>=FW_VFS || !reserve || reserve>FW_CAP_CREDIT) return FW_INVAL;
    if (ctx->vf[vf].state!=FW_FREE) return FW_STATE;
    rc=fw_hw(ctx,vf,&hw); if (rc!=FW_OK) return rc;
    if (!hw.fenced || hw.killed || hw.config_valid || hw.outstanding || hw.pci_present) return FW_STATE;
    if (ctx->used_queues>=FW_VFS || ctx->used_vectors>14 || ctx->used_credit>FW_CAP_CREDIT-reserve) return FW_NOSPC;
    ctx->vf[vf].state=FW_PROVISIONING;
    ctx->vf[vf].reserve=reserve; ctx->vf[vf].epoch=hw.epoch;
    ++ctx->used_queues; ctx->used_vectors+=2; ctx->used_credit+=reserve;
    fw_note(ctx,"vf_reserved",vf,reserve);
    return FW_OK;
}

/* 只允許 fenced 且沒有未完成工作的 VF 調整配額；先扣舊值再檢查新總量。 */
fw_status fw_set_reserve(struct fw_context *ctx, fw_u16 vf, fw_u32 reserve)
{
    struct fw_hw_snapshot hw;
    fw_u32 remaining;
    fw_status rc;
    if (!ctx || vf>=FW_VFS || !reserve || reserve>FW_CAP_CREDIT) return FW_INVAL;
    if (ctx->vf[vf].state!=FW_ACTIVE && ctx->vf[vf].state!=FW_RECOVERING) return FW_STATE;
    rc=fw_hw(ctx,vf,&hw); if (rc!=FW_OK) return rc;
    if (!hw.fenced || hw.outstanding || hw.killed) return FW_STATE;
    if (ctx->used_credit<ctx->vf[vf].reserve) return FW_STATE;
    remaining=ctx->used_credit-ctx->vf[vf].reserve;
    if (remaining>FW_CAP_CREDIT-reserve) return FW_NOSPC;
    ctx->used_credit=remaining+reserve; ctx->vf[vf].reserve=reserve;
    return FW_OK;
}

/* CONFIG 完成不等於允許 DMA；epoch、ready_gen、BME 與期限都符合後才開 gate。 */
fw_status fw_rearm(struct fw_context *ctx, fw_u16 vf, fw_u64 epoch, fw_u32 ready_gen)
{
    struct fw_hw_snapshot hw;
    fw_status rc;
    if (!ctx || vf>=FW_VFS || !ready_gen) return FW_INVAL;
    if (ctx->vf[vf].state!=FW_ACTIVE && ctx->vf[vf].state!=FW_RECOVERING) return FW_STATE;
    rc=fw_hw(ctx,vf,&hw); if (rc!=FW_OK) return rc;
    if (hw.epoch!=epoch || ctx->vf[vf].epoch!=epoch || !hw.config_valid || !hw.ready_pending ||
        hw.ready_gen!=ready_gen || hw.killed || !hw.fenced || !hw.bme || hw.outstanding) return FW_STATE;
    if (ctx->vf[vf].state==FW_RECOVERING && ctx->hal.now_tick(ctx->hal.opaque)>=ctx->vf[vf].deadline) {
        rc=fw_transition(ctx,vf,FW_VERIFY_FAIL);
        return rc==FW_OK?FW_TIMEOUT:rc;
    }
    rc=ctx->hal.set_gate(ctx->hal.opaque,vf,1);
    if (rc!=FW_OK) return rc;
    ctx->vf[vf].state=FW_ACTIVE; ctx->vf[vf].deadline=0;
    fw_note(ctx,"vf_rearmed",vf,epoch);
    return FW_OK;
}
