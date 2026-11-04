/* 裝置狀態由 QEMU main loop/BQL 序列化；fwcore 是唯一 FSM/ledger。 */
#ifndef TOY_DEVICE_H
#define TOY_DEVICE_H
#include "hw/pci/pci_device.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "fwcore/fw_core.h"
#include "include/toy_regs.h"

#define TYPE_TOY_PF "toy-sriov-pf"
#define TYPE_TOY_VF "toy-sriov-vf"
OBJECT_DECLARE_SIMPLE_TYPE(ToyPF, TOY_PF)
OBJECT_DECLARE_SIMPLE_TYPE(ToyVF, TOY_VF)

typedef struct ToyOperation {
    bool valid;
    uint64_t session, epoch, token, due;
    uint32_t cq_index;
    struct fw_sq sq;
    fw_status status;
    uint64_t result;
} ToyOperation;

typedef struct ToySlot {
    ToyVF *vf;
    uint64_t epoch, token, last_fence_epoch;
    uint32_t last_fence_token;
    bool fenced, killed, config_valid, ready_pending;
    struct fw_queue_config staging, config;
    uint64_t latch_epoch, latch_session;
    uint32_t latch_status;
    uint32_t sq_head, sq_tail, cq_head, cq_tail, ready_gen;
    uint32_t queue_status, queue_error, irq_cause, last_fault;
    uint32_t stale_desc, stale_cpl, fence_blocked, dma_fault, irq_suppressed;
    uint32_t dma_trace_seen;
    ToyOperation pending, retired;
} ToySlot;

typedef struct ToyView {
    uint32_t hw_flags, outstanding, stale_desc, stale_cpl, fence_blocked;
    uint32_t dma_fault, last_fault, irq_suppressed, ready_gen;
} ToyView;

typedef struct ToyFailpoint {
    int point;
    uint32_t hit, vf, seen, fired;
} ToyFailpoint;

struct ToyPF {
    PCIDevice parent_obj;
    MemoryRegion bar0, bar2;
    struct fw_context core;
    struct fw_snapshot snapshot;
    ToyView views[FW_VFS];
    ToySlot slots[FW_VFS];
    uint64_t session;
    uint32_t total_vfs, bad_mmio, fence_token, hw_result, event;
    struct fw_request mbx_staging, mbx_current;
    struct fw_reply mbx_reply;
    uint32_t mbx_status;
    QEMUBH *mbx_bh;
    QEMUTimer *worker;
    bool initialized, mbx_pending;
    uint32_t fault_vf, fault_after, delay_cpl_ticks;
    uint64_t operations;
    bool fw_hang_vf, fw_hang_all, drop_mbx_reply_once;
    bool ignore_quiesce, ignore_reset;
    uint32_t fault_ack;
    ToyFailpoint failpoints[2];
};

struct ToyVF {
    PCIDevice parent_obj;
    MemoryRegion bar0, bar2;
    ToyPF *pf;
    unsigned index;
};

void toy_saturate(uint32_t *counter);
uint64_t toy_tick(void);
fw_status toy_fence(ToyPF *pf, unsigned vf, uint32_t token, bool deduplicate);
void toy_kill(ToyPF *pf, unsigned vf);
void toy_clear_queue(ToyPF *pf, unsigned vf);
void toy_vf_flr(ToyPF *pf, unsigned vf);
void toy_worker(void *opaque);
void toy_schedule(ToyPF *pf);
void toy_irq(ToyPF *pf, unsigned vf, unsigned vector, const ToyOperation *op);
void toy_complete(ToyPF *pf, unsigned vf, ToyOperation *op);
void toy_fail(ToyPF *pf, unsigned vf, uint32_t reason);
bool toy_fault_target(ToyPF *pf, unsigned vf);
uint32_t toy_hw_flags(ToyPF *pf, unsigned vf);
void toy_adapter_init(ToyPF *pf);
fw_status toy_global_reset(ToyPF *pf);
void toy_mbx_work(void *opaque);
void toy_mbx_poll(ToyPF *pf);
fw_status toy_snapshot(ToyPF *pf);
fw_status toy_checkpoint(ToyPF *pf, unsigned vf, enum fw_checkpoint point);
extern const MemoryRegionOps toy_vf_mmio_ops;
#endif
