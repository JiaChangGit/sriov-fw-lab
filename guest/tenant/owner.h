#ifndef LAB_OWNER_H
#define LAB_OWNER_H
#include "vfio.h"
#include <stdbool.h>
#define OWNER_MAPS 3
enum owner_map_kind { OWNER_SQ, OWNER_CQ, OWNER_PAYLOAD };
struct owner_map {
    void *memory;
    size_t size;
    size_t dma_size;
    uint64_t iova;
    uint32_t permissions;
    bool mapped;
};
struct owner {
    struct lab_vfio vfio;
    struct owner_map maps[OWNER_MAPS];
    void *held[OWNER_MAPS];
    struct owner_map retired[OWNER_MAPS];
    void *retired_guard[OWNER_MAPS];
    unsigned char pattern;
    int eventfds[2];
    bool irq_enabled, cleanup_failed;
    uint32_t entries, sq_tail, cq_head;
    uint64_t session, epoch, irq_wakes, irq_count, completed;
    char failpoint[64];
    unsigned fail_hit, fail_seen;
    bool fail_fired;
    unsigned dma_fault;
    bool reset_test;
    bool keep_unmapped;
    unsigned recovery_case;
    bool pending_request;
    uint64_t pending_epoch,cancelled;
};
void owner_init(struct owner *o);
int owner_open(struct owner *o, const char *bdf, unsigned group);
int owner_read32(struct owner *o, uint32_t reg, uint32_t *value);
int owner_write32(struct owner *o, uint32_t reg, uint32_t value);
int owner_latch(struct owner *o, uint32_t *flags);
int owner_configure(struct owner *o, uint32_t entries);
int owner_workload(struct owner *o, unsigned requests);
int owner_pattern_pair(struct owner *o);
int owner_descriptor_vectors(struct owner *o);
int owner_dma_fault(struct owner *o);
int owner_reset_clear(struct owner *o, const char *cause);
int owner_vfio_reset(struct owner *o);
int owner_submit_pending(struct owner *o);
int owner_retire(struct owner *o);
int owner_retired_unchanged(struct owner *o);
int owner_freeze(struct owner *o);
int owner_unchanged(struct owner *o, uint32_t digests[OWNER_MAPS]);
int owner_unmap(struct owner *o, bool fence_verified);
int owner_close(struct owner *o, bool fence_verified);
#endif
