#include "fake_hal.h"

static fw_u64 now_tick(void *opaque) { return ((struct fake_hal *)opaque)->tick; }
static void note(void *opaque, const char *message, fw_u16 vf, fw_u64 value)
{
    struct fake_hal *f=opaque;
    (void)message; (void)vf; (void)value; ++f->log_count;
}
static fw_status read_hw(void *opaque, fw_u16 vf, struct fw_hw_snapshot *out)
{
    struct fake_hal *f=opaque;
    ++f->calls;
    if (f->failures&1U) return FW_IO;
    *out=f->hw[vf]; return FW_OK;
}
static fw_status cancel_io(void *opaque, fw_u16 vf)
{
    struct fake_hal *f=opaque;
    ++f->calls;
    if (f->failures&2U) return FW_IO;
    f->hw[vf].outstanding=0; return FW_OK;
}
static fw_status clear_queue(void *opaque, fw_u16 vf)
{
    struct fake_hal *f=opaque;
    ++f->calls;
    if (f->failures&4U) return FW_IO;
    f->hw[vf].config_valid=0; f->hw[vf].ready_pending=0; f->hw[vf].outstanding=0;
    return FW_OK;
}
static fw_status set_gate(void *opaque, fw_u16 vf, fw_u8 open)
{
    struct fake_hal *f=opaque;
    ++f->calls;
    if (f->failures&8U) return FW_IO;
    if (open && (f->hw[vf].killed || !f->hw[vf].config_valid || !f->hw[vf].bme)) return FW_STATE;
    f->hw[vf].fenced=(fw_u8)!open; f->hw[vf].ready_pending=0;
    return FW_OK;
}
static fw_status kill(void *opaque, fw_u16 vf)
{
    struct fake_hal *f=opaque;
    ++f->calls;
    if (f->failures&16U) return FW_IO;
    f->hw[vf].fenced=1; f->hw[vf].killed=1; f->hw[vf].outstanding=0;
    f->hw[vf].config_valid=0; f->hw[vf].ready_pending=0;
    return FW_OK;
}
static fw_u8 owners_closed(void *opaque, fw_u16 vf)
{
    struct fake_hal *f=opaque;
    fw_u16 i;
    if (vf!=FW_GLOBAL) return f->closed[vf];
    for (i=0; i<FW_VFS; ++i) if (!f->closed[i] || f->hw[i].pci_present) return 0;
    return 1;
}
static fw_status begin_global(void *opaque, fw_u64 *session, fw_u64 epochs[FW_VFS])
{
    struct fake_hal *f=opaque;
    fw_u16 i;
    if (f->failures&32U) return FW_IO;
    if (!owners_closed(opaque,FW_GLOBAL)) return FW_STATE;
    if (f->hw[0].session==FW_U64_MAX) return FW_OVERFLOW;
    for (i=0; i<FW_VFS; ++i) if (f->hw[i].epoch==FW_U64_MAX || f->hw[i].token==FW_U64_MAX) return FW_OVERFLOW;
    *session=f->hw[0].session+1;
    for (i=0; i<FW_VFS; ++i) {
        ++f->hw[i].epoch; ++f->hw[i].token; epochs[i]=f->hw[i].epoch;
        f->hw[i].session=*session; f->hw[i].fenced=1;
        f->hw[i].config_valid=0; f->hw[i].ready_pending=0; f->hw[i].outstanding=0;
    }
    return FW_OK;
}
static fw_status end_global(void *opaque)
{
    struct fake_hal *f=opaque;
    fw_u16 i;
    if (f->failures&64U) return FW_IO;
    for (i=0; i<FW_VFS; ++i) f->hw[i].killed=0;
    return FW_OK;
}
static fw_status checkpoint(void *opaque, fw_u16 vf, enum fw_checkpoint point)
{
    struct fake_hal *f=opaque;
    (void)vf;
    ++f->checkpoint_seen[point];
    if (f->checkpoint_failures & (1U<<point)) {
        f->checkpoint_failures &= ~(1U<<point);
        return FW_IO;
    }
    return FW_OK;
}
static fw_status mailbox_progress(void *opaque, fw_u16 vf, fw_u32 opcode)
{
    struct fake_hal *f=opaque;
    (void)vf;
    return f->hold_opcode==opcode?FW_BUSY:FW_OK;
}
void fake_init(struct fake_hal *f, struct fw_hal *hal)
{
    fw_u16 i;
    fw_zero(f,sizeof(*f)); fw_zero(hal,sizeof(*hal));
    for (i=0; i<FW_VFS; ++i) { f->hw[i].session=1; f->hw[i].epoch=1; f->hw[i].fenced=1; f->closed[i]=1; }
    hal->opaque=f; hal->now_tick=now_tick; hal->log=note; hal->read_hw=read_hw;
    hal->cancel_io=cancel_io; hal->clear_queue=clear_queue; hal->set_gate=set_gate;
    hal->kill=kill; hal->owners_closed=owners_closed; hal->begin_global=begin_global; hal->end_global=end_global;
    hal->checkpoint=checkpoint; hal->mailbox_progress=mailbox_progress;
}
fw_status fake_fence(struct fake_hal *f, fw_u16 vf)
{
    f->hw[vf].fenced=1;
    if (f->hw[vf].epoch==FW_U64_MAX || f->hw[vf].token==FW_U64_MAX) {
        f->hw[vf].killed=1; return FW_OVERFLOW;
    }
    ++f->hw[vf].epoch; ++f->hw[vf].token; f->hw[vf].outstanding=0;
    return FW_OK;
}
void fake_config(struct fake_hal *f, fw_u16 vf)
{
    f->hw[vf].config_valid=1; f->hw[vf].ready_pending=1; f->hw[vf].bme=1;
    ++f->hw[vf].ready_gen;
}
fw_status fake_global(struct fake_hal *f, struct fw_context *ctx)
{
    fw_u64 session, epochs[FW_VFS];
    fw_status rc=begin_global(f,&session,epochs);
    if (rc==FW_OK) rc=fw_global_recover(ctx,session,epochs);
    if (rc==FW_OK) rc=end_global(f);
    return rc;
}
