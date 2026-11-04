/* Wire 格式固定為 little-endian；逐 byte 編解碼，避免依賴 C struct padding 與未對齊存取。 */
#include "fw_core.h"

static fw_u64 get_le(const fw_u8 *p, unsigned n)
{
    fw_u64 value=0;
    unsigned i;
    for (i=0; i<n; ++i) value|=(fw_u64)p[i]<<(8U*i);
    return value;
}

static void put_le(fw_u8 *p, fw_u64 value, unsigned n)
{
    unsigned i;
    for (i=0; i<n; ++i) p[i]=(fw_u8)(value>>(8U*i));
}

static fw_status validate_sq(const struct fw_sq *sq)
{
    if (!sq->len || sq->len>4096 || sq->flags || (sq->op!=1 && sq->op!=2) ||
        sq->addr>FW_U64_MAX-(fw_u64)sq->len) return FW_BAD_DESC;
    return FW_OK;
}

fw_status fw_decode_sq(const fw_u8 bytes[32], struct fw_sq *out)
{
    struct fw_sq sq;
    fw_status rc;
    if (!bytes || !out) return FW_INVAL;
    fw_zero(&sq,sizeof(sq));
    sq.addr=get_le(bytes,8); sq.len=(fw_u32)get_le(bytes+8,4);
    sq.op=bytes[12]; sq.flags=bytes[13]; sq.id=(fw_u16)get_le(bytes+14,2);
    sq.epoch=get_le(bytes+16,8); sq.arg=get_le(bytes+24,8);
    rc=validate_sq(&sq); if (rc!=FW_OK) return rc;
    *out=sq;
    return FW_OK;
}

fw_status fw_encode_sq(const struct fw_sq *sq, fw_u8 bytes[32])
{
    fw_status rc;
    if (!sq || !bytes) return FW_INVAL;
    rc=validate_sq(sq); if (rc!=FW_OK) return rc;
    put_le(bytes,sq->addr,8); put_le(bytes+8,sq->len,4);
    bytes[12]=sq->op; bytes[13]=sq->flags; put_le(bytes+14,sq->id,2);
    put_le(bytes+16,sq->epoch,8); put_le(bytes+24,sq->arg,8);
    return FW_OK;
}

fw_status fw_decode_cq(const fw_u8 bytes[32], struct fw_cq *out)
{
    struct fw_cq cq;
    unsigned i;
    if (!bytes || !out) return FW_INVAL;
    for (i=25; i<32; ++i) if (bytes[i]) return FW_BAD_DESC;
    if (bytes[24]>1 || get_le(bytes+2,2)>FW_IO || get_le(bytes+4,4)>4096) return FW_BAD_DESC;
    fw_zero(&cq,sizeof(cq));
    cq.id=(fw_u16)get_le(bytes,2); cq.status=(fw_u16)get_le(bytes+2,2);
    cq.len=(fw_u32)get_le(bytes+4,4); cq.epoch=get_le(bytes+8,8);
    cq.result=get_le(bytes+16,8); cq.phase=bytes[24];
    *out=cq;
    return FW_OK;
}

fw_status fw_encode_cq(const struct fw_cq *cq, fw_u8 bytes[32])
{
    if (!cq || !bytes) return FW_INVAL;
    if (cq->phase>1 || cq->status>FW_IO || cq->len>4096) return FW_BAD_DESC;
    fw_zero(bytes,32);
    put_le(bytes,cq->id,2); put_le(bytes+2,cq->status,2); put_le(bytes+4,cq->len,4);
    put_le(bytes+8,cq->epoch,8); put_le(bytes+16,cq->result,8); bytes[24]=cq->phase;
    return FW_OK;
}

static fw_u8 valid_entries(fw_u32 n)
{
    return (fw_u8)(n>=16 && n<=256 && !(n&(n-1U)));
}

/* 驗證 queue 大小、對齊、世代及 SQ/CQ 位址範圍；不在這裡建立 IOMMU mapping。 */
fw_status fw_queue_validate(const struct fw_queue_config *cfg, fw_u64 session, fw_u64 epoch)
{
    fw_u64 bytes, sq_end, cq_end;
    if (!cfg) return FW_INVAL;
    if (!valid_entries(cfg->sq_entries) || cfg->cq_entries!=cfg->sq_entries ||
        (cfg->sq_base&63U) || (cfg->cq_base&63U) || cfg->epoch!=epoch ||
        cfg->session!=session || !session || !epoch) return FW_BAD_QUEUE;
    bytes=(fw_u64)cfg->sq_entries*32U;
    if (cfg->sq_base>FW_U64_MAX-bytes || cfg->cq_base>FW_U64_MAX-bytes) return FW_BAD_QUEUE;
    sq_end=cfg->sq_base+bytes; cq_end=cfg->cq_base+bytes;
    if (cfg->sq_base<cq_end && cfg->cq_base<sq_end) return FW_BAD_QUEUE;
    return FW_OK;
}

/* u32 減法允許 counter 回繞；有效差值仍必須小於或等於 ring 容量。 */
fw_status fw_ring_pending(fw_u32 tail, fw_u32 head, fw_u32 entries, fw_u32 *pending)
{
    fw_u32 count=tail-head;
    if (!pending) return FW_INVAL;
    if (!valid_entries(entries) || count>entries) return FW_BAD_QUEUE;
    *pending=count;
    return FW_OK;
}

fw_u8 fw_ring_phase(fw_u32 counter, fw_u32 entries)
{
    if (!valid_entries(entries)) return 0;
    return (fw_u8)(1U^((counter/entries)&1U));
}
