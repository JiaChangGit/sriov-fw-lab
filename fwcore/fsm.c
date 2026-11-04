#include "fw_internal.h"

struct edge { enum fw_state from; enum fw_event event; enum fw_state to; };
/* 此表只定義合法轉移；資源、硬體狀態及期限的 guard 在 fw_transition 中檢查。 */
static const struct edge edges[] = {
    {FW_FREE,FW_CREATE,FW_PROVISIONING},
    {FW_PROVISIONING,FW_COMMIT,FW_ACTIVE},
    {FW_PROVISIONING,FW_ABORT_OK,FW_FREE},
    {FW_PROVISIONING,FW_ABORT_FAIL,FW_FAILED},
    {FW_ACTIVE,FW_FENCE,FW_QUIESCING},
    {FW_QUIESCING,FW_DRAIN_OK,FW_RESETTING},
    {FW_QUIESCING,FW_EV_TIMEOUT,FW_FAILED},
    {FW_RESETTING,FW_RESET_DONE,FW_RECOVERING},
    {FW_RESETTING,FW_EV_TIMEOUT,FW_FAILED},
    {FW_RECOVERING,FW_READY_REARM,FW_ACTIVE},
    {FW_RECOVERING,FW_VERIFY_FAIL,FW_FAILED},
    {FW_ACTIVE,FW_FLR,FW_RECOVERING},
    {FW_QUIESCING,FW_FLR,FW_RECOVERING},
    {FW_RESETTING,FW_FLR,FW_RECOVERING},
    {FW_RECOVERING,FW_FLR,FW_RECOVERING},
    {FW_ACTIVE,FW_DESTROY,FW_DESTROYING},
    {FW_FAILED,FW_DESTROY,FW_DESTROYING},
    {FW_RECOVERING,FW_DESTROY,FW_DESTROYING},
    {FW_DESTROYING,FW_RELEASE_OK,FW_FREE},
    {FW_DESTROYING,FW_RELEASE_FAIL,FW_FAILED}
};

fw_status fw_transition(struct fw_context *ctx, fw_u16 vf, enum fw_event event)
{
    fw_size i;
    enum fw_state to=FW_STATE_COUNT;
    struct fw_hw_snapshot hw;
    fw_u64 deadline=0;
    fw_status rc;
    if (!ctx || vf>=FW_VFS) return FW_INVAL;
    /* 不合法的邊在任何 HAL 呼叫前拒絕，包含 context padding 都不更動。 */
    for (i=0; i<sizeof(edges)/sizeof(edges[0]); ++i) {
        if (edges[i].from==ctx->vf[vf].state && edges[i].event==event) { to=edges[i].to; break; }
    }
    if (to==FW_STATE_COUNT) return FW_STATE;
    if (event==FW_CREATE) return fw_create(ctx,vf,8);
    rc=fw_hw(ctx,vf,&hw); if (rc!=FW_OK) return rc;
    if (!hw.fenced) return FW_STATE;
    if (event==FW_FENCE || event==FW_DRAIN_OK || event==FW_FLR || event==FW_RESET_DONE) {
        rc=fw_deadline(ctx,(event==FW_FLR || event==FW_RESET_DONE)?1000:100,&deadline);
        if (rc!=FW_OK) return rc;
    }
    switch (event) {
    case FW_COMMIT:
        if (!ctx->vf[vf].reserve || hw.killed || hw.config_valid || hw.outstanding) return FW_STATE;
        break;
    case FW_ABORT_OK:
    case FW_RELEASE_OK:
        if (hw.pci_present || hw.config_valid || hw.outstanding ||
            !ctx->hal.owners_closed(ctx->hal.opaque,vf) || !ctx->vf[vf].reserve ||
            ctx->used_credit<ctx->vf[vf].reserve || !ctx->used_queues || ctx->used_vectors<2) return FW_STATE;
        ctx->used_credit-=ctx->vf[vf].reserve; --ctx->used_queues; ctx->used_vectors-=2;
        ctx->vf[vf].reserve=0;
        break;
    case FW_EV_TIMEOUT:
        if (ctx->hal.now_tick(ctx->hal.opaque)<ctx->vf[vf].deadline) return FW_STATE;
        /* fall through */
    case FW_ABORT_FAIL:
    case FW_VERIFY_FAIL:
    case FW_RELEASE_FAIL:
        rc=ctx->hal.kill(ctx->hal.opaque,vf); if (rc!=FW_OK) return rc;
        break;
    case FW_FENCE:
        if (hw.epoch<=ctx->vf[vf].epoch) return FW_STATE;
        rc=ctx->hal.cancel_io(ctx->hal.opaque,vf); if (rc!=FW_OK) return rc;
        break;
    case FW_DRAIN_OK:
        if (hw.outstanding) return FW_STATE;
        break;
    case FW_RESET_DONE:
        if (hw.outstanding) return FW_STATE;
        rc=ctx->hal.clear_queue(ctx->hal.opaque,vf); if (rc!=FW_OK) return rc;
        break;
    case FW_FLR:
        if (hw.epoch<=ctx->vf[vf].epoch || hw.outstanding || hw.config_valid) return FW_STATE;
        break;
    case FW_DESTROY:
        if (!ctx->hal.owners_closed(ctx->hal.opaque,vf) || hw.outstanding ||
            (ctx->vf[vf].state!=FW_FAILED && hw.epoch<=ctx->vf[vf].epoch)) return FW_STATE;
        rc=ctx->hal.clear_queue(ctx->hal.opaque,vf); if (rc!=FW_OK) return rc;
        break;
    default:
        return FW_STATE;
    }
    /* HAL 操作成功後才更新 core 狀態；失敗不能只改 state，假裝回收已完成。 */
    ctx->vf[vf].epoch=hw.epoch; ctx->vf[vf].deadline=deadline; ctx->vf[vf].state=to;
    fw_note(ctx,"vf_transition",vf,(fw_u64)to);
    return FW_OK;
}

fw_status fw_vf_event(struct fw_context *ctx, fw_u16 vf, enum fw_event event)
{
    struct fw_hw_snapshot hw;
    fw_status rc;
    /* READY 路徑與一般 transition 分層，timeout 收斂不再形成遞迴。 */
    if (event==FW_READY_REARM && ctx && vf<FW_VFS && ctx->vf[vf].state==FW_RECOVERING) {
        rc=fw_hw(ctx,vf,&hw);
        if (rc!=FW_OK) return rc;
        return fw_rearm(ctx,vf,hw.epoch,hw.ready_gen);
    }
    return fw_transition(ctx,vf,event);
}
