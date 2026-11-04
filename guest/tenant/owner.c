/* 一個 owner 管理一個 VF 的 fd、IRQ 與 DMA buffers；PF lease 由 labd 持有及協調。 */
#define _GNU_SOURCE
#include "owner.h"
#include "../common/lab_wire.h"
#include "../common/lab_ring.h"
#include "toy_regs.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fail(struct owner *o, const char *name)
{
    if (!o->fail_fired && !strcmp(o->failpoint,name) && ++o->fail_seen==o->fail_hit) {
        o->fail_fired=true;
        printf("{\"event\":\"OWNER_FAILPOINT_HIT\",\"name\":\"%s\",\"hit\":%u}\n",name,o->fail_seen);
        errno=EIO; return 1;
    }
    return 0;
}
void owner_init(struct owner *o)
{
    memset(o,0,sizeof(*o)); lab_vfio_init(&o->vfio);
    o->eventfds[0]=o->eventfds[1]=-1; o->fail_hit=1;
}
int owner_read32(struct owner *o,uint32_t reg,uint32_t *value)
{
    unsigned char b[4];
    if ((reg&3u) || reg>o->vfio.bar0.size-4) { errno=EINVAL; return -1; }
    if (pread(o->vfio.device,b,4,(off_t)(o->vfio.bar0.offset+reg))!=4) return -1;
    *value=lab_get32(b); return 0;
}
int owner_write32(struct owner *o,uint32_t reg,uint32_t value)
{
    unsigned char b[4]; lab_put32(b,value);
    if ((reg&3u) || reg>o->vfio.bar0.size-4) { errno=EINVAL; return -1; }
    /* 先執行 C11 thread fence，再經 VFIO pwrite 寫 MMIO；本版只驗證 x86_64 QEMU 路徑。
     * 這裡沒有替 non-coherent 實體平台做 cache maintenance。 */
    atomic_thread_fence(memory_order_seq_cst);
    return pwrite(o->vfio.device,b,4,(off_t)(o->vfio.bar0.offset+reg))==4?0:-1;
}
static int read64(struct owner *o,uint32_t reg,uint64_t *value)
{
    uint32_t lo,hi;
    if (owner_read32(o,reg,&lo)||owner_read32(o,reg+4,&hi)) return -1;
    *value=(uint64_t)lo|((uint64_t)hi<<32); return 0;
}
static int write64(struct owner *o,uint32_t reg,uint64_t value)
{
    return owner_write32(o,reg,(uint32_t)value)||owner_write32(o,reg+4,(uint32_t)(value>>32))?-1:0;
}
int owner_latch(struct owner *o,uint32_t *flags)
{
    return owner_write32(o,TOY_VF_EPOCH_LATCH,1)||
        read64(o,TOY_VF_EPOCH_LO,&o->epoch)||read64(o,TOY_VF_SESSION_LO,&o->session)||
        owner_read32(o,TOY_VF_STATUS,flags)?-1:0;
}

/* 可變長 capability 必須重查；僅使用 pread/pwrite，不 mmap MSI-X 保留頁。 */
static int region_caps(struct owner *o,unsigned index)
{
    struct vfio_region_info first={.argsz=sizeof(first),.index=index};
    unsigned char *bytes;
    uint32_t seen[64],count=0,offset;
    int ret=-1;
    if (ioctl(o->vfio.device,VFIO_DEVICE_GET_REGION_INFO,&first)) return -1;
    if (first.argsz<sizeof(first)||first.argsz>65536) { errno=EPROTO; return -1; }
    bytes=calloc(1,first.argsz); if (!bytes) return -1;
    struct vfio_region_info *r=(void *)bytes;
    r->argsz=first.argsz; r->index=index;
    if (ioctl(o->vfio.device,VFIO_DEVICE_GET_REGION_INFO,r)) goto out;
    if (r->argsz>first.argsz || r->argsz<sizeof(*r) ||
        (r->flags&(VFIO_REGION_INFO_FLAG_READ|VFIO_REGION_INFO_FLAG_WRITE))!=
         (VFIO_REGION_INFO_FLAG_READ|VFIO_REGION_INFO_FLAG_WRITE)) goto invalid;
    offset=(r->flags&VFIO_REGION_INFO_FLAG_CAPS)?r->cap_offset:0;
    while (offset) {
        if (count==64 || (offset&3u) || offset<sizeof(*r) || offset>r->argsz-sizeof(struct vfio_info_cap_header)) goto invalid;
        for (unsigned i=0;i<count;i++) if (seen[i]==offset) goto invalid;
        seen[count++]=offset;
        const struct vfio_info_cap_header *h=(const void *)(bytes+offset);
        if (h->id==VFIO_REGION_INFO_CAP_SPARSE_MMAP) {
            const struct vfio_region_info_cap_sparse_mmap *s=(const void *)h;
            size_t available=r->argsz-offset;
            if (h->version!=1 || available<sizeof(*s) || s->nr_areas>(available-sizeof(*s))/sizeof(s->areas[0])) goto invalid;
            for (unsigned i=0;i<s->nr_areas;i++) {
                uint64_t off=s->areas[i].offset,size=s->areas[i].size;
                if (!size || off>r->size || size>r->size-off || ((off|size)&(o->vfio.bytes-1))) goto invalid;
                printf("{\"event\":\"OWNER_SPARSE_AREA\",\"bdf\":\"%s\",\"region\":%u,\"offset\":%" PRIu64 ",\"size\":%" PRIu64 "}\n",o->vfio.bdf,index,off,size);
            }
        }
        offset=h->next;
    }
    printf("{\"event\":\"OWNER_REGION\",\"bdf\":\"%s\",\"index\":%u,\"argsz\":%u,\"caps\":%u,\"access\":\"pread_pwrite\"}\n",o->vfio.bdf,index,r->argsz,count);
    ret=0; goto out;
invalid: errno=EPROTO;
out: free(bytes); return ret;
}
int owner_open(struct owner *o,const char *bdf,unsigned group)
{
    uint32_t flags;
    if (lab_vfio_open(&o->vfio,bdf,group)) return -1;
    for (unsigned i=0;i<3;i++)
        if (region_caps(o,i==0?VFIO_PCI_BAR0_REGION_INDEX:i==1?VFIO_PCI_BAR2_REGION_INDEX:VFIO_PCI_CONFIG_REGION_INDEX)) return -1;
    if (owner_latch(o,&flags)) return -1;
    return o->reset_test?owner_reset_clear(o,"open"):0;
}

static int iova_allowed(struct owner *o,uint64_t base,uint64_t size)
{
    unsigned char bytes[16384] __attribute__((aligned(8)))={0};
    struct vfio_iommu_type1_info *info=(void *)bytes;
    uint32_t offset,seen[64],count=0;
    bool allowed=false;
    info->argsz=sizeof(bytes);
    if (ioctl(o->vfio.container,VFIO_IOMMU_GET_INFO,info)) return -1;
    if (info->argsz<sizeof(*info)||info->argsz>sizeof(bytes)||
        !(info->flags&VFIO_IOMMU_INFO_PGSIZES)||!(info->iova_pgsizes&o->vfio.bytes)||
        !(info->flags&VFIO_IOMMU_INFO_CAPS)) goto invalid;
    offset=info->cap_offset;
    while (offset) {
        if (count==64 || (offset&3u) || offset<sizeof(*info) || offset>info->argsz-sizeof(struct vfio_info_cap_header)) goto invalid;
        for (unsigned i=0;i<count;i++) if (seen[i]==offset) goto invalid;
        seen[count++]=offset;
        const struct vfio_info_cap_header *h=(const void *)(bytes+offset);
        if (h->id==VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
            const struct vfio_iommu_type1_info_cap_iova_range *r=(const void *)h;
            size_t n=info->argsz-offset;
            if (h->version!=1 || n<sizeof(*r)||r->nr_iovas>(n-sizeof(*r))/sizeof(r->iova_ranges[0])) goto invalid;
            for (unsigned i=0;i<r->nr_iovas;i++) {
                uint64_t start=r->iova_ranges[i].start,end=r->iova_ranges[i].end;
                if (start>end) goto invalid;
                if (base>=start && base<=end && size && size-1<=end-base) allowed=true;
            }
        }
        offset=h->next;
    }
    if (!allowed) { errno=ERANGE; return -1; } return 0;
invalid: errno=EPROTO; return -1;
}
/* vaddr 是 CPU pointer，iova 是裝置位址；READ/WRITE 權限以裝置 DMA 的方向解讀。 */
static int map_buffers(struct owner *o,uint32_t entries)
{
    static const char *const names[]={"owner.map_sq","owner.map_cq","owner.map_payload"};
    size_t page=o->vfio.bytes;
    for (unsigned i=0;i<OWNER_MAPS;i++) {
        struct owner_map *m=&o->maps[i];
        size_t wanted=i==OWNER_PAYLOAD?(size_t)entries*8192:(size_t)entries*32;
        struct vfio_iommu_type1_dma_map map={.argsz=sizeof(map)};
        if (m->memory) { errno=EBUSY; return -1; }
        m->size=(wanted+page-1)&~(page-1);
        m->iova=i==OWNER_SQ?0x100000:i==OWNER_CQ?0x200000:0x400000;
        m->permissions=i==OWNER_SQ?VFIO_DMA_MAP_FLAG_READ:i==OWNER_CQ?VFIO_DMA_MAP_FLAG_WRITE:
                       VFIO_DMA_MAP_FLAG_READ|VFIO_DMA_MAP_FLAG_WRITE;
        m->dma_size=m->size;
        if (i==OWNER_PAYLOAD) {
            if (o->dma_fault==1) m->dma_size=0;
            if (o->dma_fault==2) m->dma_size=page;
            if (o->dma_fault==3) m->permissions=VFIO_DMA_MAP_FLAG_READ;
            if (o->dma_fault==4) m->permissions=VFIO_DMA_MAP_FLAG_WRITE;
        }
        if (i==OWNER_SQ&&o->dma_fault==5) m->permissions=VFIO_DMA_MAP_FLAG_WRITE;
        if (i==OWNER_CQ&&o->dma_fault==6) m->permissions=VFIO_DMA_MAP_FLAG_READ;
        if (iova_allowed(o,m->iova,m->size)||fail(o,"owner.alloc_buffer")) return -1;
        m->memory=mmap(NULL,m->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (m->memory==MAP_FAILED) { m->memory=NULL; return -1; }
        memset(m->memory,i==OWNER_PAYLOAD?(0xa5^o->pattern):0,m->size);
        if (!m->dma_size) {
            printf("{\"event\":\"T05_MAP_OMITTED\",\"pid\":%d,\"kind\":%u,\"iova\":%" PRIu64 ",\"size\":%zu}\n",getpid(),i,m->iova,m->size);
            continue;
        }
        map.vaddr=(uintptr_t)m->memory; map.iova=m->iova; map.size=m->dma_size; map.flags=m->permissions;
        if (fail(o,names[i])||ioctl(o->vfio.container,VFIO_IOMMU_MAP_DMA,&map)) return -1;
        m->mapped=true;
        printf("{\"event\":\"OWNER_MAP\",\"pid\":%d,\"bdf\":\"%s\",\"kind\":%u,\"iova\":%" PRIu64 ",\"size\":%zu,\"flags\":%u,\"vaddr\":%" PRIuPTR "}\n",getpid(),o->vfio.bdf,i,m->iova,m->dma_size,m->permissions,(uintptr_t)m->memory);
    }
    return 0;
}
/* 將兩個 MSI-X vector 接到 eventfd；計數是通知數，完成項目仍須逐筆讀 CQ 驗證。 */
static int enable_irq(struct owner *o)
{
    unsigned char bytes[sizeof(struct vfio_irq_set)+2*sizeof(int)] __attribute__((aligned(8)))={0};
    struct vfio_irq_set *irq=(void *)bytes;
    if (o->irq_enabled) return 0;
    for (unsigned i=0;i<2;i++) {
        if (fail(o,"owner.eventfd")) return -1;
        o->eventfds[i]=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
        if (o->eventfds[i]<0) return -1;
    }
    irq->argsz=sizeof(bytes); irq->flags=VFIO_IRQ_SET_DATA_EVENTFD|VFIO_IRQ_SET_ACTION_TRIGGER;
    irq->index=VFIO_PCI_MSIX_IRQ_INDEX; irq->count=2;
    memcpy(irq->data,o->eventfds,sizeof(o->eventfds));
    if (fail(o,"owner.irq_register")||ioctl(o->vfio.device,VFIO_DEVICE_SET_IRQS,irq)) return -1;
    o->irq_enabled=true; return 0;
}
/* 先準備 mapping/IRQ，再提交 queue 設定；此函式只確認 READY_PENDING，不自行執行 PF REARM。 */
int owner_configure(struct owner *o,uint32_t entries)
{
    unsigned char command[2]; uint32_t status,error,flags;
    if (entries<16||entries>256||(entries&(entries-1))) { errno=EINVAL; return -1; }
    if (map_buffers(o,entries)||enable_irq(o)) return -1;
    if (pread(o->vfio.device,command,2,(off_t)o->vfio.config.offset+4)!=2) return -1;
    lab_put16(command,lab_get16(command)|6u);
    if (pwrite(o->vfio.device,command,2,(off_t)o->vfio.config.offset+4)!=2 ||
        pread(o->vfio.device,command,2,(off_t)o->vfio.config.offset+4)!=2 ||
        (lab_get16(command)&6)!=6) { errno=EIO; return -1; }
    o->entries=entries; o->sq_tail=o->cq_head=0;
    if (write64(o,TOY_VF_SQ_BASE_LO,o->maps[OWNER_SQ].iova)||write64(o,TOY_VF_CQ_BASE_LO,o->maps[OWNER_CQ].iova)||
        owner_write32(o,TOY_VF_SQ_ENTRIES,entries)||owner_write32(o,TOY_VF_CQ_ENTRIES,entries)||
        write64(o,TOY_VF_CONFIG_EPOCH_LO,o->epoch)||write64(o,TOY_VF_CONFIG_SESSION_LO,o->session)||
        owner_write32(o,TOY_VF_CONFIG_COMMIT,1)||owner_read32(o,TOY_VF_QUEUE_STATUS,&status)||
        owner_read32(o,TOY_VF_QUEUE_ERROR,&error)||owner_latch(o,&flags)) return -1;
    if (error||status!=TOY_QUEUE_STATUS_READY_PENDING||
        (flags&(TOY_HW_FLAGS_FENCED|TOY_HW_FLAGS_CONFIG_VALID|TOY_HW_FLAGS_READY_PENDING|TOY_HW_FLAGS_BME|TOY_HW_FLAGS_KILLED))!=
        (TOY_HW_FLAGS_FENCED|TOY_HW_FLAGS_CONFIG_VALID|TOY_HW_FLAGS_READY_PENDING|TOY_HW_FLAGS_BME)) { errno=EPROTO; return -1; }
    return 0;
}

static unsigned request_length(uint64_t n)
{
    static const unsigned sizes[]={1,9,4095,4096}; return sizes[(n/2)%4];
}
/* 每個 outstanding id 有不同 payload 區；確認完成內容後才移交 CQ_HEAD。 */
int owner_workload(struct owner *o,unsigned requests)
{
    if (o->pending_request) { errno=EBUSY; return -1; }
    unsigned done=0;
    uint32_t status;
    if (owner_read32(o,TOY_VF_QUEUE_STATUS,&status)||status!=TOY_QUEUE_STATUS_RUNNING) { errno=EACCES; return -1; }
    while (done<requests) {
        unsigned batch=requests-done<o->entries?requests-done:o->entries;
        uint32_t start=o->sq_tail;
        for (unsigned j=0;j<batch;j++) {
            uint64_t n=o->completed+j;
            uint32_t index=lab_ring_index(start+j,o->entries);
            unsigned length=request_length(n);
            unsigned char *sq=(unsigned char *)o->maps[OWNER_SQ].memory+index*32;
            unsigned char *payload=(unsigned char *)o->maps[OWNER_PAYLOAD].memory+index*8192;
            memset(payload,0xa5^o->pattern,8192); memset(sq,0,32);
            if (n&1) for (unsigned k=0;k<length;k++) payload[k]=(unsigned char)(n+o->pattern+k);
            if ((n&1)&&length==9) memcpy(payload,"123456789",9);
            lab_put64(sq,o->maps[OWNER_PAYLOAD].iova+(uint64_t)index*8192);
            lab_put32(sq+8,length); sq[12]=(n&1)?TOY_SQ_OPS_CRC32:TOY_SQ_OPS_FILL;
            lab_put16(sq+14,(uint16_t)(start+j)); lab_put64(sq+16,o->epoch); lab_put64(sq+24,(unsigned char)(n+o->pattern));
        }
        atomic_thread_fence(memory_order_release);
        o->sq_tail+=batch;
        if (owner_write32(o,TOY_VF_SQ_TAIL,o->sq_tail)||owner_write32(o,TOY_VF_DOORBELL,1)) return -1;
        unsigned received=0;
        int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
        while (received<batch) {
            uint64_t count;
            if (lab_wait(o->eventfds[0],POLLIN,deadline)) return -1;
            if (read(o->eventfds[0],&count,sizeof(count))!=sizeof(count)) return -1;
            o->irq_count+=count; ++o->irq_wakes;
            uint32_t cause;
            if (owner_read32(o,TOY_VF_IRQ_CAUSE,&cause)||owner_write32(o,TOY_VF_IRQ_CAUSE,cause)) return -1;
            while (received<batch) {
                uint32_t index=lab_ring_index(o->cq_head,o->entries);
                unsigned char *cq=(unsigned char *)o->maps[OWNER_CQ].memory+index*32;
                unsigned char phase=__atomic_load_n(cq+24,__ATOMIC_ACQUIRE);
                unsigned char expected=lab_ring_phase(o->cq_head,o->entries);
                if (phase!=expected) break;
                atomic_thread_fence(memory_order_acquire);
                uint64_t n=o->completed+received;
                unsigned length=request_length(n);
                /* stale SQ 不產生 CQ，兩個 ring 的 cursor 可能不同；payload
                 * 屬於原始 submission slot，不能由 completion slot 推算。 */
                uint32_t payload_index=lab_ring_index(start+received,o->entries);
                unsigned char *payload=(unsigned char *)o->maps[OWNER_PAYLOAD].memory+payload_index*8192;
                uint32_t crc=(n&1)?lab_crc32(payload,length):0;
                if (lab_get16(cq)!=(uint16_t)(start+received)||lab_get16(cq+2)!=TOY_STATUSES_OK||
                    lab_get32(cq+4)!=length||lab_get64(cq+8)!=o->epoch||lab_get64(cq+16)!=crc) { errno=EBADMSG; return -1; }
                if ((n&1)&&length==9&&crc!=0xcbf43926u) { errno=EBADMSG; return -1; }
                for (unsigned k=0;k<8192;k++) {
                    unsigned char expected_byte=0xa5^o->pattern;
                    if (k<length) expected_byte=(n&1)?(length==9?(unsigned char)"123456789"[k]:(unsigned char)(n+o->pattern+k)):(unsigned char)(n+o->pattern);
                    if (payload[k]!=expected_byte) { errno=EILSEQ; return -1; }
                }
                for (unsigned k=25;k<32;k++) if (cq[k]) { errno=EBADMSG; return -1; }
                ++received; ++o->cq_head;
            }
            atomic_thread_fence(memory_order_release);
            if (owner_write32(o,TOY_VF_CQ_HEAD,o->cq_head)) return -1;
        }
        o->completed+=batch; done+=batch;
    }
    printf("{\"event\":\"OWNER_WORKLOAD\",\"pid\":%d,\"bdf\":\"%s\",\"entries\":%u,\"completed\":%" PRIu64 ",\"sq_tail\":%u,\"cq_head\":%u,\"irq_wakes\":%" PRIu64 ",\"irq_count\":%" PRIu64 ",\"payload_crc\":%u,\"fixed_crc\":%u}\n",getpid(),o->vfio.bdf,o->entries,o->completed,o->sq_tail,o->cq_head,o->irq_wakes,o->irq_count,lab_crc32(o->maps[OWNER_PAYLOAD].memory,o->maps[OWNER_PAYLOAD].size),lab_crc32("123456789",9));
    return 0;
}

int owner_pattern_pair(struct owner *o)
{
    unsigned char expected[4096],*payload=o->maps[OWNER_PAYLOAD].memory;
    unsigned char pattern=(unsigned char)(0x31+o->pattern);
    memset(expected,pattern,sizeof(expected));
    memset(payload,0xa5^o->pattern,8192);
    uint32_t expected_crc=lab_crc32(expected,sizeof(expected));
    /* 先讓裝置 FILL，再讀同一 IOVA 算 CRC；中間不以 CPU 重寫 payload。 */
    for (unsigned step=0;step<2;step++) {
        uint32_t index=lab_ring_index(o->sq_tail,o->entries),cause;
        unsigned char *sq=(unsigned char *)o->maps[OWNER_SQ].memory+index*32;
        unsigned char *cq=(unsigned char *)o->maps[OWNER_CQ].memory+lab_ring_index(o->cq_head,o->entries)*32;
        memset(sq,0,32); lab_put64(sq,o->maps[OWNER_PAYLOAD].iova); lab_put32(sq+8,sizeof(expected));
        sq[12]=step?TOY_SQ_OPS_CRC32:TOY_SQ_OPS_FILL; lab_put16(sq+14,(uint16_t)o->sq_tail);
        lab_put64(sq+16,o->epoch); lab_put64(sq+24,pattern);
        atomic_thread_fence(memory_order_release);
        ++o->sq_tail;
        if (owner_write32(o,TOY_VF_SQ_TAIL,o->sq_tail)||owner_write32(o,TOY_VF_DOORBELL,1)) return -1;
        int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
        do {
            uint64_t count;
            if (lab_wait(o->eventfds[0],POLLIN,deadline)||read(o->eventfds[0],&count,sizeof(count))!=sizeof(count)) return -1;
            o->irq_count+=count; ++o->irq_wakes;
            if (owner_read32(o,TOY_VF_IRQ_CAUSE,&cause)||owner_write32(o,TOY_VF_IRQ_CAUSE,cause)) return -1;
        } while (__atomic_load_n(cq+24,__ATOMIC_ACQUIRE)!=lab_ring_phase(o->cq_head,o->entries));
        atomic_thread_fence(memory_order_acquire);
        if (lab_get16(cq)!=(uint16_t)(o->sq_tail-1)||lab_get16(cq+2)!=TOY_STATUSES_OK||
            lab_get32(cq+4)!=sizeof(expected)||lab_get64(cq+8)!=o->epoch||lab_get64(cq+16)!=(step?expected_crc:0)||
            memcmp(payload,expected,sizeof(expected))) { errno=EBADMSG; return -1; }
        for (unsigned k=sizeof(expected);k<8192;k++) if (payload[k]!=(unsigned char)(0xa5^o->pattern)) { errno=EILSEQ; return -1; }
        for (unsigned k=25;k<32;k++) if (cq[k]) { errno=EBADMSG; return -1; }
        ++o->cq_head; ++o->completed;
        if (owner_write32(o,TOY_VF_CQ_HEAD,o->cq_head)) return -1;
        printf("{\"event\":\"OWNER_PATTERN_COMMAND\",\"pid\":%d,\"step\":%u,\"iova\":%" PRIu64 ",\"bytes\":%zu,\"pattern\":%u,\"result\":%" PRIu64 ",\"expected_crc\":%u}\n",getpid(),step,o->maps[OWNER_PAYLOAD].iova,sizeof(expected),pattern,lab_get64(cq+16),expected_crc);
    }
    printf("{\"event\":\"OWNER_PATTERN_PAIR\",\"pid\":%d,\"bdf\":\"%s\",\"completed\":%" PRIu64 ",\"payload_crc\":%u,\"sq_tail\":%u,\"cq_head\":%u}\n",getpid(),o->vfio.bdf,o->completed,lab_crc32(payload,o->maps[OWNER_PAYLOAD].size),o->sq_tail,o->cq_head);
    return 0;
}

int owner_descriptor_vectors(struct owner *o)
{
    static const char *const names[]={"old-epoch","unknown-opcode","zero-length","length-over-4096","address-overflow","nonzero-flags"};
    for (unsigned variant=0;variant<6;variant++) {
        unsigned char *sq=(unsigned char *)o->maps[OWNER_SQ].memory+lab_ring_index(o->sq_tail,o->entries)*32;
        unsigned char *cq=(unsigned char *)o->maps[OWNER_CQ].memory+lab_ring_index(o->cq_head,o->entries)*32;
        uint32_t old_cq_tail,head,tail,cause;
        uint32_t id=o->sq_tail;
        char hex[65],completion_hex[65]={0};
        if (owner_read32(o,TOY_VF_CQ_TAIL,&old_cq_tail)||old_cq_tail!=o->cq_head) { errno=EPROTO; return -1; }
        memset(sq,0,32); lab_put64(sq,o->maps[OWNER_PAYLOAD].iova); lab_put32(sq+8,8);
        sq[12]=TOY_SQ_OPS_FILL; lab_put16(sq+14,(uint16_t)id); lab_put64(sq+16,o->epoch); lab_put64(sq+24,0x73);
        switch (variant) {
        case 0: lab_put64(sq+16,o->epoch-1); break;
        case 1: sq[12]=0xff; break;
        case 2: lab_put32(sq+8,0); break;
        case 3: lab_put32(sq+8,4097); break;
        case 4: lab_put64(sq,UINT64_MAX-1024); lab_put32(sq+8,2048); break;
        case 5: sq[13]=1; break;
        }
        for (unsigned i=0;i<32;i++) snprintf(hex+i*2,3,"%02x",sq[i]);
        if (owner_freeze(o)) return -1;
        uint64_t before_irq=o->irq_count;
        atomic_thread_fence(memory_order_release);
        ++o->sq_tail;
        if (owner_write32(o,TOY_VF_SQ_TAIL,o->sq_tail)||owner_write32(o,TOY_VF_DOORBELL,1)) return -1;
        int64_t deadline=lab_now_ms()+LAB_TIMEOUT_MS;
        do {
            if (owner_read32(o,TOY_VF_SQ_HEAD,&head)) return -1;
            if (head==o->sq_tail) break;
            if (lab_now_ms()>=deadline) { errno=ETIMEDOUT; return -1; }
            usleep(1000);
        } while (true);
        if (!variant) {
            uint32_t digests[OWNER_MAPS];
            struct pollfd irq={.fd=o->eventfds[0],.events=POLLIN};
            int ready;
            do { ready=poll(&irq,1,20); } while (ready<0&&errno==EINTR);
            if (ready!=0||owner_read32(o,TOY_VF_CQ_TAIL,&tail)||tail!=old_cq_tail||owner_unchanged(o,digests)) { errno=EPROTO; return -1; }
        } else {
            do {
                uint64_t count;
                if (lab_wait(o->eventfds[0],POLLIN,deadline)||read(o->eventfds[0],&count,sizeof(count))!=sizeof(count)) return -1;
                o->irq_count+=count; ++o->irq_wakes;
                if (owner_read32(o,TOY_VF_IRQ_CAUSE,&cause)||owner_write32(o,TOY_VF_IRQ_CAUSE,cause)) return -1;
            } while (__atomic_load_n(cq+24,__ATOMIC_ACQUIRE)!=lab_ring_phase(o->cq_head,o->entries));
            atomic_thread_fence(memory_order_acquire);
            if (lab_get16(cq)!=(uint16_t)id||lab_get16(cq+2)!=TOY_STATUSES_BAD_DESC||lab_get32(cq+4)||
                lab_get64(cq+8)!=o->epoch||lab_get64(cq+16)) { errno=EBADMSG; return -1; }
            for (unsigned k=25;k<32;k++) if (cq[k]) { errno=EBADMSG; return -1; }
            for (unsigned kind=0;kind<OWNER_MAPS;kind++) {
                struct owner_map *map=&o->maps[kind];
                unsigned char *actual=map->memory,*saved=o->held[kind];
                for (size_t b=0;b<map->size;b++) {
                    if (kind==OWNER_CQ&&actual+b>=cq&&actual+b<cq+32) continue;
                    if (actual[b]!=saved[b]) { errno=EILSEQ; return -1; }
                }
            }
            ++o->cq_head; ++o->completed;
            for (unsigned k=0;k<32;k++) snprintf(completion_hex+k*2,3,"%02x",cq[k]);
            if (owner_write32(o,TOY_VF_CQ_HEAD,o->cq_head)||owner_read32(o,TOY_VF_CQ_TAIL,&tail)||tail!=o->cq_head) { errno=EPROTO; return -1; }
        }
        printf("{\"event\":\"T09_VECTOR\",\"pid\":%d,\"name\":\"%s\",\"descriptor_hex\":\"%s\",\"completion_hex\":\"%s\",\"live_epoch\":%" PRIu64 ",\"sq_head\":%u,\"sq_tail\":%u,\"cq_before\":%u,\"cq_after\":%u,\"irq_before\":%" PRIu64 ",\"irq_after\":%" PRIu64 ",\"completion_status\":%d,\"payload_unchanged\":true,\"other_cq_bytes_unchanged\":true}\n",getpid(),names[variant],hex,completion_hex,o->epoch,head,o->sq_tail,old_cq_tail,tail,before_irq,o->irq_count,variant?(int)lab_get16(cq+2):-1);
    }
    return 0;
}

int owner_freeze(struct owner *o)
{
    for (unsigned i=0;i<OWNER_MAPS;i++) {
        struct owner_map *m=&o->maps[i];
        /* CPU guard 也涵蓋刻意未提供 DMA 映射的已配置記憶體。 */
        if (!m->memory) { errno=EINVAL; return -1; }
        if (!o->held[i]) { o->held[i]=malloc(m->size); if (!o->held[i]) return -1; }
        memcpy(o->held[i],m->memory,m->size);
    }
    return 0;
}
int owner_unchanged(struct owner *o,uint32_t digests[OWNER_MAPS])
{
    for (unsigned i=0;i<OWNER_MAPS;i++) {
        struct owner_map *m=&o->maps[i];
        if (!o->held[i]||!m->memory||memcmp(o->held[i],m->memory,m->size)) { errno=EILSEQ; return -1; }
        digests[i]=lab_crc32(m->memory,m->size);
        printf("{\"event\":\"OWNER_HELD_CHECK\",\"pid\":%d,\"bdf\":\"%s\",\"kind\":%u,\"completed\":%" PRIu64 ",\"bytes\":%zu,\"pattern\":%u,\"crc\":%u,\"bytewise_unchanged\":true}\n",getpid(),o->vfio.bdf,i,o->completed,m->size,o->pattern,digests[i]);
    }
    if (o->recovery_case) {
        uint64_t count;
        errno=0;
        if (read(o->eventfds[0],&count,sizeof(count))!=-1||errno!=EAGAIN) { errno=EPROTO; return -1; }
        printf("{\"event\":\"RECOVERY_NO_IRQ\",\"pid\":%d,\"epoch\":%" PRIu64 ",\"completed\":%" PRIu64 ",\"eventfd_errno\":%d}\n",getpid(),o->epoch,o->completed,EAGAIN);
    }
    return owner_retired_unchanged(o);
}
/* 呼叫者先證明 fence 生效；每段 UNMAP 都核對實際長度，失敗的 allocation 保留給 close 收尾。 */
int owner_unmap(struct owner *o,bool fence_verified)
{
    int failed=0;
    if (!fence_verified) { errno=EACCES; return -1; }
    for (unsigned i=OWNER_MAPS;i>0;i--) {
        struct owner_map *m=&o->maps[i-1];
        if (m->mapped) {
            struct vfio_iommu_type1_dma_unmap unmap={.argsz=sizeof(unmap),.iova=m->iova,.size=m->dma_size};
            /* 要求包含相鄰未映射範圍，取得真實 TYPE1 短 UNMAP 回傳。 */
            bool short_test=fail(o,"owner.short_unmap");
            if (short_test) unmap.size*=2;
            uint64_t requested=unmap.size;
            bool injected=fail(o,"owner.unmap");
            int rc=injected?-1:ioctl(o->vfio.container,VFIO_IOMMU_UNMAP_DMA,&unmap);
            if (rc||unmap.size!=requested) {
                printf("{\"event\":\"OWNER_UNMAP_QUARANTINE\",\"pid\":%d,\"kind\":%u,\"requested\":%" PRIu64 ",\"returned\":%" PRIu64 ",\"ioctl_executed\":%s,\"ioctl_errno\":%d,\"buffer_held\":true}\n",getpid(),i-1,requested,(uint64_t)unmap.size,injected?"false":"true",rc?errno:0);
                failed=1; o->cleanup_failed=true; continue;
            }
            m->mapped=false;
            printf("{\"event\":\"OWNER_UNMAP\",\"pid\":%d,\"kind\":%u,\"requested\":%zu,\"returned\":%" PRIu64 "}\n",getpid(),i-1,m->dma_size,(uint64_t)unmap.size);
        }
        if (m->memory&&!o->keep_unmapped) {
            if (munmap(m->memory,m->size)) { failed=1; continue; }
            m->memory=NULL;
        }
    }
    return failed?-1:0;
}
int owner_close(struct owner *o,bool fence_verified)
{
    int failed=0;
    if (o->irq_enabled) {
        struct vfio_irq_set irq={.argsz=sizeof(irq),.flags=VFIO_IRQ_SET_DATA_NONE|VFIO_IRQ_SET_ACTION_TRIGGER,.index=VFIO_PCI_MSIX_IRQ_INDEX};
        if (ioctl(o->vfio.device,VFIO_DEVICE_SET_IRQS,&irq)) failed=1;
        else o->irq_enabled=false;
    }
    if (fence_verified && !o->cleanup_failed && owner_unmap(o,true)) failed=1;
    /* 未確認 fence 或短 UNMAP：保留記憶體到全部 owner fd 關閉之後。 */
    if (o->vfio.device>=0) { if (close(o->vfio.device)) failed=1; o->vfio.device=-1; }
    for (unsigned i=0;i<2;i++) if (o->eventfds[i]>=0) { if (close(o->eventfds[i])) failed=1; o->eventfds[i]=-1; }
    if (o->vfio.attached) { if (ioctl(o->vfio.group,VFIO_GROUP_UNSET_CONTAINER)) failed=1; o->vfio.attached=0; }
    if (o->vfio.group>=0) { if (close(o->vfio.group)) failed=1; o->vfio.group=-1; }
    if (o->vfio.container>=0) { if (close(o->vfio.container)) failed=1; o->vfio.container=-1; }
    for (unsigned i=0;i<OWNER_MAPS;i++) {
        struct owner_map *m=&o->maps[i]; m->mapped=false;
        if (m->memory) { if (munmap(m->memory,m->size)) failed=1; else m->memory=NULL; }
        free(o->held[i]); o->held[i]=NULL;
        if (o->retired[i].memory) {
            if (munmap(o->retired[i].memory,o->retired[i].size)) failed=1;
            else o->retired[i].memory=NULL;
        }
        free(o->retired_guard[i]); o->retired_guard[i]=NULL;
    }
    o->irq_enabled=false;
    return failed||o->cleanup_failed?-1:0;
}
