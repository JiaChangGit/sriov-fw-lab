/* 以 LoadFile2 提供 initramfs，透過 Linux EFI stub 啟動 guest；不重新啟動 QEMU。 */
#include "Probe.h"
#include <Protocol/LoadFile2.h>
#include <Protocol/DevicePath.h>
#include <Guid/LinuxEfiInitrdMedia.h>
#include <Library/DevicePathLib.h>

static VOID *InitrdData;
static UINTN InitrdSize;
#pragma pack(1)
static struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} InitrdPath = {
  { { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP, { sizeof(VENDOR_DEVICE_PATH), 0 } }, LINUX_EFI_INITRD_MEDIA_GUID },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof(EFI_DEVICE_PATH_PROTOCOL), 0 } }
};
#pragma pack()

static EFI_STATUS EFIAPI LoadInitrd(EFI_LOAD_FILE2_PROTOCOL *This,
    EFI_DEVICE_PATH_PROTOCOL *Path, BOOLEAN BootPolicy, UINTN *Size, VOID *Buffer)
{
  if (BootPolicy) return EFI_UNSUPPORTED;
  if (Size == NULL || Path == NULL || !IsDevicePathEnd(Path)) return EFI_INVALID_PARAMETER;
  if (Buffer == NULL || *Size < InitrdSize) {
    *Size = InitrdSize;
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem(Buffer, InitrdData, InitrdSize);
  *Size = InitrdSize;
  Log("INITRD_LOADFILE2_BYTES=%Lu\r\n", (UINT64)InitrdSize);
  return EFI_SUCCESS;
}
static EFI_LOAD_FILE2_PROTOCOL InitrdProtocol = { LoadInitrd };

EFI_STATUS Handoff(EFI_HANDLE ImageHandle, EFI_HANDLE Device, EFI_FILE_PROTOCOL *Root)
{
  EFI_STATUS Status;
  EFI_HANDLE InitrdHandle = NULL, Kernel = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  VOID *Command = NULL;
  CHAR16 *Options = NULL;
  UINTN CommandSize, I;
  Status = ReadFile(Root,L"\\initramfs.cpio.gz",256U*1024U*1024U,&InitrdData,&InitrdSize);
  if (EFI_ERROR(Status)) goto Done;
  Status = ReadFile(Root,L"\\cmdline.txt",4096,&Command,&CommandSize);
  if (EFI_ERROR(Status)) goto Done;
  Options = AllocateZeroPool((CommandSize+1)*sizeof(CHAR16));
  if (Options == NULL) { Status=EFI_OUT_OF_RESOURCES; goto Done; }
  for (I=0; I<CommandSize; ++I) {
    UINT8 Ch=((UINT8 *)Command)[I];
    if (!Ch || Ch>0x7f) { Status=EFI_INVALID_PARAMETER; goto Done; }
    Options[I]=(Ch=='\n'||Ch=='\r')?' ':Ch;
  }
  Status=gBS->InstallMultipleProtocolInterfaces(&InitrdHandle,
      &gEfiDevicePathProtocolGuid,&InitrdPath,
      &gEfiLoadFile2ProtocolGuid,&InitrdProtocol,NULL);
  if (EFI_ERROR(Status)) goto Done;
  Path=FileDevicePath(Device,L"\\linux.efi");
  if (Path==NULL) { Status=EFI_OUT_OF_RESOURCES; goto Done; }
  Status=gBS->LoadImage(FALSE,ImageHandle,Path,NULL,0,&Kernel);
  if (EFI_ERROR(Status)) goto Done;
  Status=gBS->HandleProtocol(Kernel,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if (EFI_ERROR(Status)) goto Done;
  Loaded->LoadOptions=Options;
  Loaded->LoadOptionsSize=(UINT32)((CommandSize+1)*sizeof(CHAR16));
  Log("HANDOFF_LOAD_OPTIONS=%s\r\nHANDOFF_START=LINUX_EFI_STUB\r\n",Options);
  Status=gBS->StartImage(Kernel,NULL,NULL);
  /* Linux 正常接手不會返回；即使回 EFI_SUCCESS 也不是成功開機證據。 */
  if (!EFI_ERROR(Status)) Status=EFI_ABORTED;
Done:
  if (Kernel!=NULL) gBS->UnloadImage(Kernel);
  if (InitrdHandle!=NULL) gBS->UninstallMultipleProtocolInterfaces(InitrdHandle,
      &gEfiDevicePathProtocolGuid,&InitrdPath,&gEfiLoadFile2ProtocolGuid,&InitrdProtocol,NULL);
  if (Path!=NULL) FreePool(Path);
  if (Options!=NULL) FreePool(Options);
  if (Command!=NULL) FreePool(Command);
  if (InitrdData!=NULL) FreePool(InitrdData);
  InitrdData=NULL; InitrdSize=0;
  Log("HANDOFF_RETURNED status=%r\r\n",Status);
  return Status;
}
