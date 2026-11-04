#ifndef SRIOV_PROBE_H
#define SRIOV_PROBE_H
#include <Uefi.h>
#include <Protocol/PciIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include "Generated/toy_regs.h"
VOID EFIAPI Log(CONST CHAR8 *Format, ...);
EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Name, UINTN Limit, VOID **Data, UINTN *Size);
EFI_STATUS OpenBootRoot(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **Root, EFI_HANDLE *Device);
EFI_STATUS CoreSelftest(VOID);
EFI_STATUS Handoff(EFI_HANDLE ImageHandle, EFI_HANDLE Device, EFI_FILE_PROTOCOL *Root);
EFI_STATUS ConfigRead(EFI_PCI_IO_PROTOCOL *Pci, UINTN Offset, UINT32 *Value);
#endif
