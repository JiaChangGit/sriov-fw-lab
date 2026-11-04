/* Mailbox 協定：先驗 request，再執行交易；同一 session 只快取最近一筆已接受的 request。 */
#include "fw_internal.h"

static fw_u8 same_request(const struct fw_request *a, const struct fw_request *b)
{
    unsigned i;
    if (a->session!=b->session || a->seq!=b->seq || a->opcode!=b->opcode || a->target!=b->target) return 0;
    for (i=0; i<4; ++i) if (a->arg[i]!=b->arg[i]) return 0;
    return 1;
}

static fw_status shape(const struct fw_context *ctx, const struct fw_request *req)
{
    unsigned i, used=0;
    if (req->opcode<FW_VERSION_NEG || req->opcode>FW_GLOBAL_RESET) return FW_UNSUP;
    if (req->opcode==FW_VERSION_NEG) {
        if (req->target!=FW_GLOBAL || req->arg[3] || req->arg[1]>req->arg[2]) return FW_INVAL;
        if (req->arg[0]!=1 || req->arg[1]>0) return FW_UNSUP;
        return FW_OK;
    }
    if (!ctx->negotiated) return FW_NO_NEG;
    if (req->opcode==FW_GLOBAL_RESET) {
        if (req->target!=FW_GLOBAL) return FW_INVAL;
        if (!ctx->hal.begin_global || !ctx->hal.end_global) return FW_UNSUP;
    } else if (req->target>=FW_VFS) return FW_INVAL;
    if (req->opcode==FW_PROVISION || req->opcode==FW_SET_RESERVE) {
        used=1;
        if (!req->arg[0] || req->arg[0]>FW_CAP_CREDIT) return FW_INVAL;
    }
    if (req->opcode==FW_REARM) { used=3; if (!req->arg[2]) return FW_INVAL; }
    for (i=used; i<4; ++i) if (req->arg[i]) return FW_INVAL;
    return FW_OK;
}

static fw_status run_checkpoint(struct fw_context *ctx, fw_u16 vf, enum fw_checkpoint point)
{
    return ctx->hal.checkpoint ? ctx->hal.checkpoint(ctx->hal.opaque,vf,point) : FW_OK;
}

/* 釋放失敗時走 FAILED 並保留配額，避免後續配置重用尚未確認回收的資源。 */
static void rollback(struct fw_context *ctx, fw_u16 vf, fw_u8 destroying)
{
    fw_status rc=run_checkpoint(ctx,vf,destroying?FW_CP_DESTROY_RELEASE:FW_CP_ABORT_RELEASE);
    if (rc==FW_OK) rc=fw_vf_event(ctx,vf,destroying?FW_RELEASE_OK:FW_ABORT_OK);
    fw_note(ctx,destroying?"destroy_release_status":"provision_abort_status",vf,rc);
    if (rc!=FW_OK) {
        rc=fw_vf_event(ctx,vf,destroying?FW_RELEASE_FAIL:FW_ABORT_FAIL);
        fw_note(ctx,"rollback_quarantine_status",vf,rc);
    }
}

static fw_status progress(struct fw_context *ctx, fw_u16 vf, fw_u32 opcode,
                          fw_u64 epoch, fw_u64 deadline)
{
    enum fw_state expected=opcode==FW_QUIESCE?FW_QUIESCING:FW_RESETTING;
    fw_status rc;
    if (ctx->vf[vf].epoch!=epoch) return FW_STATE;
    if (ctx->vf[vf].state==FW_FAILED && ctx->hal.now_tick(ctx->hal.opaque)>=deadline) return FW_TIMEOUT;
    if (ctx->vf[vf].state!=expected) return FW_STATE;
    /* 期限優先於晚到的完成回呼，100 tick 的邊界不依 host timeout。 */
    if (ctx->hal.now_tick(ctx->hal.opaque)>=deadline) {
        rc=fw_vf_event(ctx,vf,FW_EV_TIMEOUT);
        return rc==FW_OK?FW_TIMEOUT:rc;
    }
    rc=ctx->hal.mailbox_progress?ctx->hal.mailbox_progress(ctx->hal.opaque,vf,opcode):FW_OK;
    if (rc!=FW_OK) return rc;
    return fw_vf_event(ctx,vf,opcode==FW_QUIESCE?FW_DRAIN_OK:FW_RESET_DONE);
}

fw_status fw_poll_mailbox(struct fw_context *ctx, struct fw_reply *reply)
{
    fw_status rc;
    if (!ctx || !reply) return FW_INVAL;
    fw_zero(reply,sizeof(*reply));
    if (!ctx->mailbox_pending) return FW_STATE;
    rc=progress(ctx,(fw_u16)ctx->cached_request.target,ctx->cached_request.opcode,
                ctx->pending_epoch,ctx->pending_deadline);
    ctx->cached_reply.status=rc;
    *reply=ctx->cached_reply;
    if (rc!=FW_BUSY) {
        ctx->mailbox_pending=0; ctx->pending_epoch=0; ctx->pending_deadline=0;
        fw_note(ctx,"mailbox_completed",(fw_u16)ctx->cached_request.target,rc);
    }
    return rc;
}

fw_status fw_handle_mailbox(struct fw_context *ctx, const struct fw_request *req, struct fw_reply *reply)
{
    fw_status rc;
    fw_u16 vf;
    struct fw_hw_snapshot hw;
    fw_u64 session, epochs[FW_VFS];
    if (!ctx || !req || !reply) return FW_INVAL;
    fw_zero(reply,sizeof(*reply)); reply->seq=req->seq;
    if (!req->seq) { reply->status=FW_INVAL; return FW_INVAL; }
    if (req->session!=ctx->session) { reply->status=FW_STALE_SEQ; return FW_STALE_SEQ; }
    /* 重送必須逐欄相同；同序號卻改內容是衝突，不能再次執行副作用。 */
    if (ctx->cache_valid && req->seq==ctx->last_seq) {
        if (!same_request(req,&ctx->cached_request)) { reply->status=FW_SEQ_CONFLICT; return FW_SEQ_CONFLICT; }
        *reply=ctx->cached_reply; return reply->status;
    }
    if (req->seq<=ctx->last_seq) { reply->status=FW_STALE_SEQ; return FW_STALE_SEQ; }
    rc=shape(ctx,req);
    if (rc!=FW_OK) { reply->status=rc; return rc; }
    /* 尚在等 quiesce/reset 的交易由 poll 推進；新 request 不可蓋掉它的 cache。 */
    if (ctx->mailbox_pending) { reply->status=FW_BUSY; return FW_BUSY; }
    vf=(fw_u16)req->target;
    switch (req->opcode) {
    case FW_VERSION_NEG:
        ctx->negotiated=1; reply->value[0]=1U<<16; rc=FW_OK;
        break;
    case FW_GET_STATE:
        rc=fw_hw(ctx,vf,&hw);
        if (rc==FW_OK) {
            reply->value[0]=(fw_u32)ctx->vf[vf].state;
            reply->value[1]=(fw_u32)hw.epoch; reply->value[2]=(fw_u32)(hw.epoch>>32);
            reply->value[3]=(fw_u32)hw.fenced|((fw_u32)hw.killed<<1)|((fw_u32)hw.pci_present<<2)|
                ((fw_u32)hw.bme<<3)|((fw_u32)hw.config_valid<<4)|((fw_u32)hw.ready_pending<<5);
        }
        break;
    case FW_PROVISION:
        rc=run_checkpoint(ctx,vf,FW_CP_BEFORE_CREATE);
        if (rc==FW_OK) rc=fw_create(ctx,vf,req->arg[0]);
        if (rc==FW_OK) {
            rc=run_checkpoint(ctx,vf,FW_CP_AFTER_CREATE);
            if (rc==FW_OK) rc=run_checkpoint(ctx,vf,FW_CP_BEFORE_COMMIT);
            if (rc==FW_OK) rc=fw_vf_event(ctx,vf,FW_COMMIT);
            if (rc!=FW_OK) rollback(ctx,vf,0);
        }
        break;
    case FW_QUIESCE:
        rc=fw_vf_event(ctx,vf,FW_FENCE);
        if (rc==FW_OK) rc=progress(ctx,vf,req->opcode,ctx->vf[vf].epoch,ctx->vf[vf].deadline);
        break;
    case FW_RESET:
        rc=progress(ctx,vf,req->opcode,ctx->vf[vf].epoch,ctx->vf[vf].deadline);
        break;
    case FW_REARM:
        rc=fw_rearm(ctx,vf,(fw_u64)req->arg[0]|((fw_u64)req->arg[1]<<32),req->arg[2]);
        break;
    case FW_DESTROY_CMD:
        rc=fw_vf_event(ctx,vf,FW_DESTROY);
        if (rc==FW_OK) {
            rc=run_checkpoint(ctx,vf,FW_CP_DESTROY_RELEASE);
            if (rc==FW_OK) rc=fw_vf_event(ctx,vf,FW_RELEASE_OK);
            fw_note(ctx,"destroy_release_status",vf,rc);
            if (rc!=FW_OK) {
                fw_status failed=fw_vf_event(ctx,vf,FW_RELEASE_FAIL);
                fw_note(ctx,"rollback_quarantine_status",vf,failed);
            }
        }
        break;
    case FW_SET_RESERVE: rc=fw_set_reserve(ctx,vf,req->arg[0]); break;
    case FW_GLOBAL_RESET:
        if (!ctx->hal.owners_closed(ctx->hal.opaque,FW_GLOBAL)) { rc=FW_STATE; break; }
        rc=ctx->hal.begin_global(ctx->hal.opaque,&session,epochs);
        if (rc!=FW_OK) break;
        rc=fw_global_recover(ctx,session,epochs);
        if (rc!=FW_OK) { reply->status=rc; return rc; }
        rc=ctx->hal.end_global(ctx->hal.opaque);
        reply->status=rc; reply->value[0]=(fw_u32)session; reply->value[1]=(fw_u32)(session>>32);
        /* 舊 session 的 transaction 不能重新污染剛清空的協商與 cache。 */
        return rc;
    default: rc=FW_UNSUP; break;
    }
    reply->status=rc; ctx->last_seq=req->seq; ctx->cache_valid=1;
    ctx->cached_request=*req; ctx->cached_reply=*reply;
    if (rc==FW_BUSY && (req->opcode==FW_QUIESCE || req->opcode==FW_RESET)) {
        ctx->mailbox_pending=1;
        ctx->pending_epoch=ctx->vf[vf].epoch; ctx->pending_deadline=ctx->vf[vf].deadline;
    }
    return rc;
}
