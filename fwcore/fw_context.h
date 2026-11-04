#ifndef FW_CONTEXT_H
#define FW_CONTEXT_H
#include "fw_hal.h"
struct fw_vf {
    enum fw_state state;
    fw_u32 reserve;
    fw_u64 epoch, deadline, serviced;
};
struct fw_context {
    struct fw_hal hal;
    struct fw_vf vf[FW_VFS];
    fw_u64 session, round;
    fw_u32 used_queues, used_vectors, used_credit, last_seq;
    fw_u8 negotiated, cache_valid;
    struct fw_request cached_request;
    struct fw_reply cached_reply;
    fw_u8 mailbox_pending;
    fw_u64 pending_epoch, pending_deadline;
};
struct fw_snapshot {
    fw_u64 session, round;
    fw_u32 used_queues, used_vectors, used_credit, last_seq;
    fw_u8 negotiated, cache_valid;
    struct fw_vf vf[FW_VFS];
    struct fw_hw_snapshot hw[FW_VFS];
};
#endif
