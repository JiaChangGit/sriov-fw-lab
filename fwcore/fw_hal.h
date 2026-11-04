#ifndef FW_HAL_H
#define FW_HAL_H
#include "fw_types.h"
/* 回呼在呼叫期間返回狀態；mailbox_progress 可回 FW_BUSY，表示交易尚未完成。
 * 失敗不得打開 gate；硬體 fence/epoch 由 adapter 管理。 */
struct fw_hal {
    void *opaque;
    fw_u64 (*now_tick)(void *opaque);
    void (*log)(void *opaque, const char *message, fw_u16 vf, fw_u64 value);
    fw_status (*read_hw)(void *opaque, fw_u16 vf, struct fw_hw_snapshot *out);
    fw_status (*cancel_io)(void *opaque, fw_u16 vf);
    fw_status (*clear_queue)(void *opaque, fw_u16 vf);
    fw_status (*set_gate)(void *opaque, fw_u16 vf, fw_u8 open);
    fw_status (*kill)(void *opaque, fw_u16 vf);
    /* Adapter 回報的是其可觀察的關閉條件；QEMU adapter 另依賴可信 PF driver 管理 lease/fd。 */
    fw_u8 (*owners_closed)(void *opaque, fw_u16 vf);
    /* 全域 begin 保留 kill；core 清理完成後 end 才解除 kill，fence 必須保留。 */
    fw_status (*begin_global)(void *opaque, fw_u64 *session, fw_u64 epochs[FW_VFS]);
    fw_status (*end_global)(void *opaque);
    /* 選配故障注入點；觸發條件由 adapter 決定，NULL 表示沒有注入。 */
    fw_status (*checkpoint)(void *opaque, fw_u16 vf, enum fw_checkpoint point);
    /* 回 FW_BUSY 暫停已開始的有界交易；core 仍獨立檢查邏輯期限。 */
    fw_status (*mailbox_progress)(void *opaque, fw_u16 vf, fw_u32 opcode);
};
#endif
