#include "Probe.h"
#include <Guid/FileInfo.h>
#include <Protocol/PciRootBridgeIo.h>

/* EDK2 X64 的 VA_START 使用 Microsoft ABI，宣告與定義須同時套用 EFIAPI。 */
VOID EFIAPI Log(CONST CHAR8 *Format, ...)
{
  CHAR8 Buffer[1024];
  VA_LIST Args;
  UINTN Count;
  VA_START(Args, Format);
  Count = AsciiVSPrint(Buffer, sizeof(Buffer), Format, Args);
  VA_END(Args);
  SerialPortWrite((UINT8 *)Buffer, Count);
}

EFI_STATUS OpenBootRoot(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **Root, EFI_HANDLE *Device)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_STATUS Status;
  Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR(Status)) return Status;
  *Device = Loaded->DeviceHandle;
  Status = gBS->HandleProtocol(*Device, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR(Status)) return Status;
  return Fs->OpenVolume(Fs, Root);
}

EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Name, UINTN Limit, VOID **Data, UINTN *Size)
{
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info = NULL;
  EFI_STATUS Status;
  UINTN InfoSize = 0, ReadSize;
  VOID *Buffer = NULL;
  *Data = NULL;
  *Size = 0;
  Status = Root->Open(Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(Status)) return Status;
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize > 4096) goto Done;
  Info = AllocatePool(InfoSize);
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR(Status)) goto Done;
  if (!Info->FileSize || Info->FileSize > Limit || Info->FileSize > MAX_UINTN - 2) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  ReadSize = (UINTN)Info->FileSize;
  Buffer = AllocateZeroPool(ReadSize + 2);
  if (Buffer == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  *Size = ReadSize;
  Status = File->Read(File, &ReadSize, Buffer);
  if (!EFI_ERROR(Status) && ReadSize != *Size) Status = EFI_DEVICE_ERROR;
  if (!EFI_ERROR(Status)) { *Data = Buffer; Buffer = NULL; }
Done:
  if (Buffer != NULL) FreePool(Buffer);
  if (Info != NULL) FreePool(Info);
  File->Close(File);
  return Status;
}

EFI_STATUS ConfigRead(EFI_PCI_IO_PROTOCOL *Pci, UINTN Offset, UINT32 *Value)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *Root;
  UINTN Segment, Bus, Device, Function, Count, Index;
  if (Offset > 0xffc || (Offset & 3)) return EFI_INVALID_PARAMETER;
  Status = Pci->Pci.Read(Pci, EfiPciIoWidthUint32, (UINT32)Offset, 1, Value);
  if (Status != EFI_UNSUPPORTED || Offset < 0x100) return Status;
  Status = Pci->GetLocation(Pci, &Segment, &Bus, &Device, &Function);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPciRootBridgeIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  Status = EFI_UNSUPPORTED;
  for (Index = 0; Index < Count; ++Index) {
    EFI_STATUS One = gBS->HandleProtocol(Handles[Index], &gEfiPciRootBridgeIoProtocolGuid, (VOID **)&Root);
    if (EFI_ERROR(One) || Root->SegmentNumber != Segment) continue;
    /* 使用 PciBusDxe 相同 ExtendedRegister 編碼與已驗 segment。 */
    One = Root->Pci.Read(Root, EfiPciWidthUint32,
            EFI_PCI_ADDRESS(Bus, Device, Function, 0) | LShiftU64(Offset, 32), 1, Value);
    if (!EFI_ERROR(One)) {
      Log("PCI_EXT_ACCESS=ROOT_BRIDGE segment=%u\r\n", (UINT32)Segment);
      Status = EFI_SUCCESS;
      break;
    }
    Status = One;
  }
  FreePool(Handles);
  return Status;
}
