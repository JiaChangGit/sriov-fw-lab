#include "Probe.h"
#include "FwCore/fw_core.h"
struct Dummy { struct fw_hw_snapshot Hw[FW_VFS]; fw_u64 Tick; };
static fw_u64 Now(VOID *Opaque) { return ((struct Dummy *)Opaque)->Tick; }
static VOID Note(VOID *Opaque, CONST char *Message, fw_u16 Vf, fw_u64 Value)
{ Log("CORE_EVENT=%a vf=%u value=%Lu\r\n", Message, Vf, Value); }
static fw_status ReadHw(VOID *Opaque, fw_u16 Vf, struct fw_hw_snapshot *Out)
{
  if (Vf >= FW_VFS || Out == NULL) return FW_INVAL;
  *Out = ((struct Dummy *)Opaque)->Hw[Vf];
  return FW_OK;
}
static fw_status Cancel(VOID *Opaque, fw_u16 Vf)
{ ((struct Dummy *)Opaque)->Hw[Vf].outstanding = 0; return FW_OK; }
static fw_status Clear(VOID *Opaque, fw_u16 Vf)
{
  struct fw_hw_snapshot *Hw = &((struct Dummy *)Opaque)->Hw[Vf];
  Hw->config_valid = Hw->ready_pending = 0;
  return FW_OK;
}
static fw_status Gate(VOID *Opaque, fw_u16 Vf, fw_u8 Open)
{
  struct fw_hw_snapshot *Hw = &((struct Dummy *)Opaque)->Hw[Vf];
  if (Open && (Hw->killed || !Hw->config_valid || !Hw->bme)) return FW_STATE;
  Hw->fenced = !Open;
  if (Open) Hw->ready_pending = 0;
  return FW_OK;
}
static fw_status Kill(VOID *Opaque, fw_u16 Vf)
{ ((struct Dummy *)Opaque)->Hw[Vf].killed = 1; return FW_OK; }
static fw_u8 Closed(VOID *Opaque, fw_u16 Vf)
{ return Vf == FW_GLOBAL || !((struct Dummy *)Opaque)->Hw[Vf].pci_present; }

EFI_STATUS CoreSelftest(VOID)
{
  struct Dummy Dummy;
  struct fw_context Core;
  struct fw_hal Hal;
  struct fw_request Req;
  struct fw_reply Reply;
  struct fw_snapshot Snapshot;
  UINTN I;
  ZeroMem(&Dummy, sizeof(Dummy));
  ZeroMem(&Hal, sizeof(Hal));
  for (I = 0; I < FW_VFS; ++I) {
    Dummy.Hw[I].session = Dummy.Hw[I].epoch = Dummy.Hw[I].token = 1;
    Dummy.Hw[I].fenced = 1;
  }
  Hal.opaque=&Dummy; Hal.now_tick=Now; Hal.log=Note; Hal.read_hw=ReadHw;
  Hal.cancel_io=Cancel; Hal.clear_queue=Clear; Hal.set_gate=Gate;
  Hal.kill=Kill; Hal.owners_closed=Closed;
#define CHECK(Call) do { if ((Call) != FW_OK) { Log("CORE_SELFTEST_STEP_FAILED=%a\r\n", #Call); return EFI_COMPROMISED_DATA; } } while (0)
  CHECK(fw_init(&Core, &Hal));
  if (fw_crc32((CONST fw_u8 *)"123456789", 9) != 0xcbf43926U) return EFI_CRC_ERROR;
  ZeroMem(&Req, sizeof(Req));
  Req.session=1; Req.seq=1; Req.opcode=FW_VERSION_NEG; Req.target=FW_GLOBAL; Req.arg[0]=1;
  CHECK(fw_handle_mailbox(&Core,&Req,&Reply));
  CHECK(fw_create(&Core,0,8));
  CHECK(fw_vf_event(&Core,0,FW_COMMIT));
  if (Core.used_credit != 8 || Core.used_queues != 1 || Core.used_vectors != 2) return EFI_COMPROMISED_DATA;
  ++Dummy.Hw[0].epoch; ++Dummy.Hw[0].token;
  CHECK(fw_vf_event(&Core,0,FW_FENCE));
  CHECK(fw_vf_event(&Core,0,FW_DRAIN_OK));
  CHECK(fw_vf_event(&Core,0,FW_RESET_DONE));
  ++Dummy.Hw[0].epoch; ++Dummy.Hw[0].token;
  CHECK(fw_vf_event(&Core,0,FW_DESTROY));
  CHECK(fw_vf_event(&Core,0,FW_RELEASE_OK));
  CHECK(fw_snapshot(&Core,&Snapshot));
  CHECK(fw_check_invariants(&Core));
  if (Core.used_credit || Core.used_queues || Core.used_vectors || Core.vf[0].state != FW_FREE) return EFI_COMPROMISED_DATA;
#undef CHECK
  Log("CORE_SELFTEST=PASS\r\n");
  return EFI_SUCCESS;
}
