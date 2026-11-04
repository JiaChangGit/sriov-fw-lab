"""Validate actual VFIO FLR, queue generations, healthy traffic and owner cleanup."""
import re
import struct
import zlib


def buffers(completed, tail, epoch, pattern, fresh=False):
    sq, cq = bytearray(4096), bytearray(4096)
    payload = bytearray([0xa5 ^ pattern]) * (16 * 8192)
    if not fresh:
        for index in range(16):
            n, sequence = completed - 16 + index, tail - 16 + index
            length = (1, 9, 4095, 4096)[(n // 2) % 4]
            data = bytes((n + pattern + k) & 255 for k in range(length)) if n & 1 else bytes([(n + pattern) & 255]) * length
            if n & 1 and length == 9: data = b"123456789"
            payload[index*8192:index*8192+length] = data
            struct.pack_into("<QIBB HQQ", sq, index*32, 0x400000 + index*8192, length,
                             2 if n & 1 else 1, 0, sequence & 65535, epoch, (n+pattern) & 255)
            struct.pack_into("<HHIQQB", cq, index*32, sequence & 65535, 0, length, epoch,
                             zlib.crc32(data) if n & 1 else 0, 1 ^ ((sequence//16) & 1))
    return [zlib.crc32(value) for value in (sq, cq, payload)]


def validate_t06(cases, events, device):
    def need(condition, reason):
        if not condition: raise RuntimeError("T06 semantic validation: " + reason)
    positions = {id(row): index for index, row in enumerate(events)}
    position = lambda row: positions[id(row)]
    rows = lambda name: [r for r in events if r.get("event") == name]
    case = cases.get("T06", {})
    need(case.get("status") == "PASS" and case.get("ioctl_resets") == 1 and case.get("vf_a_owners") == 2
         and case.get("all_free") is True and case.get("owner_closed") is True, "case and cleanup")
    need(not re.search(r"TOY_CORE event=vf_transition vf=\d+ value=6\b|TOY_DMA_ERROR|TOY_FAULT", device), "no fault or FAILED state")
    peers, groups = rows("M3_PEER_VERIFIED"), rows("T06_GROUP")
    need(len(peers) == len(groups) == len(rows("vfio_open")) == 3 and [p.get("vf") for p in peers] == [0, 1, 0], "two A owners and one continuous B owner")
    pids = [p.get("pid") for p in peers]
    need(len(set(pids)) == 3 and all(type(p) is int and p > 0 for p in pids) and
         all(p.get("pidfd") is True for p in peers), "distinct registered processes")
    a, b, reopened = pids
    need(groups[0].get("group") == groups[2].get("group") != groups[1].get("group") and
         [g.get("bdf") for g in groups] == ["0000:01:00.1", "0000:01:00.2", "0000:01:00.1"] and
         all(g.get("members") == 1 for g in groups), "independent single-VF groups")
    clears = rows("T06_CLEAR")
    need(len(clears) == 4 and [c.get("pid") for c in clears] == [a, b, a, reopened] and
         [c.get("cause") for c in clears] == ["open", "open", "VFIO_DEVICE_RESET", "open"], "actual open and explicit reset queue readback")
    for c in clears:
        need(c.get("zero_registers") == 16 and c.get("flags", 0) & 1 and not c["flags"] & (2|16|32), "all old queue registers clear and gate closed")
    resets = rows("T06_IOCTL_RESET")
    need(len(resets) == 1, "one actual ioctl")
    reset = resets[0]
    old_epoch, new_epoch = clears[0].get("epoch"), clears[2].get("epoch")
    need(reset.get("pid") == a and reset.get("old_epoch") == old_epoch and reset.get("new_epoch") == new_epoch > old_epoch and
         reset.get("old_sq_head") == reset.get("old_cq_tail") == reset.get("completed") == 4096 and
         reset.get("ioctl_errno") == reset.get("cq_irq") == 0 and reset.get("doorbell") is True and
         reset.get("old_buffers_unchanged") is True, "ioctl advances epoch after real I/O and blocks old buffers/IRQ")
    snapshots = rows("T06_SNAPSHOT")
    phases = ["before_ioctl", "after_ioctl", "rearm_rejected", "fresh_config", "rearmed", "closed", "reopened_rearmed", "reclosed"]
    need([r.get("phase") for r in snapshots] == phases, "reset, configure, rearm, close/open phases")
    snap = dict(zip(phases, snapshots))
    for phase, state, config, fenced in zip(phases, (2,5,5,5,2,5,2,5), (1,0,0,1,1,0,1,0), (0,1,1,1,0,1,0,1)):
        r = snap[phase]
        need(r.get("vf") == 0 and r.get("state") == state and r.get("reserve") == 8 and
             r.get("outstanding") == 0 and r.get("lease", 0) > 0 and not r.get("flags", 2) & 2 and
             bool(r["flags"] & 16) == bool(config) and bool(r["flags"] & 1) == bool(fenced), "snapshot gate and ledger: " + phase)
    need(snap["before_ioctl"]["epoch"] == old_epoch and
         all(snap[p]["epoch"] == new_epoch for p in phases[1:5]) and
         snap["closed"]["epoch"] > new_epoch and clears[3]["epoch"] > snap["closed"]["epoch"] and
         snap["reopened_rearmed"]["epoch"] == clears[3]["epoch"] < snap["reclosed"]["epoch"], "monotonic ioctl/close/open generations")
    need(len({r["session"] for r in clears + snapshots}) == 1, "no hidden global reset")
    rejects = rows("T06_REARM_REJECT")
    need(len(rejects) == 1 and rejects[0].get("errno") == 16 and rejects[0].get("epoch") == new_epoch and
         rejects[0].get("ready_gen") == snap["before_ioctl"]["ready_gen"] == snap["after_ioctl"]["ready_gen"] and
         snap["fresh_config"]["ready_gen"] == snap["rearmed"]["ready_gen"] == rejects[0]["ready_gen"]+1,
         "old configuration cannot REARM; fresh generation is required")
    progress, healthy = rows("T06_HEALTHY_PROGRESS"), rows("T06_HEALTHY_CHECK")
    labels = ["started", "before_ioctl", "unconfigured", "configured_fenced", "fresh_io", "closed", "reopened", "reopened_io"]
    need(len(progress) == len(healthy) == len(labels) and [r.get("phase") for r in healthy] == labels, "healthy I/O checkpoints span every reset phase")
    previous = 0
    for p, h in zip(progress, healthy):
        need(p.get("pid") == h.get("pid") == b and p.get("epoch") == h.get("epoch") == clears[1]["epoch"] and
             p.get("session") == h.get("session") == clears[1]["session"] and p.get("flags") == h.get("flags") == 28 and
             h.get("state") == 2 and h.get("reserve") == 8 and h.get("lease", 0) > 0 and
             p.get("completed", 0) > previous and p["completed"] % 256 == 0, "healthy owner advances at unchanged epoch")
        previous = p["completed"]
    need(position(healthy[1]) < position(reset) < position(healthy[2]) and
         position(healthy[3]) < position(snap["rearmed"]) < position(healthy[4]) and
         position(snap["closed"]) < position(healthy[5]) < position(clears[3]) < position(healthy[6]), "healthy work straddles ioctl, REARM and actual close/open")
    work = rows("OWNER_WORKLOAD")
    for pid, expected in ((a, [4096, 8192]), (reopened, [4096])):
        mine = [w for w in work if w.get("pid") == pid]
        need([w.get("completed") for w in mine] == expected, "A workload before/after reset and reopen")
        for w in mine: need(w.get("sq_tail") == w.get("cq_head") == 4096, "fresh ring indices start from zero")
    bw = [w for w in work if w.get("pid") == b]
    need(len(bw) >= 16 and [w.get("completed") for w in bw] == list(range(256, 256*(len(bw)+1), 256)), "continuous bounded B batches")
    for w in work:
        pid, completed = w["pid"], w["completed"]
        need(pid in pids and w.get("entries") == 16 and w.get("fixed_crc") == 0xcbf43926 and
             0 < w.get("irq_wakes", 0) <= w.get("irq_count", 0) < completed+1 and
             w.get("payload_crc") == buffers(completed, w["sq_tail"], 1, 49 if pid == b else 0)[2], "independent payload CRC and real interrupts")
        if pid == b: need(w.get("sq_tail") == w.get("cq_head") == completed, "B never resets its indices")
    guards = rows("OWNER_HELD_CHECK")
    ag = [g for g in guards if g.get("pid") == a]
    need(len(ag) == 9 and len(guards) == 9 + 3*len(progress), "old and freshly configured A buffers plus healthy progress digests")
    for i, g in enumerate(ag):
        fresh = i >= 6
        need(g.get("kind") == i % 3 and g.get("completed") == 4096 and g.get("bytewise_unchanged") is True and
             g.get("bytes") == (131072 if i % 3 == 2 else 4096) and
             g.get("crc") == buffers(4096, 4096, old_epoch, 0, fresh)[i % 3], "independent old/new A guard digest")
    need(position(ag[5]) < position(snap["fresh_config"]) < position(ag[6]) < position(snap["rearmed"]), "guard observations before and after CONFIG with gate still closed")
    for p in progress:
        gg = [g for g in guards if g.get("pid") == b and g.get("completed") == p["completed"]]
        need(len(gg) == 3 and [g.get("kind") for g in gg] == [0,1,2] and
             all(g.get("bytewise_unchanged") is True and g.get("crc") == buffers(p["completed"],p["completed"],p["epoch"],49)[g["kind"]] for g in gg), "independent healthy SQ/CQ/payload digest")
    for pid, sets in ((a,2),(b,1),(reopened,1)):
        mapped = [m for m in rows("OWNER_MAP") if m.get("pid") == pid]
        unmapped = [m for m in rows("OWNER_UNMAP") if m.get("pid") == pid]
        exits = [r for r in rows("OWNER_EXIT") if r.get("pid") == pid]
        reaped = [r for r in rows("M3_OWNER_REAP") if r.get("pid") == pid]
        need(len(mapped) == len(unmapped) == 3*sets and len(exits) == len(reaped) == 1, "exact map ownership and actual process close")
        for n in range(sets):
            for k in range(3):
                m, u = mapped[3*n+k], unmapped[3*n+2-k]
                need(m.get("kind") == u.get("kind") == k and m.get("iova") == [0x100000,0x200000,0x400000][k] and
                     m.get("flags") == [1,2,3][k] and m.get("size") == u.get("requested") == u.get("returned") == [4096,4096,131072][k] and
                     position(m) < position(u), "ordered exact TYPE1 map/unmap")
        if sets == 2: need(position(reset) < position(unmapped[0]) < position(mapped[3]) < position(snap["fresh_config"]), "old owner unmaps after FLR before new maps")
        e, r = exits[0], reaped[0]
        need(e.get("baseline_fds",0) >= 3 and e.get("final_fds") == e["baseline_fds"] and
             all(e.get(k) == 0 for k in ("fds","maps","eventfds","buffers")) and r.get("pidfd") is True and r.get("exit_code") == 0 and
             position(unmapped[-1]) < position(e) < position(r), "real close before successful pidfd reap")
    controls = rows("M3_CONTROL")
    ac = [c for c in controls if c.get("vf") == 0]
    bc = [c for c in controls if c.get("vf") == 1]
    need([c.get("command") for c in ac] == [3,7,7,7,5,4,3,7,5,4] and [c.get("command") for c in bc] == [3,7,5,4], "lease and reset control sequence")
    need([c.get("errno") for c in ac] == [0,0,16,0,0,0,0,0,0,0] and all(c.get("errno") == 0 for c in bc), "only premature REARM is rejected")
    for pid, sequence in ((a,ac[:6]),(reopened,ac[6:]),(b,bc)):
        unmap = [u for u in rows("OWNER_UNMAP") if u.get("pid") == pid][-3]
        reap = next(r for r in rows("M3_OWNER_REAP") if r["pid"] == pid)
        need(position(sequence[-2]) < position(unmap) < position(reap) < position(sequence[-1]) and
             sequence[-2].get("flags",0) & 1 and sequence[-2].get("outstanding") == 0, "final fence, exact owner cleanup, reap then lease release")
    flrs = [(int(v),int(e),int(s)) for v,e,s in re.findall(r"TOY_FLR vf=(\d+) epoch=(\d+) status=(\d+)", device)]
    need([e for v,e,s in flrs if v == 0] == [old_epoch,new_epoch,snap["closed"]["epoch"],clears[3]["epoch"],snap["reclosed"]["epoch"]] and
         len([r for r in flrs if r[0] == 1]) == 2 and len(flrs) == 7 and all(s == 0 for v,e,s in flrs), "actual device FLR callback for every open/ioctl/close")
    traces = [(int(v),int(r,16),role,int(e)) for v,r,role,e in re.findall(r"TOY_DMA_OK vf=(\d+) rid=([0-9a-f]+) role=(\w+) addr=[0-9a-f]+ length=\d+ write=\d+ epoch=(\d+)", device)]
    for vf, epochs in ((0,[old_epoch,new_epoch,clears[3]["epoch"]]),(1,[clears[1]["epoch"]])):
        for epoch in epochs:
            ts = [t for t in traces if t[0] == vf and t[3] == epoch]
            need(len(ts) == 5 and all(t[1] == 0x101+vf for t in ts) and
                 {t[2] for t in ts} == {"sq","payload_read","payload_write","cq","cq_phase"}, "actual requester and epoch for SQ/payload/CQ")
    need(len(traces) == 20, "no unexpected DMA generation")
    final = rows("T06_FINAL")
    need(len(final) == 8 and {r.get("vf") for r in final} == set(range(8)) and
         all(r.get("flags") == 1 and all(r.get(k) == 0 for k in ("state","reserve","outstanding","lease")) for r in final), "all slots FREE and empty ledger")
    return {"status":"PASS", "scope":"T06_only", "healthy_completions":bw[-1]["completed"],
            "reset_owner_completions":12288, "exact_owner_unmaps":12,
            "evidence":{"reset-generations.json":snapshots+clears, "healthy-progress.json":healthy+progress,
                        "guard-digests.json":guards}}
