#include "owner.h"
#include "../common/lab_wire.h"
#include "toy_regs.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int owner_reset_clear(struct owner *o,const char *cause)
{
    static const uint32_t registers[]={TOY_VF_SQ_BASE_LO,TOY_VF_SQ_BASE_HI,TOY_VF_SQ_ENTRIES,
        TOY_VF_CQ_BASE_LO,TOY_VF_CQ_BASE_HI,TOY_VF_CQ_ENTRIES,TOY_VF_SQ_HEAD,TOY_VF_SQ_TAIL,
        TOY_VF_CQ_HEAD,TOY_VF_CQ_TAIL,TOY_VF_CONFIG_EPOCH_LO,TOY_VF_CONFIG_EPOCH_HI,
        TOY_VF_CONFIG_SESSION_LO,TOY_VF_CONFIG_SESSION_HI,TOY_VF_QUEUE_STATUS,TOY_VF_QUEUE_ERROR};
    uint32_t flags,value;
    if (owner_latch(o,&flags)) return -1;
    if (!(flags&TOY_HW_FLAGS_FENCED)||(flags&(TOY_HW_FLAGS_CONFIG_VALID|TOY_HW_FLAGS_READY_PENDING|TOY_HW_FLAGS_KILLED))) { errno=EPROTO; return -1; }
    for (unsigned i=0;i<sizeof(registers)/sizeof(registers[0]);i++) {
        if (owner_read32(o,registers[i],&value)) return -1;
        if (value) { errno=EPROTO; return -1; }
    }
    printf("{\"event\":\"%s\",\"pid\":%d,\"bdf\":\"%s\",\"cause\":\"%s\",\"session\":%" PRIu64 ",\"epoch\":%" PRIu64 ",\"flags\":%u,\"zero_registers\":16}\n",o->recovery_case?"RECOVERY_CLEAR":"T06_CLEAR",getpid(),o->vfio.bdf,cause,o->session,o->epoch,flags);
    return 0;
}

int owner_vfio_reset(struct owner *o)
{
    uint64_t old_epoch=o->epoch,old_session=o->session,count;
    uint32_t status,sq,cq,digests[OWNER_MAPS];
    if (!(o->vfio.info.flags&VFIO_DEVICE_FLAGS_RESET)||owner_freeze(o)||
        owner_read32(o,TOY_VF_QUEUE_STATUS,&status)||status!=TOY_QUEUE_STATUS_RUNNING||
        owner_read32(o,TOY_VF_SQ_HEAD,&sq)||owner_read32(o,TOY_VF_CQ_TAIL,&cq)||!sq||sq!=cq) { errno=EPROTO; return -1; }
    /* 真正走 VFIO ioctl 與 PCI FLR，不以 PF mailbox 代替。 */
    if (ioctl(o->vfio.device,VFIO_DEVICE_RESET)||owner_reset_clear(o,"VFIO_DEVICE_RESET")||
        o->session!=old_session||o->epoch<=old_epoch||owner_write32(o,TOY_VF_DOORBELL,1)||
        owner_unchanged(o,digests)) { if (!errno) errno=EPROTO; return -1; }
    errno=0;
    if (read(o->eventfds[0],&count,sizeof(count))!=-1||errno!=EAGAIN) { errno=EPROTO; return -1; }
    printf("{\"event\":\"T06_IOCTL_RESET\",\"pid\":%d,\"old_epoch\":%" PRIu64 ",\"new_epoch\":%" PRIu64 ",\"old_sq_head\":%u,\"old_cq_tail\":%u,\"completed\":%" PRIu64 ",\"ioctl_errno\":0,\"doorbell\":true,\"cq_irq\":0,\"old_buffers_unchanged\":true}\n",getpid(),old_epoch,o->epoch,sq,cq,o->completed);
    return 0;
}
