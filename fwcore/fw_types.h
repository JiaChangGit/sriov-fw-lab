#ifndef FW_TYPES_H
#define FW_TYPES_H
#include "fw_port.h"
typedef enum fw_status {
    FW_OK=0, FW_INVAL, FW_NOSPC, FW_STATE, FW_FENCED, FW_DMA,
    FW_TIMEOUT, FW_UNSUP, FW_NO_NEG, FW_SEQ_CONFLICT, FW_STALE_SEQ,
    FW_BUSY, FW_OVERFLOW, FW_BAD_DESC, FW_BAD_QUEUE, FW_IO
} fw_status;
enum fw_state {
    FW_FREE=0, FW_PROVISIONING, FW_ACTIVE, FW_QUIESCING, FW_RESETTING,
    FW_RECOVERING, FW_FAILED, FW_DESTROYING, FW_STATE_COUNT
};
enum fw_event {
    FW_CREATE=0, FW_COMMIT, FW_ABORT_OK, FW_ABORT_FAIL, FW_FENCE,
    FW_DRAIN_OK, FW_EV_TIMEOUT, FW_RESET_DONE, FW_READY_REARM,
    FW_VERIFY_FAIL, FW_FLR, FW_DESTROY, FW_RELEASE_OK, FW_RELEASE_FAIL,
    FW_EVENT_COUNT
};
enum fw_opcode {
    FW_VERSION_NEG=1, FW_GET_STATE, FW_PROVISION, FW_QUIESCE, FW_RESET,
    FW_REARM, FW_DESTROY_CMD, FW_SET_RESERVE, FW_GLOBAL_RESET,
    FW_UPDATE_STAGE=0x20, FW_UPDATE_VALIDATE, FW_UPDATE_ACTIVATE,
    FW_UPDATE_CONFIRM, FW_UPDATE_ROLLBACK
};
enum fw_checkpoint {
    FW_CP_BEFORE_CREATE=0, FW_CP_AFTER_CREATE, FW_CP_BEFORE_COMMIT,
    FW_CP_ABORT_RELEASE, FW_CP_DESTROY_RELEASE, FW_CP_COUNT
};
struct fw_request {
    fw_u64 session;
    fw_u32 seq, opcode, target, arg[4];
};
struct fw_reply { fw_status status; fw_u32 seq, value[4]; };
struct fw_hw_snapshot {
    fw_u64 session, epoch, token;
    fw_u32 ready_gen, outstanding;
    fw_u8 fenced, killed, bme, config_valid, ready_pending, pci_present;
};
struct fw_sq { fw_u64 addr, epoch, arg; fw_u32 len; fw_u16 id; fw_u8 op, flags; };
struct fw_cq { fw_u16 id, status; fw_u32 len; fw_u64 epoch, result; fw_u8 phase; };
struct fw_queue_config {
    fw_u64 sq_base, cq_base, epoch, session;
    fw_u32 sq_entries, cq_entries;
};
#endif
