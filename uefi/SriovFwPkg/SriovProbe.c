/* UEFI 階段檢查 PF capabilities、BAR 與 mailbox；handoff 模式再交給同次開機的 Linux。 */
#include "Probe.h"
#include <IndustryStandard/Acpi10.h>

static VOID Report(CONST CHAR8 *Id, EFI_STATUS Status)
{
  Log("{\"id\":\"%a\",\"status\":\"%a\",\"efi_status\":\"%r\"}\r\n",
      Id, EFI_ERROR(Status)?"FAIL":"PASS", Status);
}

static EFI_STATUS MemRead(EFI_PCI_IO_PROTOCOL *Pci, UINT32 Reg, UINT32 *Value)
{ return Pci->Mem.Read(Pci,EfiPciIoWidthUint32,0,Reg,1,Value); }
static EFI_STATUS MemWrite(EFI_PCI_IO_PROTOCOL *Pci, UINT32 Reg, UINT32 Value)
{ return Pci->Mem.Write(Pci,EfiPciIoWidthUint32,0,Reg,1,&Value); }

static EFI_STATUS WalkCaps(EFI_PCI_IO_PROTOCOL *Pci, UINTN *Sriov)
{
  BOOLEAN Seen[1024];
  UINT32 Value, Offset, Next;
  UINTN Hops;
  BOOLEAN Express=FALSE, Msix=FALSE, Ari=FALSE, Acs=FALSE;
  EFI_STATUS Status=ConfigRead(Pci,0x34,&Value);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(Seen,sizeof(Seen));
  Offset=Value&0xff;
  for (Hops=0; Offset && Hops<48; ++Hops) {
    if (Offset<0x40 || Offset>0xfc || (Offset&3) || Seen[Offset/4]) return EFI_COMPROMISED_DATA;
    Seen[Offset/4]=TRUE;
    Status=ConfigRead(Pci,Offset,&Value);
    if (EFI_ERROR(Status)) return Status;
    if ((Value&0xff)==0x10) Express=TRUE;
    if ((Value&0xff)==0x11) {
      Msix=TRUE;
      if (((Value>>16)&0x7ff)+1!=2) return EFI_COMPROMISED_DATA;
      Log("PF_MSIX_VECTORS=2\r\n");
    }
    Log("PCI_CAP offset=0x%x id=0x%x\r\n",Offset,Value&0xff);
    Offset=(Value>>8)&0xff;
  }
  if (Offset || !Express || !Msix) return EFI_COMPROMISED_DATA;
  ZeroMem(Seen,sizeof(Seen));
  *Sriov=0;
  Offset=0x100;
  for (Hops=0; Offset && Hops<64; ++Hops) {
    if (Offset<0x100 || Offset>0xffc || (Offset&3) || Seen[Offset/4]) return EFI_COMPROMISED_DATA;
    Seen[Offset/4]=TRUE;
    Status=ConfigRead(Pci,Offset,&Value);
    if (EFI_ERROR(Status)) return Status;
    if (!Value || Value==MAX_UINT32) return EFI_COMPROMISED_DATA;
    Log("PCI_EXT_CAP offset=0x%x id=0x%x version=%u\r\n",Offset,Value&0xffff,(Value>>16)&15);
    switch (Value&0xffff) {
    case 0x000d: if (Acs) return EFI_COMPROMISED_DATA; Acs=TRUE; break;
    case 0x000e: if (Ari) return EFI_COMPROMISED_DATA; Ari=TRUE; break;
    case 0x0010:
      if (*Sriov || Offset>0xfc0) return EFI_COMPROMISED_DATA;
      *Sriov=Offset;
      break;
    default: return EFI_UNSUPPORTED;
    }
    Next=Value>>20;
    Offset=Next;
  }
  return Offset || !*Sriov || !Ari || !Acs ? EFI_COMPROMISED_DATA : EFI_SUCCESS;
}

static EFI_STATUS Bar(EFI_PCI_IO_PROTOCOL *Pci, UINT8 Index, UINT64 Expected,
                      UINT64 *Base, UINT64 *End)
{
  EFI_STATUS Status;
  VOID *Resources=NULL;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *Desc;
  UINT64 Supports=0;
  UINT64 ConfigBase;
  UINT32 Low,High;
  UINT8 Physical=0,ProtocolIndex=0;
  /* 此版 PciBus 將 64-bit BAR 的兩個 DWORD 存成一筆 PciBar；protocol 索引須跳過高半部。 */
  while (Physical<Index) {
    Status=ConfigRead(Pci,0x10+4*Physical,&Low);
    if (EFI_ERROR(Status)) return Status;
    Physical+=(Low&7)==4?2:1;
    ++ProtocolIndex;
  }
  if (Physical!=Index || Index>=6) return EFI_INVALID_PARAMETER;
  Status=ConfigRead(Pci,0x10+4*Index,&Low);
  if (EFI_ERROR(Status)) return Status;
  if ((Low&15)!=(Index==0?4U:0U)) return EFI_COMPROMISED_DATA;
  ConfigBase=Low&~15U;
  if ((Low&7)==4) {
    Status=ConfigRead(Pci,0x14+4*Index,&High);
    if (EFI_ERROR(Status)) return Status;
    ConfigBase|=LShiftU64(High,32);
  }
  Status=Pci->GetBarAttributes(Pci,ProtocolIndex,&Supports,&Resources);
  if (EFI_ERROR(Status)) return Status;
  if (Resources==NULL) return EFI_COMPROMISED_DATA;
  Desc=Resources;
  if (Desc->Desc!=ACPI_ADDRESS_SPACE_DESCRIPTOR || Desc->ResType!=ACPI_ADDRESS_SPACE_TYPE_MEM ||
      Desc->AddrLen!=Expected || !Desc->AddrRangeMin || Desc->AddrRangeMin>MAX_UINT64-(Expected-1) ||
      Desc->AddrRangeMin>MAX_UINT64-Desc->AddrTranslationOffset ||
      Desc->AddrRangeMin+Desc->AddrTranslationOffset!=ConfigBase) {
    FreePool(Resources);
    return EFI_COMPROMISED_DATA;
  }
  *Base=Desc->AddrRangeMin;
  *End=*Base+Expected-1;
  Log("PF_BAR%u_PCI_IO_INDEX=%u PF_BAR%u_CONFIG_BASE=0x%Lx\r\n",Index,ProtocolIndex,Index,ConfigBase);
  Log("PF_ASSIGNED_BAR%u_BASE=0x%Lx PF_ASSIGNED_BAR%u_LENGTH=0x%Lx PF_ASSIGNED_BAR%u_END=0x%Lx\r\n",
      Index,*Base,Index,Desc->AddrLen,Index,*End);
  FreePool(Resources);
  return EFI_SUCCESS;
}

static EFI_STATUS Windows(EFI_HANDLE *Handles,UINTN Count,UINTN Segment,UINTN PfBus,
    CONST UINT64 Bases[4],CONST UINT64 Ends[4],BOOLEAN Negative)
{
  EFI_PCI_IO_PROTOCOL *Pci;
  EFI_STATUS Status;
  UINTN I, J, S, Bus, Dev, Fun, Matching=0;
  UINT32 Class, Buses, Window;
  UINT64 Base, End;
  for (I=0; I<Count; ++I) {
    Status=gBS->HandleProtocol(Handles[I],&gEfiPciIoProtocolGuid,(VOID **)&Pci);
    if (EFI_ERROR(Status)) return Status;
    Status=Pci->GetLocation(Pci,&S,&Bus,&Dev,&Fun);
    if (EFI_ERROR(Status)) return Status;
    if (S!=Segment) continue;
    Status=ConfigRead(Pci,0x08,&Class);
    if (EFI_ERROR(Status)) return Status;
    if ((Class>>16)!=0x0604) continue;
    Status=ConfigRead(Pci,0x18,&Buses);
    if (EFI_ERROR(Status)) return Status;
    if (PfBus<((Buses>>8)&0xff) || PfBus>((Buses>>16)&0xff)) continue;
    ++Matching;
    Status=ConfigRead(Pci,0x20,&Window);
    if (EFI_ERROR(Status)) return Status;
    Base=(UINT64)(Window&0xfff0)<<16;
    End=(UINT64)(Window&0xfff00000)|0xfffff;
    Log("BRIDGE_WINDOW segment=%u bdf=%02x:%02x.%x secondary=%u subordinate=%u base=0x%Lx limit=0x%Lx\r\n",
        (UINT32)S,(UINT32)Bus,(UINT32)Dev,(UINT32)Fun,(Buses>>8)&0xff,(Buses>>16)&0xff,Base,End);
    if (Base>End) return EFI_COMPROMISED_DATA;
    for (J=0; J<(Negative?2:4); ++J) {
      if (!Bases[J] || Bases[J]<Base || Ends[J]>End) return EFI_COMPROMISED_DATA;
    }
  }
  Log("BRIDGE_MATCH_COUNT=%u VF_WINDOW_ASSERTION=%a\r\n",(UINT32)Matching,Negative?"OBSERVE_ONLY":"CHECKED");
  return Matching?EFI_SUCCESS:EFI_NOT_FOUND;
}

static EFI_STATUS Mailbox(EFI_PCI_IO_PROTOCOL *Pci,UINT32 Major,UINT32 Expected)
{
  EFI_STATUS Status;
  UINT32 Seq, State, Ack, Result=MAX_UINT32, Value;
  UINTN I;
  Status=MemRead(Pci,TOY_PF_MBX_ACK_SEQ,&Seq);
  if (EFI_ERROR(Status)) return Status;
  if (Seq==MAX_UINT32) return EFI_OUT_OF_RESOURCES;
  ++Seq;
#define WRITE(Reg,Val) do { Status=MemWrite(Pci,Reg,Val); if (EFI_ERROR(Status)) return Status; } while (0)
  WRITE(TOY_PF_MBX_ACK,1);
  WRITE(TOY_PF_MBX_SEQ,Seq);
  WRITE(TOY_PF_MBX_OPCODE,TOY_OPCODES_VERSION_NEG);
  WRITE(TOY_PF_MBX_TARGET,TOY_GLOBAL);
  WRITE(TOY_PF_MBX_ARG0,Major);
  WRITE(TOY_PF_MBX_ARG1,0); WRITE(TOY_PF_MBX_ARG2,0); WRITE(TOY_PF_MBX_ARG3,0);
  WRITE(TOY_PF_MBX_SUBMIT,1);
  for (I=0; I<5000; ++I) {
    Status=MemRead(Pci,TOY_PF_MBX_STATUS,&State);
    if (EFI_ERROR(Status)) return Status;
    Status=MemRead(Pci,TOY_PF_MBX_ACK_SEQ,&Ack);
    if (EFI_ERROR(Status)) return Status;
    if (State==TOY_MBX_STATUS_DONE && Ack==Seq) break;
    gBS->Stall(1000);
  }
  if (I==5000) return EFI_TIMEOUT;
  Status=MemRead(Pci,TOY_PF_MBX_RESULT,&Result);
  if (EFI_ERROR(Status)) return Status;
  if (Result!=Expected) return EFI_PROTOCOL_ERROR;
  if (!Result) {
    Status=MemRead(Pci,TOY_PF_MBX_REPLY0,&Value);
    if (EFI_ERROR(Status)) return Status;
    if (Value!=TOY_ABI_VERSION) return EFI_INCOMPATIBLE_VERSION;
    Log("MAILBOX_VERSION=1.0\r\n");
  } else Log("MAILBOX_UNKNOWN_MAJOR=REJECTED status=%u\r\n",Result);
  WRITE(TOY_PF_MBX_ACK,1);
#undef WRITE
  return EFI_SUCCESS;
}

static EFI_STATUS Probe(BOOLEAN Negative)
{
  EFI_HANDLE *Handles=NULL;
  EFI_PCI_IO_PROTOCOL *Pci=NULL,*Candidate;
  EFI_STATUS Status;
  UINTN Count,I,Found=0,Sriov,Segment,Bus,Dev,Fun;
  UINT32 Value, Total, Initial, Offset, Stride, VfId, Supported, System, Control, Low,High;
  UINT64 Bases[4],Ends[4],OriginalAttributes=0;
  BOOLEAN AttributesKnown=FALSE;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPciIoProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I=0; I<Count; ++I) {
    Status=gBS->HandleProtocol(Handles[I],&gEfiPciIoProtocolGuid,(VOID **)&Candidate);
    if (EFI_ERROR(Status)) goto Done;
    Status=ConfigRead(Candidate,0,&Value);
    if (EFI_ERROR(Status)) goto Done;
    if (Value==((TOY_PCI_PF<<16)|TOY_PCI_VENDOR)) { Pci=Candidate; ++Found; }
  }
  if (Found!=1) { Status=EFI_NOT_FOUND; goto Done; }
  Status=Pci->GetLocation(Pci,&Segment,&Bus,&Dev,&Fun);
  if (EFI_ERROR(Status)) goto Done;
  Log("PF_LOCATION segment=%u bus=%u device=%u function=%u\r\n",(UINT32)Segment,(UINT32)Bus,(UINT32)Dev,(UINT32)Fun);
  Status=WalkCaps(Pci,&Sriov);
  if (EFI_ERROR(Status)) goto Done;
#define READ(Off,Out) do { Status=ConfigRead(Pci,(Off),&(Out)); if (EFI_ERROR(Status)) goto Done; } while (0)
  READ(Sriov+0x0c,Value); Initial=Value&0xffff; Total=Value>>16;
  READ(Sriov+0x14,Value); Offset=Value&0xffff; Stride=Value>>16;
  READ(Sriov+0x18,Value); VfId=Value>>16;
  READ(Sriov+0x1c,Supported); READ(Sriov+0x20,System); READ(Sriov+8,Control);
  Log("CAP_TOTAL_VFS=%u CAP_INITIAL_VFS=%u VF_DEVICE_ID=0x%x VF_OFFSET=%u VF_STRIDE=%u\r\n",Total,Initial,VfId,Offset,Stride);
  Log("SRIOV_SUPPORTED_PAGE_SIZE=0x%x SRIOV_SYSTEM_PAGE_SIZE=0x%x SRIOV_CONTROL=0x%x\r\n",Supported,System,Control&0xffff);
  /* 停用 SR-IOV allocator 的負向韌體可能未選 System Page Size，記錄而不預設失敗。 */
  if (Total!=8 || Initial!=8 || VfId!=TOY_PCI_VF || !Offset || !Stride || !Supported ||
      (!Negative && !System) || (System && ((System&(System-1)) || !(System&Supported))) || (Control&1)) {
    Status=EFI_COMPROMISED_DATA; goto Done;
  }
  Status=Bar(Pci,0,TOY_LIMITS_PF_BAR0_BYTES,&Bases[0],&Ends[0]);
  if (EFI_ERROR(Status)) goto Done;
  Status=Bar(Pci,2,TOY_LIMITS_BAR2_BYTES,&Bases[1],&Ends[1]);
  if (EFI_ERROR(Status)) goto Done;
  READ(Sriov+0x24,Low); READ(Sriov+0x28,High);
  if ((Low&15)!=4) { Status=EFI_COMPROMISED_DATA; goto Done; }
  Bases[2]=LShiftU64(High,32)|(Low&~15U);
  READ(Sriov+0x2c,Low);
  if (Low&15) { Status=EFI_COMPROMISED_DATA; goto Done; }
  Bases[3]=Low;
  for (I=2; I<4; ++I) {
    UINT64 Required=(UINT64)Total*TOY_LIMITS_VF_BAR0_BYTES;
    if (Bases[I]>MAX_UINT64-(Required-1)) { Status=EFI_COMPROMISED_DATA; goto Done; }
    Ends[I]=Bases[I]+Required-1;
    Log("VF_BAR%u_REQUIRED=0x%Lx VF_BAR%u_ASSIGNED_BASE=0x%Lx VF_BAR%u_ASSIGNED_END=0x%Lx\r\n",
        I==2?0:2,Required,I==2?0:2,Bases[I],I==2?0:2,Ends[I]);
  }
  Report("M2_CAPS",EFI_SUCCESS);
  Status=Windows(Handles,Count,Segment,Bus,Bases,Ends,Negative);
  Report("M2_WINDOWS",Status);
  if (EFI_ERROR(Status)) goto Done;
  Status=Pci->Attributes(Pci,EfiPciIoAttributeOperationGet,0,&OriginalAttributes);
  if (EFI_ERROR(Status)) goto Done;
  AttributesKnown=TRUE;
  Status=Pci->Attributes(Pci,EfiPciIoAttributeOperationEnable,EFI_PCI_IO_ATTRIBUTE_MEMORY,NULL);
  if (EFI_ERROR(Status)) goto Done;
  Status=MemRead(Pci,TOY_PF_MAGIC,&Value);
  if (EFI_ERROR(Status)) goto Done;
  if (Value!=TOY_PF_MAGIC_VALUE) { Status=EFI_COMPROMISED_DATA; goto Done; }
  Status=MemRead(Pci,TOY_PF_ABI,&Value);
  if (EFI_ERROR(Status)) goto Done;
  if (Value!=TOY_ABI_VERSION) { Status=EFI_INCOMPATIBLE_VERSION; goto Done; }
  Status=Mailbox(Pci,1,TOY_STATUSES_OK);
  if (!EFI_ERROR(Status)) Status=Mailbox(Pci,2,TOY_STATUSES_UNSUP);
  Report("M2_MAILBOX",Status);
#undef READ
Done:
  if (AttributesKnown) {
    EFI_STATUS Restore=Pci->Attributes(Pci,EfiPciIoAttributeOperationSet,OriginalAttributes,NULL);
    if (!EFI_ERROR(Status) && EFI_ERROR(Restore)) Status=Restore;
  }
  if (Handles!=NULL) FreePool(Handles);
  return Status;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *Root=NULL;
  EFI_HANDLE Device=NULL;
  VOID *ModeData=NULL;
  UINTN ModeSize;
  BOOLEAN DoHandoff=FALSE,Negative=FALSE;
  Status=SerialPortInitialize();
  if (EFI_ERROR(Status)) return Status;
  Log("SRIOV_PROBE_START ABI=1.0\r\n");
  Status=OpenBootRoot(ImageHandle,&Root,&Device);
  if (EFI_ERROR(Status)) goto Failed;
  Status=ReadFile(Root,L"\\lab-mode.txt",64,&ModeData,&ModeSize);
  if (EFI_ERROR(Status)) goto Failed;
  /* 僅接受完整模式名稱；文字檔可帶 CR/LF，不能以合法 prefix 隱藏未知模式。 */
  while (ModeSize && (((UINT8 *)ModeData)[ModeSize-1]=='\r' ||
                      ((UINT8 *)ModeData)[ModeSize-1]=='\n')) --ModeSize;
  if (ModeSize==16 && !CompareMem(ModeData,"negative-handoff",16)) {
    DoHandoff=TRUE;
    Negative=TRUE;
  } else if (ModeSize==7 && !CompareMem(ModeData,"handoff",7)) DoHandoff=TRUE;
  else if (ModeSize==8 && !CompareMem(ModeData,"negative",8)) Negative=TRUE;
  else if (ModeSize!=5 || CompareMem(ModeData,"probe",5)) { Status=EFI_INVALID_PARAMETER; goto Failed; }
  FreePool(ModeData); ModeData=NULL;
  Log("PROBE_MODE=%a\r\n",DoHandoff?(Negative?"negative-handoff":"handoff"):Negative?"negative":"probe");
  Status=CoreSelftest();
  Report("M2_CORE",Status);
  if (EFI_ERROR(Status)) goto Failed;
  Status=Probe(Negative);
  if (EFI_ERROR(Status)) goto Failed;
  Log("PROBE_PASS\r\n");
  if (DoHandoff) {
    Status=Handoff(ImageHandle,Device,Root);
    goto Failed;
  }
  Root->Close(Root);
  Log("LAB_RESULT=PASS\r\n");
  gRT->ResetSystem(EfiResetShutdown,EFI_SUCCESS,0,NULL);
  return EFI_SUCCESS;
Failed:
  if (ModeData!=NULL) FreePool(ModeData);
  if (Root!=NULL) Root->Close(Root);
  Log("PROBE_FAILED status=%r\r\nLAB_RESULT=FAIL\r\n",Status);
  gRT->ResetSystem(EfiResetShutdown,Status,0,NULL);
  return Status;
}
