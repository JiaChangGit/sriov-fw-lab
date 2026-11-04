#define _GNU_SOURCE
#include "owner.h"
#include "lab_ring.h"
#include "lab_wire.h"
#include "toy_regs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* Exercise the actual owner workload with a synchronous device behind its
 * pread/pwrite boundary. A stale SQ descriptor advances SQ without a CQ, so
 * completion slots must never be used to locate the request's payload. */
static struct owner *active;
static uint32_t sq_head, sq_tail, cq_tail;
static const int device_fd = 12345;

ssize_t __wrap_pread(int fd, void *bytes, size_t length, off_t offset)
{
    assert(fd == device_fd && length == 4);
    uint32_t reg = (uint32_t)(offset - (off_t)active->vfio.bar0.offset);
    if (reg == TOY_VF_QUEUE_STATUS) lab_put32(bytes, TOY_QUEUE_STATUS_RUNNING);
    else if (reg == TOY_VF_IRQ_CAUSE) lab_put32(bytes, 1);
    else abort();
    return 4;
}

ssize_t __wrap_pwrite(int fd, const void *bytes, size_t length, off_t offset)
{
    assert(fd == device_fd && length == 4);
    uint32_t reg = (uint32_t)(offset - (off_t)active->vfio.bar0.offset);
    uint32_t value = lab_get32(bytes);
    if (reg == TOY_VF_SQ_TAIL) sq_tail = value;
    else if (reg == TOY_VF_DOORBELL) {
        uint64_t completed = 0;
        assert(value == 1);
        while (sq_head != sq_tail) {
            unsigned char *sq = (unsigned char *)active->maps[OWNER_SQ].memory +
                lab_ring_index(sq_head, active->entries) * 32;
            unsigned char *cq = (unsigned char *)active->maps[OWNER_CQ].memory +
                lab_ring_index(cq_tail, active->entries) * 32;
            uint64_t offset = lab_get64(sq) - active->maps[OWNER_PAYLOAD].iova;
            uint32_t count = lab_get32(sq + 8);
            assert(offset <= active->maps[OWNER_PAYLOAD].size &&
                count <= active->maps[OWNER_PAYLOAD].size - offset);
            unsigned char *payload = (unsigned char *)active->maps[OWNER_PAYLOAD].memory + offset;
            uint32_t result = 0;
            assert(lab_get64(sq + 16) == active->epoch);
            if (sq[12] == TOY_SQ_OPS_FILL) memset(payload, (unsigned char)lab_get64(sq + 24), count);
            else {
                assert(sq[12] == TOY_SQ_OPS_CRC32);
                result = lab_crc32(payload, count);
            }
            memset(cq, 0, 32);
            lab_put16(cq, lab_get16(sq + 14));
            lab_put32(cq + 4, count);
            lab_put64(cq + 8, active->epoch);
            lab_put64(cq + 16, result);
            cq[24] = lab_ring_phase(cq_tail, active->entries);
            ++sq_head; ++cq_tail; ++completed;
        }
        assert(completed && write(active->eventfds[0], &completed, sizeof(completed)) == sizeof(completed));
    } else assert(reg == TOY_VF_CQ_HEAD || reg == TOY_VF_IRQ_CAUSE);
    return 4;
}

static void run(uint32_t sq_start, uint32_t cq_start, uint32_t entries)
{
    struct owner o = {0};
    active = &o;
    o.vfio.device = device_fd;
    o.vfio.bar0.offset = 0x1000;
    o.vfio.bar0.size = TOY_LIMITS_VF_BAR0_BYTES;
    o.epoch = 7;
    o.entries = entries;
    o.sq_tail = sq_head = sq_tail = sq_start;
    o.cq_head = cq_tail = cq_start;
    o.eventfds[0] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(o.eventfds[0] >= 0);
    for (unsigned i = 0; i < OWNER_MAPS; ++i) {
        o.maps[i].size = entries * (i == OWNER_PAYLOAD ? 8192 : 32);
        o.maps[i].memory = calloc(1, o.maps[i].size);
        assert(o.maps[i].memory);
    }
    o.maps[OWNER_PAYLOAD].iova = 0x400000;
    errno = 0;
    int result = owner_workload(&o, 2 * entries + 3);
    if (result) fprintf(stderr, "WORKLOAD_CURSOR_FAILURE sq=%u cq=%u entries=%u errno=%d\n",
        sq_start, cq_start, entries, errno);
    assert(!result);
    assert(o.completed == 2 * entries + 3);
    assert(o.sq_tail == (uint32_t)(sq_start + o.completed));
    assert(o.cq_head == (uint32_t)(cq_start + o.completed));
    assert(!close(o.eventfds[0]));
    for (unsigned i = 0; i < OWNER_MAPS; ++i) free(o.maps[i].memory);
}

int main(void)
{
    for (uint32_t entries = 16; entries <= 256; entries *= 2) {
        run(0, 0, entries);
        run(1, 0, entries);
        run(UINT32_MAX - 7, UINT32_MAX - 8, entries);
    }
    puts("OWNER_WORKLOAD_TEST_PASS aligned=5 stale_sq_gap=5 counter_wrap=5");
    return 0;
}
