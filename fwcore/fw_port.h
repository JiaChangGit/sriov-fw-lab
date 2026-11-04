#ifndef FW_PORT_H
#define FW_PORT_H
#ifdef FW_UEFI
#include <Uefi.h>
typedef UINT8 fw_u8;
typedef UINT16 fw_u16;
typedef UINT32 fw_u32;
typedef UINT64 fw_u64;
typedef UINTN fw_size;
#else
#include <stdint.h>
#include <stddef.h>
typedef uint8_t fw_u8;
typedef uint16_t fw_u16;
typedef uint32_t fw_u32;
typedef uint64_t fw_u64;
typedef size_t fw_size;
#endif
#define FW_U32_MAX ((fw_u32)0xffffffffU)
#define FW_U64_MAX ((fw_u64)~(fw_u64)0)
#define FW_VFS 8U
#define FW_CAP_CREDIT 64U
#define FW_GLOBAL 0xffffU
void fw_zero(void *ptr, fw_size size);
void fw_copy(void *dst, const void *src, fw_size size);
#endif
