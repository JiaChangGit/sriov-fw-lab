#ifndef FAKE_HAL_H
#define FAKE_HAL_H
#include "fw_core.h"
struct fake_hal {
    struct fw_hw_snapshot hw[FW_VFS];
    fw_u8 closed[FW_VFS];
    fw_u64 tick;
    fw_u32 failures, calls, log_count;
    fw_u32 checkpoint_failures, checkpoint_seen[FW_CP_COUNT], hold_opcode;
};
void fake_init(struct fake_hal *fake, struct fw_hal *hal);
fw_status fake_fence(struct fake_hal *fake, fw_u16 vf);
void fake_config(struct fake_hal *fake, fw_u16 vf);
fw_status fake_global(struct fake_hal *fake, struct fw_context *ctx);
#endif
