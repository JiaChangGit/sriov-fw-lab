#define _GNU_SOURCE
#include "owner.h"
#include "../common/lab_wire.h"
#include "toy_regs.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int owner_dma_fault(struct owner *o)
{
    unsigned variant=o->dma_fault;
    unsigned char *sq=o->maps[OWNER_SQ].memory,*cq=o->maps[OWNER_CQ].memory;
    unsigned char *payload=o->maps[OWNER_PAYLOAD].memory;
    uint64_t original_epoch=o->epoch;
    uint64_t offset=variant==2?o->vfio.bytes-16:0;
    uint32_t length=variant==2?32:8,flags,status,error;
    uint64_t irq=0;
    size_t changed=0;
    char cq_hex[65]={0},sq_hex[65]={0};
    if (!variant||variant>6||o->sq_tail||o->cq_head) { errno=EINVAL; return -1; }
    memset(sq,0,32); lab_put64(sq,o->maps[OWNER_PAYLOAD].iova+offset);
    lab_put32(sq+8,length); sq[12]=variant==4?2:1; lab_put16(sq+14,0x505);
    lab_put64(sq+16,o->epoch); lab_put64(sq+24,0x73);
    for (unsigned i=0;i<32;i++) snprintf(sq_hex+i*2,3,"%02x",sq[i]);
    if (owner_freeze(o)) return -1;
    atomic_thread_fence(memory_order_release);
    if (owner_write32(o,TOY_VF_SQ_TAIL,1)||owner_write32(o,TOY_VF_DOORBELL,1)) return -1;
    int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
    do {
        if (owner_read32(o,TOY_VF_QUEUE_STATUS,&status)) return -1;
        if (status==TOY_QUEUE_STATUS_ERROR) break;
        atomic_thread_fence(memory_order_acquire);
        if (cq[24]==1&&lab_get16(cq+2)==TOY_STATUSES_OK) {
            fprintf(stderr,"T05_UNEXPECTED_DMA_SUCCESS variant=%u status=%u result=%" PRIu64 "\n",variant,lab_get16(cq+2),lab_get64(cq+16));
            errno=EPROTO; return -1;
        }
        if (lab_now_ms()>=deadline) { errno=ETIMEDOUT; return -1; }
        usleep(1000);
    } while (1);
    if (owner_latch(o,&flags)||owner_read32(o,TOY_VF_QUEUE_ERROR,&error)||error!=TOY_STATUSES_DMA||
        (flags&(TOY_HW_FLAGS_FENCED|TOY_HW_FLAGS_KILLED))!=(TOY_HW_FLAGS_FENCED|TOY_HW_FLAGS_KILLED)||
        o->epoch<=original_epoch) { errno=EPROTO; return -1; }
    atomic_thread_fence(memory_order_acquire);
    if (memcmp(sq,o->held[OWNER_SQ],o->maps[OWNER_SQ].size)) { errno=EILSEQ; return -1; }
    /* 跨頁 FILL 可能已寫入第一頁；逐 byte 驗證真實部分寫入，不做 CPU 回滾。 */
    for (size_t i=0;i<o->maps[OWNER_PAYLOAD].size;i++) {
        bool written=(variant==2&&i>=offset&&i<offset+16)||(variant==6&&i<8);
        unsigned char expected=written?0x73:(0xa5^o->pattern);
        if (payload[i]!=expected) { errno=EILSEQ; return -1; }
        changed+=payload[i]!=((unsigned char *)o->held[OWNER_PAYLOAD])[i];
    }
    if (variant<=4) {
        if (lab_get16(cq)!=0x505||lab_get16(cq+2)!=TOY_STATUSES_DMA||lab_get32(cq+4)!=length||
            lab_get64(cq+8)!=original_epoch||lab_get64(cq+16)||cq[24]!=1) { errno=EPROTO; return -1; }
        for (unsigned i=25;i<32;i++) if (cq[i]) { errno=EPROTO; return -1; }
        if (memcmp(cq+32,(unsigned char *)o->held[OWNER_CQ]+32,o->maps[OWNER_CQ].size-32)) { errno=EILSEQ; return -1; }
        for (unsigned i=0;i<32;i++) snprintf(cq_hex+i*2,3,"%02x",cq[i]);
    } else if (memcmp(cq,o->held[OWNER_CQ],o->maps[OWNER_CQ].size)) { errno=EILSEQ; return -1; }
    /* 已失敗的 VF 不會再出 IRQ；等一個短觀察窗後讀取真實 eventfd 計數。 */
    struct pollfd p={o->eventfds[0],POLLIN,0};
    int ready=poll(&p,1,20);
    if (ready<0) return -1;
    if (ready&&read(o->eventfds[0],&irq,sizeof(irq))!=(ssize_t)sizeof(irq)) return -1;
    if (variant>=5&&irq) { errno=EPROTO; return -1; }
    printf("{\"event\":\"T05_FAULT\",\"pid\":%d,\"variant\":%u,\"descriptor_hex\":\"%s\",\"completion_hex\":\"%s\",\"epoch_before\":%" PRIu64 ",\"epoch_after\":%" PRIu64 ",\"flags\":%u,\"queue_status\":%u,\"queue_error\":%u,\"payload_bytes\":%zu,\"payload_changed\":%zu,\"partial_write\":%s,\"cpu_rollback\":false,\"sq_unchanged\":true,\"other_cq_unchanged\":true,\"vf_irq_count\":%" PRIu64 "}\n",getpid(),variant,sq_hex,cq_hex,original_epoch,o->epoch,flags,status,error,o->maps[OWNER_PAYLOAD].size,changed,variant==2?"true":"false",irq);
    return 0;
}
