/* Reset 測試的 owner 端：建立真正未完成的 SQ，並保留已撤映射的舊 allocation 供 guard 比對。 */
#include "owner.h"
#include "../common/lab_wire.h"
#include "../common/lab_ring.h"
#include "toy_regs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* SQ_HEAD 必須前進而 CQ_TAIL 不變，才能證明 descriptor 已被取走且尚未完成。 */
int owner_submit_pending(struct owner *o)
{
    if (o->pending_request) { errno=EBUSY; return -1; }
    uint32_t index=lab_ring_index(o->sq_tail,o->entries),head,tail,digests[OWNER_MAPS];
    unsigned char *sq=(unsigned char *)o->maps[OWNER_SQ].memory+index*32;
    if (owner_freeze(o)) return -1;
    memset(sq,0,32); lab_put64(sq,o->maps[OWNER_PAYLOAD].iova);
    lab_put32(sq+8,8); sq[12]=TOY_SQ_OPS_FILL; lab_put16(sq+14,0x708);
    lab_put64(sq+16,o->epoch); lab_put64(sq+24,0x79);
    char hex[65]; static const char digits[]="0123456789abcdef";
    for (unsigned i=0;i<32;i++) { hex[2*i]=digits[sq[i]>>4]; hex[2*i+1]=digits[sq[i]&15]; }
    hex[64]=0; ++o->sq_tail;
    if (owner_write32(o,TOY_VF_SQ_TAIL,o->sq_tail)||owner_write32(o,TOY_VF_DOORBELL,1)) return -1;
    int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
    for (;;) {
        if (owner_read32(o,TOY_VF_SQ_HEAD,&head)||owner_read32(o,TOY_VF_CQ_TAIL,&tail)) return -1;
        if (tail!=o->cq_head) { errno=EPROTO; return -1; }
        if (head==o->sq_tail) break;
        if (lab_now_ms()>=deadline) { errno=ETIMEDOUT; return -1; }
        usleep(1000);
    }
    for (unsigned i=OWNER_CQ;i<OWNER_MAPS;i++)
        if (memcmp(o->held[i],o->maps[i].memory,o->maps[i].size)) { errno=EILSEQ; return -1; }
    if (owner_freeze(o)||owner_unchanged(o,digests)) return -1;
    o->pending_request=true; o->pending_epoch=o->epoch;
    printf("{\"event\":\"RECOVERY_PENDING\",\"case\":%u,\"pid\":%d,\"epoch\":%" PRIu64 ",\"session\":%" PRIu64 ",\"sq_head\":%u,\"cq_tail\":%u,\"completed\":%" PRIu64 ",\"descriptor_hex\":\"%s\",\"payload_unchanged\":true,\"cq_unchanged\":true}\n",o->recovery_case,getpid(),o->epoch,o->session,head,tail,o->completed,hex);
    return 0;
}

/* 管理端已完成 fence/reset 握手才可呼叫；keep_unmapped 只保留 CPU 記憶體，不保留 IOMMU mapping。 */
int owner_retire(struct owner *o)
{
    for (unsigned i=0;i<OWNER_MAPS;i++)
        if (o->retired[i].memory||!o->maps[i].memory||!o->held[i]) { errno=EBUSY; return -1; }
    /* 舊 IOVA 精確解除後仍保留 CPU allocations，供真實 stale callback 前後比對。 */
    o->keep_unmapped=true;
    int rc=owner_unmap(o,true);
    o->keep_unmapped=false;
    if (rc) return -1;
    for (unsigned i=0;i<OWNER_MAPS;i++) {
        o->retired[i]=o->maps[i]; o->retired_guard[i]=o->held[i];
        memset(&o->maps[i],0,sizeof(o->maps[i])); o->held[i]=NULL;
        printf("{\"event\":\"OWNER_RETIRED\",\"pid\":%d,\"kind\":%u,\"iova\":%" PRIu64 ",\"bytes\":%zu,\"vaddr\":%" PRIuPTR ",\"mapped\":false}\n",getpid(),i,o->retired[i].iova,o->retired[i].size,(uintptr_t)o->retired[i].memory);
    }
    return owner_retired_unchanged(o);
}

int owner_retired_unchanged(struct owner *o)
{
    for (unsigned i=0;i<OWNER_MAPS;i++) {
        const struct owner_map *old=&o->retired[i];
        if (!old->memory) continue;
        if (old->mapped||!o->retired_guard[i]||memcmp(old->memory,o->retired_guard[i],old->size)||
            old->memory==o->maps[i].memory) { errno=EILSEQ; return -1; }
        printf("{\"event\":\"OWNER_RETIRED_CHECK\",\"pid\":%d,\"kind\":%u,\"bytes\":%zu,\"crc\":%u,\"bytewise_unchanged\":true,\"mapped\":false}\n",getpid(),i,old->size,lab_crc32(old->memory,old->size));
    }
    return 0;
}
