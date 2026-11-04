# Portable controller core API

The public entry point is `fw_core.h`. Every `fw_context`, HAL object, request,
reply, descriptor buffer, snapshot, and output array is caller-owned. No API takes
ownership, allocates memory, stores a pointer to a request, or retains an output
buffer. `fw_init` copies the HAL structure; its `opaque` referent must outlive the
context. The adapter serializes all calls for one context. HAL callbacks must not
reenter that context.

The core owns VF state, admission accounting, mailbox negotiation/cache and
scheduler statistics. The adapter alone owns hardware session/epoch/token,
fence/kill, PCI state, queue configuration and retained operations. `read_hw`
returns one internally consistent snapshot. Its boolean fields are exactly 0 or
1. Core snapshots include live HAL state without mutating the authoritative
context. Invalid state/event pairs return `FW_STATE` before any HAL call and leave
every byte of the context unchanged.

The native build uses C11, `-ffreestanding -fno-builtin` and fixed arrays. UEFI
builds define `FW_UEFI` to select EDK2 integer types through `Uefi.h`; the same C
sources are linked. `fw_zero` and `fw_copy` avoid a libc requirement. Copy buffers
must not overlap. CRC callers provide at least `length` readable bytes; a null
pointer is permitted only for length zero. Codec entry buffers always contain
exactly 32 readable/writable bytes. A rejected decoder does not publish a partial
output structure.

## HAL and lifecycle ordering

`owners_closed(vf)` reports the adapter's observable closure condition. The QEMU
adapter checks that the VF PCI function is absent; it cannot inspect guest VFIO
fds or leases. The trusted PF driver and labd enforce owner shutdown, unbind and
lease release before PCI disable. A global query requires all VFs to be absent.

`set_gate(open=1)` validates queue/BME/kill again and may only succeed after its
hardware changes are committed. It schedules later work; it must not issue DMA,
IRQ, or a recursive core call before returning, because the core commits ACTIVE
immediately after this callback succeeds. On failure it must keep the gate closed.
Cancel/clear/kill callbacks complete synchronously. Error paths may make hardware
safer (fence/cancel), but cannot speculatively reopen it. Quota remains reserved
when cleanup fails.

For a normal reset the adapter performs FENCE_BEGIN (fence, cancel pending work,
clear pending IRQs, then checked epoch/cancel-token increment), then calls FENCE, DRAIN_OK, RESET_DONE. Configuration remains
invalid and the fence remains set in RECOVERING. The owner installs a current
session/epoch queue, producing a nonzero `ready_gen`; `fw_rearm` validates all
three before the HAL opens the gate. An initial ACTIVE provision also needs this
configuration/rearm handshake. FLR first completes the hardware fence/advance/
clear operation, then sends FLR to the core. FLR never releases admission quota.

`fw_global_recover` is a transaction commit, not an arbitrary state setter. The
adapter must first close owners, disable VFs, fence/cancel/clear all queues and
advance the global session plus every epoch. Recovery checks all these facts
before atomically clearing slots, ledger, negotiation and mailbox cache. Existing
kill latches are allowed and remain latched until the adapter's `end_global`
callback clears them, with every fence still set. Neither generation may wrap.
Successful mailbox GLOBAL_RESET returns the new session but never inserts the
old-session request into its fresh cache. An old-session retry returns STALE_SEQ;
the host reconciles through its global-operation token and session readback.

Quiesce/reset deadlines are 100 logical ticks; recovery readiness is 1000 ticks.
TIMEOUT events before the stored deadline are rejected. The adapter invokes
`fw_poll_deadlines` using its virtual-time source. Host wall-clock timeouts remain
a separate driver/controller responsibility. A RECOVERING function whose PCI
presentation has been removed has no tenant READY endpoint, so readiness timeout
polling retains its fenced state and quota until DESTROY. This specifically covers
Linux's SR-IOV disable removal delay; a present VF still has the full readiness
deadline, and quiesce/reset deadlines do not change.

## Mailbox and scheduling

Protocol rejection (INVAL, UNSUP, NO_NEG, SEQ_CONFLICT, STALE_SEQ) leaves the full
context unchanged. Requests which pass protocol validation cache their current
reply, including STATE/device errors and a pending QUIESCE/RESET's BUSY status.
Polling updates that cached status when the transaction completes. Replaying the
same request does not repeat its partial side effects. The last key and all command fields are compared individually rather
than comparing structure padding. Sequence zero and wrap are never accepted.

The scheduler receives a fixed backlog snapshot. The adapter supplies zero for
CQ-full, hung or otherwise unavailable queues. The core additionally filters
non-ACTIVE, fenced, killed, BME-disabled, unconfigured or retained-operation VFs.
The return array is published only after all checks and checked cumulative-counter
updates succeed. It contains admission guarantees plus weighted largest-remainder
redistribution, bounded by 64 operations and eight VFs. A zero-work round advances
the tie rotation but adds no services. This measures logical service, not physical
PCIe throughput.

## Executable evidence

`python3 tools/build_core.py` builds the static archive, native units, ASan/UBSan
units and oracle driver, gcov instrumentation, and two libFuzzer executables.
`python3 tools/verify_core.py` runs the complete core gate. Its defaults require
four seeds with 100000 operations each, line coverage at least 90%, and both
fuzzers at 100000 iterations followed by a separate 300-second exploration.
Reduced budgets report NOT_RUN for the required gate. Logs, full oracle traces,
compiler commands, source hashes and machine-readable results are retained under
`artifacts/verify-core`; the script works from any current directory.

The Python oracle is authored directly from the specification and imports no C
sources or generated tables. It compares full state, hardware mirrors, accounting,
mailbox metadata, service counters and every reply after each operation. Unit
fixtures independently enumerate the 20 legal and 92 illegal state/event pairs,
check byte-identical rejection, and exercise failure paths and finite-width limits.
