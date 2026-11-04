#include "fw_core.h"

fw_u32 fw_crc32(const fw_u8 *data, fw_size length)
{
    fw_u32 crc=FW_U32_MAX;
    fw_size i;
    unsigned bit;
    for (i=0; i<length; ++i) {
        crc^=data[i];
        for (bit=0; bit<8; ++bit) crc=(crc>>1)^((crc&1U)?0xedb88320U:0U);
    }
    return crc^FW_U32_MAX;
}
