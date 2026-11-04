#ifndef FW_INTERNAL_H
#define FW_INTERNAL_H
#include "fw_core.h"
fw_status fw_hw(const struct fw_context *ctx, fw_u16 vf, struct fw_hw_snapshot *out);
fw_status fw_deadline(const struct fw_context *ctx, fw_u64 delta, fw_u64 *out);
void fw_note(struct fw_context *ctx, const char *message, fw_u16 vf, fw_u64 value);
fw_status fw_transition(struct fw_context *ctx, fw_u16 vf, enum fw_event event);
#endif
