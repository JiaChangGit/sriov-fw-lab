"""Semantic checks for individual M3 cases; these do not approve the full gate."""
import re
import zlib
import struct


def validate_t01(cases, events, device_text, controller):
    def need(condition, reason):
        if not condition:
            raise RuntimeError("T01 semantic validation: " + reason)

    rows = lambda data, kind: [r for r in data if r.get("event") == kind]
    case = cases.get("T01", {})
    need(case.get("status") == "PASS" and case.get("cycles") == 100 and case.get("warmup_cycles") == 1 and
         case.get("owners_per_cycle") == 4 and all(case.get(k) == 0 for k in ("remaining_fds", "remaining_maps", "remaining_work")),
         "full lease-owned lifecycle budget")
    need(not re.search(r"TOY_CORE event=vf_transition vf=\d+ value=6\b", device_text), "unexpected FAILED transition")
    starts = [i for i, r in enumerate(events) if r.get("event") == "T01_CYCLE_BEGIN"]
    need(len(starts) == len(rows(events, "T01_CYCLE_END")) == 101, "warmup plus 100 measured cycles")
    pids = set()
    for cycle, start in enumerate(starts):
        part = events[start:starts[cycle+1] if cycle < 100 else len(events)]
        need(part[0].get("cycle") == cycle and part[0].get("warmup") is (cycle == 0), "ordered cycle identities")
        end = rows(part, "T01_CYCLE_END")
        need(len(end) == 1 and end[0].get("cycle") == cycle and end[0].get("status") == "PASS" and end[0].get("owners") == 4 and
             end[0].get("pci_devices_absent") == 4, "cycle completion and actual PCI function removal")
        ledger = rows(part, "T01_LEDGER")
        need(len(ledger) == 16, "enabled and disabled eight-slot ledgers")
        for enabled in (4, 0):
            state = [r for r in ledger if r.get("enabled") == enabled]
            need(len(state) == 8 and {r.get("vf") for r in state} == set(range(8)), "all VF snapshots per phase")
            for r in state:
                active = enabled == 4 and r["vf"] < 4
                need(r.get("cycle") == cycle and r.get("state") == (2 if active else 0) and
                     r.get("reserve") == (8 if active else 0) and r.get("flags") == (5 if active else 1) and
                     r.get("outstanding") == r.get("lease") == 0, "bounded quota, gate and no retained lease")
        owner_starts = [i for i, r in enumerate(part) if r.get("event") == "T01_OWNER_BEGIN"]
        need(len(owner_starts) == 4, "four lease-owned opens each cycle")
        fences = [r for r in rows(part, "M3_CONTROL") if r.get("command") == 5]
        reaps = rows(part, "M3_OWNER_REAP")
        need(len(fences) == len(reaps) == 4 and max(part.index(r) for r in fences) < min(part.index(r) for r in reaps),
             "all owners receive a fenced stop before waiting for individual process exits")
        groups = rows(part, "T01_GROUP")
        need(len(groups) == 4 and len({g.get("group") for g in groups}) == 4 and
             len({g.get("bdf") for g in groups}) == 4, "four distinct domains each cycle")
        for vf, owner_start in enumerate(owner_starts):
            need(part[owner_start].get("cycle") == cycle and part[owner_start].get("vf") == vf, "ordered owner identity")
            peer_matches = [p for p in rows(part, "M3_PEER_VERIFIED") if p.get("vf") == vf]
            need(len(peer_matches) == 1, "one peer per VF")
            pid = peer_matches[0].get("pid")
            group = groups[vf]
            owner = [r for r in part if r.get("pid") == pid or
                     r.get("event") == "M3_CONTROL" and r.get("vf") == vf or
                     r.get("event") == "vfio_open" and r.get("vf") == group.get("bdf")]
            peers, opened, closed, reaped, controlled, mapped, unmapped, worked = (
                rows(owner, name) for name in ("M3_PEER_VERIFIED", "vfio_open", "OWNER_EXIT", "M3_OWNER_REAP",
                                              "M3_CONTROL", "OWNER_MAP", "OWNER_UNMAP", "OWNER_WORKLOAD"))
            need(len(peers) == len(opened) == len(closed) == len(reaped) == len(worked) == 1 and
                 len(mapped) == len(unmapped) == 3 and len(controlled) == 4, "complete owner lifecycle evidence")
            peer, opened, closed, reaped, worked = peers[0], opened[0], closed[0], reaped[0], worked[0]
            pid = peer.get("pid")
            need(type(pid) is int and pid > 0 and pid not in pids and peer.get("vf") == vf and peer.get("pidfd") is True, "registered fresh owner process")
            pids.add(pid)
            group = groups[vf]
            need(group.get("vf") == vf and group.get("members") == 1 and opened.get("group") == group["group"] and
                 opened.get("vf") == group["bdf"] and all(opened.get(k, -1) >= 0 for k in ("container_fd", "group_fd", "device_fd")),
                 "real viable container and isolated group")
            need([r.get("vf") for r in controlled] == [vf]*4 and [r.get("command") for r in controlled] == [3, 7, 5, 4] and
                 [r.get("operation") for r in controlled] == [1, 2, 3, 4] and
                 all(r.get("errno") == r.get("fw_status") == 0 for r in controlled), "lease/config/rearm/fence/release")
            need(controlled[2]["epoch"] > controlled[1]["epoch"] and controlled[2]["flags"] & 1 and
                 controlled[2]["outstanding"] == 0, "real hardware fence before cleanup")
            need({m.get("kind") for m in mapped} == {0, 1, 2}, "all typed maps")
            for m in mapped:
                kind = m["kind"]
                match = [u for u in unmapped if u.get("kind") == kind]
                need(m.get("pid") == pid and m.get("bdf") == group["bdf"] and m.get("flags") == kind+1 and
                     m.get("size") == [4096, 4096, 131072][kind] and len(match) == 1 and match[0].get("pid") == pid and
                     match[0].get("requested") == match[0].get("returned") == m["size"], "exact owner UNMAP")
                need(owner.index(controlled[0]) < owner.index(opened) < owner.index(m) < owner.index(controlled[1]) <
                     owner.index(controlled[2]) < owner.index(match[0]) < owner.index(closed), "lease covers open/map/fence/unmap/close")
            need(closed.get("pid") == reaped.get("pid") == worked.get("pid") == pid and closed.get("vf") == reaped.get("vf") == vf and
                 closed.get("baseline_fds", 0) >= 3 and closed.get("baseline_fds") == closed.get("final_fds") and
                 all(closed.get(k) == 0 for k in ("fds", "maps", "eventfds", "buffers")) and
                 reaped.get("pidfd") is True and reaped.get("exit_code") == 0 and
                 owner.index(closed) < owner.index(reaped) < owner.index(controlled[3]), "measured cleanup, reap then release")
            need(worked.get("entries") == 16 and worked.get("completed") == worked.get("sq_tail") == worked.get("cq_head") == 0,
                 "open/config/close cycle without workload substitution")
        need(max(part.index(p) for p in rows(part, "M3_PEER_VERIFIED")) <
             min(part.index(c) for c in rows(part, "M3_CONTROL") if c["command"] == 5), "all four owners live before closing begins")
        quiet = rows(part, "T01_LIFECYCLE")
        need(len(quiet) == 1, "one measured quiet state per cycle")
        q = quiet[0]
        need(q.get("cycle") == cycle and q.get("contexts") == 1 and q.get("tasks") == q.get("baseline_tasks") == 1 and
             q.get("fds", 0) >= 4 and q.get("fds") == q.get("baseline_fds") and
             all(q.get(k) == 0 for k in ("leases", "orphaned", "events", "work_busy", "pending_irqs", "children")) and
             part.index(q) > part.index(ledger[-1]), "driver work/event/context and process/fd cleanup")
    need(len(pids) == 404 and len(controller.records) == 3 and all(r["status"] == "PASS" for r in controller.records) and
         not controller.armed and set(controller.inventory) == {"cold", "initial", "final"} and
         controller.inventory["initial"]["children"] == controller.inventory["final"]["children"] and
         all(not any(s["owned"].values()) for s in controller.inventory.values()), "acknowledged steady QOM ownership inventory")
    return {"status": "PASS", "scope": "T01_full_lease_lifecycle", "measured_cycles": 100, "warmup_cycles": 1,
            "measured_owner_exits": 400, "exact_unmaps_including_warmup": 1212, "remaining_m3_gate": "NOT_RUN",
            "evidence": {"groups.json": rows(events, "T01_GROUP"), "lifecycle-counters.json": rows(events, "T01_LIFECYCLE"),
            "vfio-ownership.json": [r for r in events if r.get("event") in
                ("T01_OWNER_BEGIN", "M3_PEER_VERIFIED", "M3_CONTROL", "OWNER_MAP", "OWNER_UNMAP", "OWNER_EXIT", "M3_OWNER_REAP")]}}


def validate_t03(cases, events, entries, device_text, case_id="T03"):
    def need(condition, reason):
        if not condition:
            raise RuntimeError(case_id + " semantic validation: " + reason)

    def rows(kind):
        return [row for row in events if row.get("event") == kind]

    case = cases.get(case_id, {})
    need(not re.search(r"TOY_CORE event=vf_transition vf=\d+ value=6\b", device_text),
         "unexpected FAILED transition during positive workload")
    checks = case.get("checks", {})
    need(case.get("status") == "PASS" and checks.get("entries") == entries, "case/variant")
    need(checks.get("requests", 0) >= 4096 and checks.get("owner_closed") is True
         and checks.get("all_free") is True, "requested budget and cleanup")
    work = rows("OWNER_WORKLOAD")
    need(len(work) == 1, "one completed owner workload")
    work = work[0]
    need(work.get("entries") == entries and work.get("completed") == checks["requests"], "completion budget")
    need(work.get("sq_tail") == work.get("cq_head") == work["completed"], "ring counters")
    need(0 < work.get("irq_wakes", 0) <= work.get("irq_count", 0), "actual eventfd wakeups")
    need(work["irq_wakes"] < work["completed"], "multiple CQ entries drained per eventfd wake")
    need(work.get("fixed_crc") == 0xcbf43926 and type(work.get("payload_crc")) is int, "CRC evidence")
    maps, unmaps = rows("OWNER_MAP"), rows("OWNER_UNMAP")
    need(len(maps) == len(unmaps) == 3, "three owned maps and exact unmaps")
    need({row.get("kind") for row in maps} == {0, 1, 2}, "SQ/CQ/payload map kinds")
    for row in maps:
        kind = row["kind"]
        need(row.get("pid") == work.get("pid") and row.get("flags") == [1, 2, 3][kind], "owner and DMA permissions")
        matching = [u for u in unmaps if u.get("kind") == kind]
        need(len(matching) == 1 and matching[0].get("requested") == matching[0].get("returned") == row.get("size"), "exact unmap extent")
        need(matching[0].get("pid") == row["pid"], "UNMAP remains in owner")
    peers = rows("M3_PEER_VERIFIED")
    need(len(peers) == 1 and peers[0].get("pid") == work.get("pid") and peers[0].get("pidfd") is True, "registered child credentials")
    exits = rows("OWNER_EXIT")
    need(len(exits) == 1 and exits[0].get("pid") == work.get("pid") and
         all(exits[0].get(k) == 0 for k in ("fds", "maps", "eventfds", "buffers")), "owner close counters")
    controls = rows("M3_CONTROL")
    need([row.get("command") for row in controls] == [3, 7, 5, 4], "acquire/rearm/fence/release ordering")
    need([row.get("operation") for row in controls] == [1, 2, 3, 4], "per-open operations")
    need(all(row.get("errno") == 0 and row.get("fw_status") == 0 for row in controls), "successful controls")
    need(controls[2]["epoch"] > controls[1]["epoch"] and controls[2]["flags"] & 1 and
         controls[2]["outstanding"] == 0, "new fenced epoch before owner cleanup")
    need(events.index(controls[2]) < events.index(unmaps[0]) < events.index(exits[0]) < events.index(controls[3]), "cleanup happens after hardware fence, before release")
    return {"status": "PASS", "scope": case_id + "_single_entries_variant", "entries": entries,
            "verified_completions": work["completed"], "exact_owner_unmaps": 3,
            "remaining_m3_gate": "NOT_RUN"}


def validate_t04(cases, events, entries, device_text):
    def need(condition, reason):
        if not condition:
            raise RuntimeError("T04 semantic validation: " + reason)

    rows = lambda kind: [r for r in events if r.get("event") == kind]
    case = cases.get("T04", {})
    checks = case.get("checks", {})
    need(case.get("status") == "PASS" and checks.get("owners") == 2, "two completed owners")
    peers, groups, opened, work = (rows(k) for k in ("M3_PEER_VERIFIED", "T04_GROUP", "vfio_open", "OWNER_WORKLOAD"))
    need(len(peers) == len(groups) == len(opened) == len(work) == 2, "two actual peers/groups/containers/workloads")
    need({p.get("vf") for p in peers} == {0, 1} and len({p.get("pid") for p in peers}) == 2,
         "two distinct registered processes")
    need({g.get("vf") for g in groups} == {0, 1} and len({g.get("group") for g in groups}) == 2 and
         len({g.get("bdf") for g in groups}) == 2 and all(g.get("members") == 1 for g in groups), "isolated group membership")
    need(len(rows("OWNER_MAP")) == len(rows("OWNER_UNMAP")) == len(rows("OWNER_HELD_CHECK")) == 6,
         "both full map sets, exact UNMAPs and bytewise guard checks")
    need(len(rows("T04_INSPECT")) == 2 and len(rows("M3_CONTROL")) == 8 and len(rows("OWNER_EXIT")) == 2,
         "both inspected and independently closed")
    need(len(rows("M3_OWNER_REAP")) == 2, "two successful pidfd reaps")
    need(len(rows("OWNER_PATTERN_PAIR")) == 2 and len(rows("OWNER_PATTERN_COMMAND")) == 4,
         "each owner fills then CRCs the same payload")
    need(work[0].get("payload_crc") != work[1].get("payload_crc"), "different resulting payloads")
    traces = []
    for match in re.finditer(r"TOY_DMA_OK vf=(\d+) rid=([0-9a-f]+) role=(\w+) addr=([0-9a-f]+) length=(\d+) write=(\d+) epoch=(\d+)", device_text):
        vf, rid, role, addr, length, write, epoch = match.groups()
        traces.append(dict(vf=int(vf), rid=int(rid, 16), role=role, addr=int(addr, 16),
                           length=int(length), write=int(write), epoch=int(epoch)))
    need(len(traces) == 10, "actual first successful SQ/payload/CQ/phase DMA for each VF")
    for vf in (0, 1):
        peer = next(p for p in peers if p["vf"] == vf)
        group = next(g for g in groups if g["vf"] == vf)
        pid, bdf = peer["pid"], group["bdf"]
        matching_open = [o for o in opened if o.get("vf") == bdf and o.get("group") == group["group"]]
        need(len(matching_open) == 1 and all(matching_open[0].get(k, -1) >= 0 for k in
             ("container_fd", "group_fd", "device_fd")), "successful independent TYPE1 open")
        part = [r for r in events if r.get("pid") == pid or r.get("event") == "M3_CONTROL" and r.get("vf") == vf]
        validate_t03({"T04": case}, part, entries, device_text, "T04")
        maps = sorted((r for r in part if r["event"] == "OWNER_MAP"), key=lambda r: r["kind"])
        need([m.get("iova") for m in maps] == [0x100000, 0x200000, 0x400000] and
             [m.get("size") for m in maps] == [max(4096, entries*32)]*2 + [entries*8192], "identical numeric IOVAs and extents")
        current_work = next(w for w in work if w["pid"] == pid)
        need(current_work.get("bdf") == bdf and work[vf] == current_work, "A workload followed by B")
        pairs = [r for r in part if r["event"] == "OWNER_PATTERN_PAIR"]
        commands = [r for r in part if r["event"] == "OWNER_PATTERN_COMMAND"]
        need(len(pairs) == 1 and len(commands) == 2 and [c.get("step") for c in commands] == [0, 1], "FILL then CRC pair order")
        pair = pairs[0]
        need(pair.get("completed") == pair.get("sq_tail") == pair.get("cq_head") == checks["requests"]+2 and
             pair.get("bdf") == bdf, "two additional verified completions")
        expected_crc = zlib.crc32(bytes([0x31 + vf*49])*4096)
        for step, command in enumerate(commands):
            need(command.get("iova") == 0x400000 and command.get("bytes") == 4096 and
                 command.get("pattern") == 0x31 + vf*49 and command.get("expected_crc") == expected_crc and
                 command.get("result") == (expected_crc if step else 0), "device FILL/CRC of distinct known pattern")
            need(events.index(current_work) < events.index(command) < events.index(pair), "pair follows checked workload")
        held = [r for r in part if r["event"] == "OWNER_HELD_CHECK"]
        need([h.get("kind") for h in held] == [0, 1, 2], "whole SQ/CQ/payload guards")
        inspect = [r for r in part if r["event"] == "T04_INSPECT"]
        need(len(inspect) == 1 and inspect[0].get("vf") == vf and inspect[0].get("request") == (3 if vf == 0 else 2), "authenticated inspection phase")
        inspect = inspect[0]
        for h, m, key in zip(held, maps, ("sq_crc", "cq_crc", "payload_crc")):
            need(h.get("bdf") == bdf and h.get("pattern") == vf*49 and h.get("bytes") == m["size"] and
                 h.get("completed") == (checks["requests"]+2 if vf == 0 else 0) and h.get("bytewise_unchanged") is True and
                 h.get("crc") == inspect.get(key), "owner bytewise comparison and authenticated CRC reply")
            if vf == 1:
                need(h["crc"] == zlib.crc32(bytes([0xa5 ^ 49 if h["kind"] == 2 else 0])*m["size"]), "B retains initial guard bytes")
            need(events.index(work[0]) < events.index(h) < events.index(work[1]) if vf == 1 else
                 events.index(work[1]) < events.index(h), "guard inspected after the other VF ran")
        if vf == 0:
            need(inspect["payload_crc"] == pair["payload_crc"], "A retains its completed payload")
        controls = [r for r in part if r["event"] == "M3_CONTROL"]
        reaped = [r for r in part if r["event"] == "M3_OWNER_REAP"]
        exit_row = next(r for r in part if r["event"] == "OWNER_EXIT")
        need(len(reaped) == 1 and reaped[0].get("vf") == vf and reaped[0].get("pidfd") is True and
             reaped[0].get("exit_code") == 0 and events.index(exit_row) < events.index(reaped[0]) < events.index(controls[-1]),
             "lease release follows successful owner process exit")
        per_vf = [t for t in traces if t["vf"] == vf]
        need({t["role"] for t in per_vf} == {"sq", "payload_read", "payload_write", "cq", "cq_phase"}, "all DMA roles")
        _, bus, devfn = bdf.split(":")
        dev, fn = devfn.split(".")
        rid = (int(bus, 16) << 8) | (int(dev, 16) << 3) | int(fn, 16)
        for t in per_vf:
            kind = 0 if t["role"] == "sq" else 1 if t["role"].startswith("cq") else 2
            m = maps[kind]
            need(t["rid"] == rid and t["epoch"] == controls[1]["epoch"] and
                 t["write"] == int(t["role"] in ("cq", "cq_phase", "payload_write")) and
                 m["iova"] <= t["addr"] < t["addr"]+t["length"] <= m["iova"]+m["size"], "actual VF requester, mapped extent and DMA direction")
            if t["role"] == "cq_phase":
                need(t["length"] == 1 and t["addr"] % 32 == 24, "independent phase-last write")
    final = rows("T04_FINAL")
    need(len(final) == 8 and {r.get("vf") for r in final} == set(range(8)) and
         all(r.get("flags") == 1 and all(r.get(k) == 0 for k in ("state", "reserve", "outstanding", "lease")) for r in final),
         "all eight slots FREE without leases or pending DMA")
    return {"status": "PASS", "scope": "T04_two_independent_domains", "owners": 2,
            "verified_completions": 2*(checks["requests"]+2), "whole_buffer_checks": 6, "exact_owner_unmaps": 6,
            "remaining_m3_gate": "NOT_RUN", "evidence": {"groups.json": groups, "vfio-maps.json": rows("OWNER_MAP"),
            "buffer-digests.json": rows("OWNER_HELD_CHECK"), "dma-requester-evidence.json": traces}}


def validate_t05(cases, events, variant, device_text):
    def need(condition, reason):
        if not condition: raise RuntimeError("T05 semantic validation: " + reason)

    rows = lambda kind: [r for r in events if r.get("event") == kind]
    case = cases.get("T05", {})
    checks = case.get("checks", {})
    need(case.get("status") == "PASS" and checks.get("variant") == variant and variant in range(1, 7) and
         checks.get("entries") == 16 and checks.get("healthy_requests") == 4096 and checks.get("owners") == 2 and
         checks.get("owner_closed") is True and checks.get("all_free") is True, "case variant and budget")
    peers, groups, opened = (rows(k) for k in ("M3_PEER_VERIFIED", "T05_GROUP", "vfio_open"))
    need(len(peers) == len(groups) == len(opened) == 2 and {p.get("vf") for p in peers} == {0, 1} and
         len({p.get("pid") for p in peers}) == 2 and {g.get("vf") for g in groups} == {0, 1} and
         len({g.get("group") for g in groups}) == len({g.get("bdf") for g in groups}) == 2 and
         all(p.get("pidfd") is True for p in peers) and all(g.get("members") == 1 for g in groups), "two owned isolated domains")
    fault = rows("T05_FAULT")
    need(len(fault) == 1, "one real fault submission")
    fault = fault[0]
    pids = {p["vf"]: p["pid"] for p in peers}
    need(fault.get("pid") == pids[0] and fault.get("variant") == variant and
         fault.get("epoch_after", 0) > fault.get("epoch_before", 0) > 0 and fault.get("flags", 0) & 3 == 3 and
         fault.get("queue_status") == 3 and fault.get("queue_error") == 5 and
         fault.get("payload_changed") == ({2: 16, 6: 8}.get(variant, 0)) and fault.get("payload_bytes") == 131072 and
         fault.get("partial_write") is (variant == 2) and fault.get("cpu_rollback") is False and
         fault.get("sq_unchanged") is True and fault.get("other_cq_unchanged") is True, "fenced fault and honest partial writes")
    address, length, opcode = 0x400000 + (4080 if variant == 2 else 0), 32 if variant == 2 else 8, 2 if variant == 4 else 1
    need(bytes.fromhex(fault.get("descriptor_hex", "")) == struct.pack("<QIBBHQQ", address, length, opcode, 0, 0x505,
         fault["epoch_before"], 0x73), "actual descriptor bytes")
    completion = struct.pack("<HHIQQB7x", 0x505, 5, length, fault["epoch_before"], 0, 1) if variant <= 4 else b""
    need(bytes.fromhex(fault.get("completion_hex", "")) == completion, "DMA_ERROR only with safe CQ")
    if variant >= 5: need(fault.get("vf_irq_count") == 0, "no completion IRQ after SQ/CQ failure")
    need(re.findall(r"TOY_CORE event=vf_transition vf=(\d+) value=6\b", device_text) == ["0"], "only A enters FAILED")
    errors = re.findall(r"TOY_DMA_ERROR vf=(\d+) rid=([0-9a-f]+) addr=([0-9a-f]+) length=(\d+) write=(\d+)", device_text)
    expected_addr = {2: 0x401000, 5: 0x100000, 6: 0x200000}.get(variant, 0x400000)
    expected_length = {2: 16, 5: 32, 6: 24}.get(variant, 8)
    need(errors == [("0", "0101", format(expected_addr, "x"), str(expected_length), str(int(variant not in (4, 5))))],
         "actual failed VF requester, address, length and direction")
    need(re.findall(r"TOY_FAULT vf=(\d+) rid=([0-9a-f]+) reason=(\d+) core_status=(\d+)", device_text) ==
         [("0", "0101", "1", "0")], "device failure handler ran successfully")
    counters = rows("T05_COUNTERS")
    need([(r.get("vf"), r.get("phase")) for r in counters] == [(0, "before"), (1, "before"), (0, "after"), (1, "after")], "both counter baselines and outcomes")
    for c in counters:
        bad = c["vf"] == 0 and c["phase"] == "after"
        need(c.get("dma_fault") == int(bad) and c.get("state") == (6 if bad else 2) and c.get("reserve") == 8 and
             c.get("lease", 0) > 0 and all(c.get(k) == 0 for k in ("stale_desc", "stale_cpl", "outstanding")), "independent fault counters and retained quota")
    need(events.index(counters[1]) < events.index(fault) < events.index(counters[2]), "fault surrounded by counter samples")
    need(len(rows("M3_CONTROL")) == 8 and len(rows("OWNER_EXIT")) == len(rows("M3_OWNER_REAP")) == 2 and
         len(rows("OWNER_HELD_CHECK")) == 6 and len(rows("T05_INSPECT")) == 2 and
         len(rows("OWNER_WORKLOAD")) == len(rows("OWNER_PATTERN_PAIR")) == 1 and len(rows("OWNER_PATTERN_COMMAND")) == 2,
         "both complete cleanup and guards, only B performs healthy work")
    rid_map = []
    for vf in (0, 1):
        pid = pids[vf]
        group = next(g for g in groups if g["vf"] == vf)
        need(group.get("bdf") == f"0000:01:00.{vf+1}", "observed BDF and expected minimal topology")
        matching = [o for o in opened if o.get("vf") == group["bdf"] and o.get("group") == group["group"]]
        need(len(matching) == 1 and all(matching[0].get(k, -1) >= 0 for k in ("container_fd", "group_fd", "device_fd")), "successful TYPE1 open")
        part = [r for r in events if r.get("pid") == pid or r.get("event") == "M3_CONTROL" and r.get("vf") == vf]
        per = lambda kind: [r for r in part if r.get("event") == kind]
        maps, unmaps, ctl = (per(k) for k in ("OWNER_MAP", "OWNER_UNMAP", "M3_CONTROL"))
        need([c.get("command") for c in ctl] == [3, 7, 5, 4] and [c.get("operation") for c in ctl] == [1, 2, 3, 4] and
             all(c.get("errno") == c.get("fw_status") == 0 for c in ctl), "per-owner lease lifecycle")
        need(ctl[2].get("epoch", 0) > ctl[1].get("epoch", 0) and ctl[2].get("flags", 0) & 1 and ctl[2].get("outstanding") == 0, "fence readback before unmapping")
        expected_kinds = [0, 1] if vf == 0 and variant == 1 else [0, 1, 2]
        need([m.get("kind") for m in maps] == expected_kinds and len(unmaps) == len(maps), "exact map set")
        for m in maps:
            kind = m["kind"]
            flags = [1, 2, 3][kind]
            if vf == 0:
                flags = {(3, 2): 1, (4, 2): 2, (5, 0): 2, (6, 1): 1}.get((variant, kind), flags)
            size = 4096 if kind != 2 or vf == 0 and variant == 2 else 131072
            need(m.get("flags") == flags and m.get("size") == size and m.get("iova") == [0x100000, 0x200000, 0x400000][kind], "actual mapping extent and permissions")
            u = [u for u in unmaps if u.get("kind") == kind]
            need(len(u) == 1 and u[0].get("requested") == u[0].get("returned") == size and events.index(ctl[2]) < events.index(u[0]), "exact owner UNMAP after fence")
        exit_row, reap = per("OWNER_EXIT"), per("M3_OWNER_REAP")
        need(len(exit_row) == len(reap) == 1 and reap[0].get("exit_code") == 0 and reap[0].get("pidfd") is True and
             exit_row[0].get("baseline_fds", 0) >= 3 and exit_row[0].get("final_fds") == exit_row[0]["baseline_fds"] and
             all(exit_row[0].get(k) == 0 for k in ("fds", "maps", "eventfds", "buffers")) and
             events.index(unmaps[-1]) < events.index(exit_row[0]) < events.index(reap[0]) < events.index(ctl[-1]), "actual close and successful reap before release")
        held, inspect = per("OWNER_HELD_CHECK"), per("T05_INSPECT")
        need([h.get("kind") for h in held] == [0, 1, 2] and len(inspect) == 1 and inspect[0].get("request") == (2 if vf else 3), "authenticated whole-buffer inspection")
        for h, key in zip(held, ("sq_crc", "cq_crc", "payload_crc")):
            size = 131072 if h["kind"] == 2 else 4096
            need(h.get("bytes") == size and h.get("crc") == inspect[0].get(key) and h.get("bytewise_unchanged") is True and
                 events.index(fault) < events.index(h), "guard checked after A fault")
            if vf == 1:
                need(h["crc"] == zlib.crc32(bytes([0xa5 ^ 49 if h["kind"] == 2 else 0])*size), "B original memory unchanged by A")
            else:
                expected = bytearray([0xa5 if h["kind"] == 2 else 0])*size
                if h["kind"] == 0: expected[:32] = bytes.fromhex(fault["descriptor_hex"])
                elif h["kind"] == 1: expected[:len(completion)] = completion
                elif variant == 2: expected[4080:4096] = bytes([0x73])*16
                elif variant == 6: expected[:8] = bytes([0x73])*8
                need(h["crc"] == zlib.crc32(expected), "A retains exact independently computed fault outcome")
        if vf == 1:
            synthetic = {"T05": {"status": "PASS", "checks": {"entries": 16, "requests": 4096, "owner_closed": True, "all_free": True}}}
            validate_t03(synthetic, part, 16, "", "T05")
            need(len(per("OWNER_PATTERN_PAIR")) == 1 and per("OWNER_PATTERN_PAIR")[0].get("completed") == 4098 and
                 events.index(held[-1]) < events.index(per("OWNER_WORKLOAD")[0]) < events.index(counters[-1]), "healthy B work completes after A failed")
            pair = per("OWNER_PATTERN_PAIR")[0]
            need(pair.get("sq_tail") == pair.get("cq_head") == 4098, "healthy B drains its final completions")
            commands = per("OWNER_PATTERN_COMMAND")
            need([c.get("step") for c in commands] == [0, 1], "healthy B FILL then CRC")
            crc = zlib.crc32(bytes([0x62])*4096)
            for index, command in enumerate(commands):
                need(command.get("iova") == 0x400000 and command.get("bytes") == 4096 and
                     command.get("pattern") == 0x62 and command.get("expected_crc") == crc and
                     command.get("result") == (crc if index else 0), "B device computes correct payload after A failed")
            traces = re.findall(r"TOY_DMA_OK vf=1 rid=([0-9a-f]+) role=(\w+) addr=([0-9a-f]+) length=(\d+) write=(\d+) epoch=(\d+)", device_text)
            need(len(traces) == 5 and {t[1] for t in traces} == {"sq", "payload_read", "payload_write", "cq", "cq_phase"}, "B actual successful DMA roles")
            for rid, role, addr, count, direction, epoch in traces:
                kind = 0 if role == "sq" else 1 if role.startswith("cq") else 2
                m = next(m for m in maps if m["kind"] == kind)
                need(rid == "0102" and int(epoch) == ctl[1]["epoch"] and
                     int(direction) == int(role in ("cq", "cq_phase", "payload_write")) and
                     m["iova"] <= int(addr, 16) < int(addr, 16)+int(count) <= m["iova"]+m["size"], "B requester and authorized DMA range")
        else:
            need(not per("OWNER_WORKLOAD") and events.index(held[0]) > events.index(counters[-1]), "A stays unchanged while B completes")
        rid_map.append({**group, "pid": pid, "rid": 0x101 + vf})
    omitted = rows("T05_MAP_OMITTED")
    need(len(omitted) == int(variant == 1) and (not omitted or omitted[0].get("pid") == pids[0] and
         omitted[0].get("iova") == 0x400000 and omitted[0].get("size") == 131072), "A target absent while B target mapped")
    retained, final, recovery = (rows(k) for k in ("T05_POST_DISABLE", "T05_FINAL", "T05_GLOBAL_RECOVER"))
    need(len(retained) == len(final) == 8 and {r.get("vf") for r in retained} == {r.get("vf") for r in final} == set(range(8)) and
         len(recovery) == 1 and recovery[0].get("after_session", 0) > recovery[0].get("before_session", 0) > 0 and
         recovery[0].get("disable_errno") == 0 and recovery[0].get("vfs") == 0, "explicit recovery only after close and disable")
    for r in retained:
        need(r.get("state") == r.get("reserve") == r.get("lease") == 0 and
             r.get("flags") == (1 if r["vf"] else 3), "verified DESTROY releases quota while KILL remains latched")
    need(all(r.get("flags") == 1 and all(r.get(k) == 0 for k in ("state", "reserve", "outstanding", "lease")) for r in final), "all FREE after global recovery")
    need(events.index(rows("M3_CONTROL")[-1]) < events.index(retained[0]) < events.index(recovery[0]) < events.index(final[0]), "recovery chronology")
    return {"status": "PASS", "scope": "T05_DMA_fault_variant", "variant": variant, "healthy_completions": 4098,
            "evidence": {"vf-rid-map.json": rid_map, "device-fault-counters.json": counters, "guard-digests.json": rows("OWNER_HELD_CHECK")}}


def validate_t09(cases, events, entries, device_text):
    result = validate_t03(cases, events, entries, device_text, "T09")

    def need(condition, reason):
        if not condition:
            raise RuntimeError("T09 semantic validation: " + reason)

    rows = lambda kind: [r for r in events if r.get("event") == kind]
    work = rows("OWNER_WORKLOAD")[0]
    controls = rows("M3_CONTROL")
    vectors = rows("T09_VECTOR")
    names = ("old-epoch", "unknown-opcode", "zero-length", "length-over-4096", "address-overflow", "nonzero-flags")
    need([v.get("name") for v in vectors] == list(names), "all six vectors in order")
    baseline = work["completed"]
    epoch = controls[1]["epoch"]
    for index, v in enumerate(vectors):
        need(v.get("pid") == work["pid"] and v.get("live_epoch") == epoch and
             v.get("sq_head") == v.get("sq_tail") == baseline+index+1 and
             v.get("cq_before") == baseline+max(0, index-1) and v.get("cq_after") == baseline+index,
             "one SQ consumed per vector, only malformed CQ completions")
        need(v.get("completion_status") == (13 if index else -1) and v.get("payload_unchanged") is True and
             v.get("other_cq_bytes_unchanged") is True, "BAD_DESC and unchanged non-target buffers")
        need(v.get("irq_after") > v.get("irq_before") if index else v.get("irq_before") == v.get("irq_after"),
             "stale descriptor emits no observed IRQ, malformed completion wakes owner")
        addr = (1 << 64)-1025 if index == 4 else 0x400000
        length = {2: 0, 3: 4097, 4: 2048}.get(index, 8)
        expected = struct.pack("<QIBBHQQ", addr, length, 255 if index == 1 else 1, int(index == 5),
                               (baseline+index) & 0xffff, epoch if index else epoch-1, 0x73)
        need(bytes.fromhex(v.get("descriptor_hex", "")) == expected, "exact independent negative descriptor encoding")
        if index:
            expected_cq = struct.pack("<HHIQQB7x", (baseline+index) & 0xffff, 13, 0, epoch, 0,
                                      1 ^ (((baseline+index-1)//entries) & 1))
            need(bytes.fromhex(v.get("completion_hex", "")) == expected_cq, "actual CQ bytes including phase/reserved fields")
        else:
            need(v.get("completion_hex") == "", "old epoch has no completion")
        need(events.index(work) < events.index(v) < events.index(controls[2]), "vectors executed before fence")
    counters = rows("T09_COUNTERS")
    need([c.get("phase") for c in counters] == ["before", "after"], "before/after hardware counters")
    for index, c in enumerate(counters):
        need(c.get("vf") == 0 and c.get("stale_desc") == index and
             all(c.get(k) == 0 for k in ("stale_cpl", "dma_fault", "fence_blocked", "irq_suppressed")), "independent stale descriptor/completion counters")
    need(events.index(controls[1]) < events.index(counters[0]) < events.index(vectors[0]) <
         events.index(vectors[-1]) < events.index(counters[1]) < events.index(controls[2]), "counter observation surrounds real submissions")
    guards = rows("OWNER_HELD_CHECK")
    need(len(guards) == 3 and [r.get("kind") for r in guards] == [0, 1, 2] and
         all(r.get("pid") == work["pid"] and r.get("bytewise_unchanged") is True and
             r.get("completed") == baseline for r in guards), "stale descriptor leaves whole SQ/CQ/payload intact")
    result.update(scope="T09_stale_and_malformed_descriptors", negative_vectors=6, stale_descriptors=1,
                  malformed_completions=5, evidence={"descriptor-vectors.json": vectors, "counter-snapshots.json": counters})
    return result


def validate_t10(cases, events, entries, device_text):
    result = validate_t03(cases, events, entries, device_text, "T10")
    busy = [r for r in events if r.get("event") == "T10_BUSY"]
    snapshots = [r for r in events if r.get("event") == "T10_SNAPSHOT"]
    peers = [r for r in events if r.get("event") == "M3_PEER_VERIFIED"]
    controls = [r for r in events if r.get("event") == "M3_CONTROL"]
    if len(busy) != 2 or len(snapshots) != 16:
        raise RuntimeError("T10 requires two rejected operations and 16 unchanged VF snapshots")
    for op, row in enumerate(busy):
        if not (row.get("attempt") == op and row.get("requested") == [0, 2][op] and
                row.get("errno") in (1, 16) and row.get("enabled") == 4 and
                row.get("topology_unchanged") is True and row.get("ledger_unchanged") is True and
                row.get("owner_pid") == peers[0]["pid"] and
                events.index(controls[1]) < events.index(row) < events.index(controls[2])):
            raise RuntimeError("T10 rejected operation evidence invalid")
        vf_rows = [s for s in snapshots if s.get("attempt") == op]
        if len(vf_rows) != 8 or {s.get("vf") for s in vf_rows} != set(range(8)):
            raise RuntimeError("T10 requires all eight VF snapshots per attempt")
        for s in vf_rows:
            if not (s.get("unchanged") is True and s.get("reserve") == (8 if s["vf"] < 4 else 0) and
                    s.get("state") == (2 if s["vf"] < 4 else 0) and s.get("outstanding") == 0 and
                    s.get("epoch", 0) > 0 and (s.get("lease", 0) > 0 if s["vf"] == 0 else s.get("lease") == 0)):
                raise RuntimeError("T10 quota/state/lease evidence invalid")
    result.update(scope="T10_busy_lease_disable_resize", rejected_operations=2)
    return result


def validate_t02_provision(cases, events, serial_text, device_text, controller):
    def need(condition, reason):
        if not condition:
            raise RuntimeError("T02 provision semantic validation: " + reason)

    def rows(kind):
        return [row for row in events if row.get("event") == kind]

    probe_names = {"probe." + name for name in ("alloc_ctx", "enable_mem", "request_regions", "map_bar",
                   "validate_caps", "alloc_vectors", "request_irq0", "request_irq1", "version_neg", "misc_register")}
    probes = rows("T02_PROBE")
    need(len(probes) == 10 and {p.get("name") for p in probes} == probe_names, "ten distinct probe points")
    for probe in probes:
        need(probe.get("status") == "PASS" and all(probe.get(k) is True for k in
             ("failed_probe_control_absent", "clean_probe_control_present", "module_unloaded")), "probe cleanup and clean retry")
        need(serial_text.count("TOY_FAILPOINT_HIT name=" + probe["name"] + "\n") == 1, "actual probe failure hit")
    variants = rows("T02_PROVISION_CASE")
    need(len(variants) == 24 and [v.get("case_index") for v in variants] == list(range(1, 25)), "24 ordered variants")
    expected = [(p, h, 0, "", -1) for p in ("provision.before_create", "provision.after_create", "provision.before_commit")
                for h in range(1, 5)]
    expected += [("provision.host_record", h, 1, "", -1) for h in range(1, 5)]
    expected += [("provision.pci_enable", 1, 1, "", -1), ("provision.after_pci_enable", 1, 1, "", -1)]
    expected += [("provision.before_commit", h, 0, "provision.abort_release", h - 1) for h in range(1, 5)]
    expected += [("provision.pci_enable", 1, 1, name, 3) for name in ("destroy.release", "unwind.destroy")]
    ledger_rows = rows("T02_LEDGER")
    need(len(ledger_rows) == 24 * 2 * 8, "before/after recovery ledgers for every VF")
    for index, (variant, definition) in enumerate(zip(variants, expected), 1):
        point, hit, driver, unwind, retained = definition
        need(tuple(variant.get(k) for k in ("name", "hit", "driver", "unwind", "retained_vf")) == definition,
             "variant identity")
        need(variant.get("status") == "PASS" and variant.get("clean_cycle") is True and
             variant.get("expected_errno") == (12 if point == "provision.host_record" else 5), "expected error and clean retry")
        for phase in ("after_fault", "after_clean_cycle"):
            ledger = [r for r in ledger_rows if r.get("case_index") == index and r.get("phase") == phase]
            need(len(ledger) == 8 and {r.get("vf") for r in ledger} == set(range(8)), "eight unique VF snapshots")
            for row in ledger:
                failed = phase == "after_fault" and row["vf"] == retained
                need(row.get("state") == (6 if failed else 0) and row.get("reserve") == (8 if failed else 0) and
                     row.get("outstanding") == 0 and row.get("flags") == (3 if failed else 1), "retained/free quota and hardware gate")
    recoveries = rows("T02_GLOBAL_RECOVER")
    need(len(recoveries) == 6 and [r.get("case_index") for r in recoveries] == list(range(19, 25)) and
         all(r.get("after_session", 0) > r.get("before_session", 0) > 0 for r in recoveries), "six explicit global recoveries")
    driver_hits = rows("T02_DRIVER_HIT")
    expected_hits = [("forward", "provision.host_record", h) for h in range(1, 5)]
    expected_hits += [("forward", p, 1) for p in
                     ("provision.pci_enable", "provision.after_pci_enable", "provision.pci_enable", "provision.pci_enable")]
    expected_hits += [("unwind", "unwind.destroy", 1)]
    need([(r.get("slot"), r.get("name"), r.get("hit")) for r in driver_hits] == expected_hits and
         all(r.get("seen") == r["hit"] and r.get("fired") == 1 for r in driver_hits), "actual host forward/unwind hits")
    for name in {h[1] for h in expected_hits}:
        need(serial_text.count("TOY_FAILPOINT_HIT name=" + name + "\n") ==
             sum(h[1] == name for h in expected_hits), "raw driver hit cross-check")
    need(len(re.findall(r"TOY_FAILPOINT_HIT slot=", device_text)) == 21, "actual adapter hits")
    need(len(re.findall(r"TOY_CORE event=vf_transition vf=\d+ value=6\b", device_text)) == 6, "exactly six expected FAILED transitions")
    need(not controller.armed and len(controller.records) == 45 and
         all(r["status"] == "PASS" for r in controller.records), "acknowledged barriers")
    need(set(controller.inventory) == {"cold", "initial", "final"} and
         controller.inventory["initial"]["children"] == controller.inventory["final"]["children"], "steady QOM inventory")
    case = cases.get("T02_PROVISION", {})
    need(case.get("status") == "PASS" and case.get("cases") == 24 and case.get("retained_cases") == 6 and
         case.get("owner_failure_tests") == case.get("partial_pci_enumeration") == "NOT_RUN", "honest subset result")
    return {"status": "PASS", "scope": "T02_probe_and_provision_subset", "probe_points": 10,
            "provision_variants": 24, "retained_quota_cases": 6, "full_T02_status": "NOT_RUN"}


def validate_t02_owner(cases, events, device_text):
    def need(condition, reason):
        if not condition:
            raise RuntimeError("T02 owner semantic validation: " + reason)

    expected = [(name, h) for name, hits in (("owner.alloc_buffer", 3), ("owner.map_sq", 1),
                ("owner.map_cq", 1), ("owner.map_payload", 1), ("owner.eventfd", 2),
                ("owner.irq_register", 1), ("owner.unmap", 3), ("owner.short_unmap", 3)) for h in range(1, hits+1)]
    starts = [i for i, r in enumerate(events) if r.get("event") == "T02_OWNER_START"]
    need(len(starts) == 30, "15 faults each with a clean retry")
    need(not re.search(r"TOY_CORE event=vf_transition vf=\d+ value=6\b", device_text), "unexpected FAILED VF")
    for j, start in enumerate(starts):
        name, hit = expected[j // 2]
        clean = bool(j % 2)
        request = events[start]
        part = events[start+1:starts[j+1] if j+1 < len(starts) else len(events)]
        rows = lambda kind: [r for r in part if r.get("event") == kind]
        need(request.get("case_index") == j // 2+1 and request.get("name") == name and
             request.get("hit") == hit and request.get("clean_retry") is clean, "ordered fault/retry identity")
        case_rows = rows("T02_OWNER_CASE")
        need(len(case_rows) == 1, "one complete child result")
        case = case_rows[0]
        need(case.get("status") == "PASS" and case.get("name") == ("" if clean else name) and
             case.get("hit") == (1 if clean else hit) and case.get("owner_closed") is True and
             case.get("all_free") is True, "owner close and all slots FREE")
        pid = case.get("pid")
        if clean:
            need(not rows("OWNER_FAILPOINT_HIT") and not rows("OWNER_FAILURE_CLEANUP"), "clean retry has no injection")
            synthetic = {"T02_CLEAN": {"status": case["status"], "checks": {"entries": 16, "requests": 4096,
                         "owner_closed": case["owner_closed"], "all_free": case["all_free"]}}}
            validate_t03(synthetic, part, 16, "", "T02_CLEAN")
            continue
        hits, audit, reaped = rows("OWNER_FAILPOINT_HIT"), rows("OWNER_FAILURE_CLEANUP"), rows("T02_OWNER_REAP")
        need(len(hits) == len(audit) == len(reaped) == 1 and hits[0].get("name") == name and
             hits[0].get("hit") == hit, "actual once-only owner failure hit")
        audit, reaped = audit[0], reaped[0]
        stage = 3 if name in ("owner.unmap", "owner.short_unmap") else 1
        need(audit.get("pid") == pid and audit.get("name") == name and audit.get("hit") == hit and
             audit.get("status") == "PASS" and audit.get("fired") is True and audit.get("fence_verified") is True and
             audit.get("stage") == stage and audit.get("baseline_fds", 0) >= 3 and
             audit.get("final_fds") == audit["baseline_fds"] and
             all(audit.get(k) == 0 for k in ("maps", "buffers", "eventfds")), "measured owner cleanup")
        need(reaped.get("pid") == pid and reaped.get("exit_code") == 42 and reaped.get("stage") == stage,
             "expected child exit status")
        controls = rows("M3_CONTROL")
        need([r.get("command") for r in controls] == ([3, 7, 5, 4] if stage == 3 else [3, 5, 4]) and
             [r.get("operation") for r in controls] == list(range(1, len(controls)+1)) and
             all(r.get("errno") == r.get("fw_status") == 0 for r in controls), "lease/fence/close/release order")
        fence = next(r for r in controls if r["command"] == 5)
        need(fence.get("flags", 0) & 1 and fence.get("outstanding") == 0 and
             part.index(fence) < part.index(audit) < part.index(reaped) < part.index(controls[-1]), "fence before cleanup and reap before lease release")
        mapped, unmapped, quarantine = rows("OWNER_MAP"), rows("OWNER_UNMAP"), rows("OWNER_UNMAP_QUARANTINE")
        count = hit-1 if name == "owner.alloc_buffer" else {"owner.map_sq": 0, "owner.map_cq": 1, "owner.map_payload": 2}.get(name, 3)
        need(len(mapped) == count and [r.get("kind") for r in mapped] == list(range(count)) and
             len(quarantine) == (1 if stage == 3 else 0) and len(unmapped) == count-len(quarantine), "every mapped buffer accounted")
        for mapped_row in mapped:
            kind = mapped_row["kind"]
            need(mapped_row.get("pid") == pid and mapped_row.get("flags") == [1, 2, 3][kind], "map owner and permissions")
            matching = [u for u in unmapped + quarantine if u.get("kind") == kind]
            need(len(matching) == 1 and matching[0].get("pid") == pid and part.index(fence) < part.index(matching[0]), "unmap only after fence")
        for unmap in unmapped:
            size = next(m["size"] for m in mapped if m["kind"] == unmap["kind"])
            need(unmap.get("requested") == unmap.get("returned") == size, "exact successful UNMAP")
        if quarantine:
            q = quarantine[0]
            need(q.get("kind") == 3-hit and q.get("buffer_held") is True, "retained failed UNMAP buffer")
            need((q.get("ioctl_executed") is True and q.get("ioctl_errno") == 0 and
                  0 < q.get("returned", 0) < q.get("requested", 0)) if name == "owner.short_unmap" else
                 (q.get("ioctl_executed") is False and q.get("ioctl_errno") == 5), "real short return or named syscall failure")
    case = cases.get("T02_OWNER", {})
    need(case.get("status") == "PASS" and case.get("variants") == case.get("clean_retries") == 15 and
         case.get("partial_pci_enumeration") == "NOT_RUN", "honest subset scope")
    return {"status": "PASS", "scope": "T02_owner_fault_subset", "fault_variants": 15, "clean_retries": 15,
            "real_short_unmaps": 3, "full_T02_status": "NOT_RUN"}


def validate_t02_pci(cases, events, kernel_text, device_text, controller):
    def need(condition, reason):
        if not condition:
            raise RuntimeError("T02 PCI semantic validation: " + reason)
    rows = lambda kind: [r for r in events if r.get("event") == kind]
    variants, ledger = rows("T02_PCI_CASE"), rows("T02_LEDGER")
    need(len(variants) == 5 and len(ledger) == 80, "five variants and all per-VF ledgers")
    hits = re.findall(r"TOY_PCI_ENUM_FAIL nth=(\d+) added=(\d+)", kernel_text)
    need(hits == [(str(n), str(n-1)) for n in (1, 2, 3, 4, 4)], "actual PCI partial enumeration failures")
    for index, variant in enumerate(variants, 1):
        need(variant.get("case_index") == index and variant.get("nth") == min(index, 4) and
             variant.get("expected_errno") == 12 and variant.get("retained_vf") == (3 if index == 5 else -1) and
             all(variant.get(k) is True for k in ("hook_consumed", "pci_devices_absent", "clean_retry")) and
             variant.get("status") == "PASS", "fault/retry and absent PCI functions")
        for phase in ("after_fault", "after_clean_cycle"):
            snapshots = [r for r in ledger if r.get("case_index") == index and r.get("phase") == phase]
            need(len(snapshots) == 8 and {r.get("vf") for r in snapshots} == set(range(8)), "all VF snapshots")
            for row in snapshots:
                retained = index == 5 and phase == "after_fault" and row["vf"] == 3
                need(row.get("state") == (6 if retained else 0) and row.get("reserve") == (8 if retained else 0) and
                     row.get("flags") == (3 if retained else 1) and row.get("outstanding") == 0, "quota/hardware gate")
    recovery = rows("T02_GLOBAL_RECOVER")
    need(len(recovery) == 1 and recovery[0].get("case_index") == 5 and
         recovery[0].get("after_session", 0) > recovery[0].get("before_session", 0) > 0, "explicit retained-quota recovery")
    need(len(re.findall(r"TOY_CORE event=vf_transition vf=\d+ value=6\b", device_text)) == 1 and
         "TOY_FAILPOINT_HIT slot=unwind name=destroy.release vf=3 hit=1" in device_text,
         "actual paired unwind failure")
    need(not controller.armed and len(controller.records) == 5 and all(r["status"] == "PASS" for r in controller.records)
         and set(controller.inventory) == {"cold", "initial", "final"} and
         controller.inventory["initial"]["children"] == controller.inventory["final"]["children"], "barriers and stable QOM inventory")
    case = cases.get("T02_PCI", {})
    need(case.get("status") == case.get("partial_pci_enumeration") == "PASS" and
         case.get("variants") == 5 and case.get("retained_cases") == 1, "complete PCI subset result")
    return {"status": "PASS", "scope": "T02_partial_pci_enumeration", "variants": 5,
            "maximum_vfs_added_before_error": 3, "paired_unwind_failure": True, "full_T02_status": "NOT_RUN"}
