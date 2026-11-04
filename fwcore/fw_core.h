#ifndef FW_CORE_H
#define FW_CORE_H
#include "fw_context.h"
fw_status fw_init(struct fw_context *ctx, const struct fw_hal *hal);
fw_status fw_handle_mailbox(struct fw_context *ctx, const struct fw_request *req, struct fw_reply *reply);
fw_status fw_poll_mailbox(struct fw_context *ctx, struct fw_reply *reply);
fw_status fw_vf_event(struct fw_context *ctx, fw_u16 vf, enum fw_event event);
fw_status fw_create(struct fw_context *ctx, fw_u16 vf, fw_u32 reserve);
fw_status fw_set_reserve(struct fw_context *ctx, fw_u16 vf, fw_u32 reserve);
fw_status fw_rearm(struct fw_context *ctx, fw_u16 vf, fw_u64 epoch, fw_u32 ready_gen);
fw_status fw_schedule_round(struct fw_context *ctx, const fw_u32 backlog[FW_VFS], fw_u32 service[FW_VFS]);
fw_status fw_snapshot(const struct fw_context *ctx, struct fw_snapshot *out);
fw_status fw_global_recover(struct fw_context *ctx, fw_u64 new_session, const fw_u64 epochs[FW_VFS]);
fw_status fw_poll_deadlines(struct fw_context *ctx);
fw_status fw_check_invariants(const struct fw_context *ctx);
fw_status fw_decode_sq(const fw_u8 bytes[32], struct fw_sq *out);
fw_status fw_encode_sq(const struct fw_sq *sq, fw_u8 bytes[32]);
fw_status fw_decode_cq(const fw_u8 bytes[32], struct fw_cq *out);
fw_status fw_encode_cq(const struct fw_cq *cq, fw_u8 bytes[32]);
fw_status fw_queue_validate(const struct fw_queue_config *cfg, fw_u64 session, fw_u64 epoch);
fw_status fw_ring_pending(fw_u32 tail, fw_u32 head, fw_u32 entries, fw_u32 *pending);
fw_u8 fw_ring_phase(fw_u32 counter, fw_u32 entries);
fw_u32 fw_crc32(const fw_u8 *data, fw_size length);
#endif
