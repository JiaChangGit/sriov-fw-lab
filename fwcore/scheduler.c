/* 每輪先按可執行 VF 的需求分配 reserve，再按權重分配剩餘 credit；運算結果不代表 DMA 已完成。 */
#include "fw_internal.h"

fw_status fw_schedule_round(struct fw_context *ctx, const fw_u32 backlog[FW_VFS], fw_u32 service[FW_VFS])
{
    fw_u32 demand[FW_VFS], allocation[FW_VFS], remainders[FW_VFS];
    fw_u32 left=FW_CAP_CREDIT, weights, floor_sum, available;
    fw_u16 i, j, winner;
    unsigned pass;
    fw_u8 capped;
    struct fw_hw_snapshot hw;
    fw_status rc;
    if (!ctx || !backlog || !service) return FW_INVAL;
    if (ctx->round==FW_U64_MAX) return FW_OVERFLOW;
    for (i=0; i<FW_VFS; ++i) {
        rc=fw_hw(ctx,i,&hw); if (rc!=FW_OK) return rc;
        demand[i]=(ctx->vf[i].state==FW_ACTIVE && !hw.fenced && !hw.killed && hw.bme &&
            hw.config_valid && !hw.outstanding)?backlog[i]:0;
        allocation[i]=demand[i]<ctx->vf[i].reserve?demand[i]:ctx->vf[i].reserve;
        if (allocation[i]>left || (demand[i] && !ctx->vf[i].reserve)) return FW_STATE;
        left-=allocation[i]; demand[i]-=allocation[i];
    }
    /* 至多移出八個 VF；沒有飽和者時一次 floor/remainder 即完成。 */
    for (pass=0; pass<FW_VFS+1U && left; ++pass) {
        weights=0;
        for (i=0; i<FW_VFS; ++i) if (demand[i]) weights+=ctx->vf[i].reserve;
        if (!weights) break;
        capped=0; available=left;
        for (i=0; i<FW_VFS; ++i) {
            if (demand[i] && ((fw_u64)available*ctx->vf[i].reserve)/weights>=demand[i]) {
                allocation[i]+=demand[i]; left-=demand[i]; demand[i]=0; capped=1;
            }
        }
        if (capped) continue;
        /* 無 VF 先達需求上限時，用整數商分配，再依餘數補足；同餘數輪替優先順序。 */
        floor_sum=0;
        for (i=0; i<FW_VFS; ++i) {
            fw_u64 product=demand[i]?(fw_u64)left*ctx->vf[i].reserve:0;
            fw_u32 whole=(fw_u32)(product/weights);
            allocation[i]+=whole; demand[i]-=whole; floor_sum+=whole;
            remainders[i]=(fw_u32)(product%weights);
        }
        left-=floor_sum;
        for (j=0; j<FW_VFS && left; ++j) {
            winner=FW_VFS;
            for (i=0; i<FW_VFS; ++i) {
                if (!demand[i] || !remainders[i]) continue;
                if (winner==FW_VFS || remainders[i]>remainders[winner] ||
                    (remainders[i]==remainders[winner] &&
                     (i+FW_VFS-(fw_u32)(ctx->round%FW_VFS))%FW_VFS<
                     (winner+FW_VFS-(fw_u32)(ctx->round%FW_VFS))%FW_VFS)) winner=i;
            }
            if (winner==FW_VFS) break;
            ++allocation[winner]; --demand[winner]; --left; remainders[winner]=0;
        }
        break;
    }
    /* 先檢查全部累計值是否溢位，再發布 service，避免只更新一部分 VF。 */
    for (i=0; i<FW_VFS; ++i)
        if (ctx->vf[i].serviced>FW_U64_MAX-allocation[i]) return FW_OVERFLOW;
    for (i=0; i<FW_VFS; ++i) { service[i]=allocation[i]; ctx->vf[i].serviced+=allocation[i]; }
    ++ctx->round;
    return FW_OK;
}
