"""Semantic acceptance for reset with retained work and late completion callbacks."""
import re
import struct
import zlib
from validate_t06 import buffers


def validate_recovery(cases, events, device, scenario, controller):
    def need(ok, reason):
        if not ok: raise RuntimeError(scenario + " semantic validation: " + reason)
    rows = lambda name: [r for r in events if r.get("event") == name]
    positions = {id(r):i for i,r in enumerate(events)}
    pos = lambda r: positions[id(r)]
    callback_count = 2 if scenario == "T08" else 0
    case = cases.get(scenario,{})
    need(case.get("status") == "PASS" and case.get("pending_at_reset") == 1 and case.get("callbacks") == callback_count and
         case.get("all_free") is True and case.get("owner_closed") is True, "case, retained work and cleanup")
    need(not re.search(r"TOY_CORE event=vf_transition vf=\d+ value=6\b|TOY_DMA_ERROR|TOY_FAULT vf=",device), "no unexpected failure or DMA fault")
    peers, groups, opened = rows("M3_PEER_VERIFIED"), rows(scenario+"_GROUP"), rows("vfio_open")
    need(len(peers) == len(groups) == len(opened) == 2 and [p.get("vf") for p in peers] == [0,1] and
         peers[0].get("pid") != peers[1].get("pid") and all(p.get("pidfd") is True for p in peers) and
         groups[0].get("group") != groups[1].get("group") and all(g.get("members") == 1 for g in groups), "two independent owners/domains")
    a,b = [p["pid"] for p in peers]
    clears = rows("RECOVERY_CLEAR")
    need(len(clears) == 3 and [r.get("pid") for r in clears] == [a,b,a] and
         [r.get("cause") for r in clears] == ["open","open","PF_RESET"] and
         all(r.get("zero_registers") == 16 and r.get("flags",0)&1 and not r["flags"]&(2|16|32) for r in clears), "actual FLR and PF reset clear queue readbacks")
    old_epoch,new_epoch = clears[0]["epoch"],clears[2]["epoch"]
    need(new_epoch > old_epoch and len({r["session"] for r in clears}) == 1, "same session, newer recovery epoch")
    pending,cancelled,loaded = rows("RECOVERY_PENDING"),rows("RECOVERY_CANCELLED"),rows("RECOVERY_LOADED")
    need(len(pending) == len(cancelled) == len(loaded) == 1, "one actual retained request")
    expected_desc = struct.pack("<QIBBHQQ",0x400000,8,1,0,0x708,old_epoch,0x79)
    p,c,l = pending[0],cancelled[0],loaded[0]
    need(p.get("pid") == c.get("pid") == a and p.get("epoch") == c.get("old_epoch") == old_epoch and
         c.get("new_epoch") == new_epoch and p.get("sq_head") == 4097 and p.get("cq_tail") == p.get("completed") == 4096 and
         p.get("descriptor_hex") == expected_desc.hex() and p.get("payload_unchanged") is True and p.get("cq_unchanged") is True and
         c.get("requests_marked_error") == 1 and c.get("old_maps_retained") is True and l.get("outstanding") == 1 and
         l.get("state") == 2 and l.get("flags") == 28 and l.get("epoch") == old_epoch, "real outstanding work cancelled without pretending it completed")
    stops,stopped = rows("RECOVERY_STOP_REQUEST"),rows("RECOVERY_STOPPED")
    need(len(stops) == len(stopped) == 1 and stops[0].get("epoch") == old_epoch and stopped[0].get("pid") == a and
         stopped[0].get("pending_requests") == 1 and stopped[0].get("submissions_stopped") is True and
         pos(p) < pos(l) < pos(stops[0]) < pos(stopped[0]) < pos(c), "explicit STOP_REQUEST/STOPPED before reset notice cleanup")
    snapshots = rows(scenario+"_SNAPSHOT")
    need([s.get("phase") for s in snapshots] == ["recovering","slow_ready","fresh_config","rearmed"], "recovery/config/rearm stages")
    for index,s in enumerate(snapshots):
        need(s.get("state") == (2 if index==3 else 5) and s.get("epoch") == new_epoch and
             s.get("reserve") == 8 and s.get("outstanding") == 0 and s.get("lease",0)>0 and
             bool(s.get("flags",0)&1) == (index!=3) and bool(s["flags"]&16) == (index>=2) and not s["flags"]&2,
             "RECOVERING remains fenced, retains quota, clears outstanding")
    need(snapshots[0]["ready_gen"] == snapshots[1]["ready_gen"] and
         snapshots[2]["ready_gen"] == snapshots[3]["ready_gen"] == snapshots[0]["ready_gen"]+1, "fresh READY generation")
    records = controller.records
    need(not controller.armed and all(r.get("status") == "PASS" for r in records), "all QMP barriers acknowledged")
    settings = [r for r in records if r["request"]["action"] == "qmp-set"]
    expected_settings = [("fault-vf",0),("fault-after",0),("delay-cpl-ticks",60000),("delay-cpl-ticks",0)] + [("try-stale-cpl",True)]*callback_count
    need([(r["request"].get("property"),r["request"].get("value")) for r in settings] == expected_settings and
         all(r.get("ack",{}).get("property") == r["request"]["property"] and r["ack"].get("value") == r["request"]["value"] and
             r["ack"].get("ack",0)>0 for r in settings), "actual acknowledged fault setup, disable and callback calls")
    retained = [r for r in records if r["request"]["action"] == "pending-check"]
    need([r.get("observation",{}).get("pending") for r in retained] == [1,0], "device actually retained then cancelled completion")
    delays = [r for r in records if r["request"]["action"] == "virtual-delay"]
    need([r["request"].get("ticks") for r in delays] == ([200] if callback_count else [200,50]), "bounded delayed READY observation")
    for r in delays:
        ob=r.get("observation",{})
        need(ob.get("requested_ticks") == r["request"]["ticks"] and
             ob["requested_ticks"] <= ob.get("last_tick",0)-ob.get("first_tick",0) < 1000, "delay measured in actual device virtual ticks")
    trace = re.findall(r"TOY_PENDING vf=(\d+) epoch=(\d+) token=(\d+) cq_index=(\d+) due=(\d+)",device)
    need(len(trace)==1 and tuple(map(int,trace[0][:2]))==(0,old_epoch) and int(trace[0][3])==4096, "actual saved descriptor/cancel token/CQ reservation")
    callbacks = [tuple(map(int,v)) for v in re.findall(r"TOY_STALE_CPL vf=(\d+) old_session=(\d+) live_session=(\d+) old_epoch=(\d+) live_epoch=(\d+) old_token=(\d+) live_token=(\d+) count=(\d+)",device)]
    need(len(callbacks) == callback_count and len(re.findall(r"TOY_FAULT_ACK property=try-stale-cpl valid=1 ack=\d+ consumed=1",device)) == callback_count, "real valid retired callback invocation")
    for index,v in enumerate(callbacks,1):
        need(v[0]==0 and v[1]==v[2]==clears[0]["session"] and v[3]==old_epoch and v[4]==new_epoch and
             v[5]==int(trace[0][2]) and v[6]>v[5] and v[7]==index, "stale completion drop before DMA by old token")
    counters=rows("RECOVERY_COUNTERS")
    need([r.get("phase") for r in counters] == (["reset","callback_fenced","callback_rearmed","finished"] if callback_count else ["reset","finished"]) and
         [r.get("stale_cpl") for r in counters] == ([0,1,2,2] if callback_count else [0,0]) and
         all(r.get("vf")==0 and all(r.get(k)==0 for k in ("stale_desc","dma_fault","irq_suppressed")) for r in counters), "STALE_CPL independent of STALE_DESC and DMA fault")
    progress,healthy = rows("RECOVERY_HEALTHY_PROGRESS"),rows(scenario+"_HEALTHY_CHECK")
    need(len(progress)==len(healthy)==4 and [r.get("phase") for r in healthy]==["pending","slow_ready","configured_fenced","fresh_io"], "healthy VF spans load/reset/ready/resume")
    previous=0
    for pr,h in zip(progress,healthy):
        need(pr.get("pid")==h.get("pid")==b and pr.get("epoch")==h.get("epoch")==clears[1]["epoch"] and
             pr.get("flags")==h.get("flags")==28 and h.get("state")==2 and pr.get("completed",0)>previous,
             "healthy VF continues in same epoch")
        previous=pr["completed"]
    need(pos(healthy[0]) < pos(c) < pos(healthy[1]) < pos(snapshots[2]) < pos(healthy[2]) < pos(snapshots[3]) < pos(healthy[3]), "healthy work during recovery fence and after resume")
    work=rows("OWNER_WORKLOAD")
    aw=[w for w in work if w.get("pid")==a]; bw=[w for w in work if w.get("pid")==b]
    need([w.get("completed") for w in aw]==[4096,8192] and all(w.get("sq_tail")==w.get("cq_head")==4096 for w in aw), "valid A I/O before and after new rings")
    need(len(bw)>=16 and [w.get("completed") for w in bw]==list(range(256,256*(len(bw)+1),256)), "continuous checked B workload")
    for w in work:
        need(w.get("pid") in (a,b) and w.get("entries")==16 and w.get("fixed_crc")==0xcbf43926 and
             0<w.get("irq_wakes",0)<=w.get("irq_count",0)<=w["completed"] and
             w.get("payload_crc")==buffers(w["completed"],w["sq_tail"],1,49 if w["pid"]==b else 0)[2], "independent payload/CRC results and eventfds")
    old_sq=bytearray(4096)
    for i in range(16):
        n=4080+i; length=(1,9,4095,4096)[(n//2)%4]
        struct.pack_into("<QIBBHQQ",old_sq,i*32,0x400000+i*8192,length,2 if n&1 else 1,0,n,old_epoch,n&255)
    old_sq[:32]=expected_desc
    old= buffers(4096,4096,old_epoch,0); old[0]=zlib.crc32(old_sq)
    fresh=buffers(4096,4096,new_epoch,0,True); finished=buffers(8192,4096,new_epoch,0)
    guards=rows("OWNER_HELD_CHECK"); ag=[g for g in guards if g.get("pid")==a]
    expected=old*3+fresh*(2 if callback_count else 1)+finished
    need(len(ag)==len(expected) and all(g.get("kind")==i%3 and g.get("crc")==expected[i] and
         g.get("bytewise_unchanged") is True for i,g in enumerate(ag)), "independent old, fresh and completed ring guard digests")
    retired=rows("OWNER_RETIRED_CHECK")
    need(len(retired)==(15 if callback_count else 12) and all(g.get("pid")==a and g.get("kind")==i%3 and
         g.get("mapped") is False and g.get("bytewise_unchanged") is True and g.get("crc")==old[i%3] for i,g in enumerate(retired)), "old CPU buffers remain allocated and unchanged after UNMAP, callback and new I/O")
    for pr in progress:
        gg=[g for g in guards if g.get("pid")==b and g.get("completed")==pr["completed"]]
        need(len(gg)==3 and all(g.get("kind")==i and g.get("crc")==buffers(pr["completed"],pr["completed"],pr["epoch"],49)[i] and
             g.get("bytewise_unchanged") is True for i,g in enumerate(gg)), "independent healthy ring and payload snapshots")
    quiet=rows("RECOVERY_NO_IRQ")
    need(len(quiet)==len(guards)//3 and all(r.get("eventfd_errno")==11 for r in quiet), "real eventfd EAGAIN while guard buffers are held")
    maps,unmaps=rows("OWNER_MAP"),rows("OWNER_UNMAP")
    need(len(maps)==len(unmaps)==9 and len(rows("OWNER_RETIRED"))==3, "old/new A and independent B mappings")
    controls=rows("M3_CONTROL")
    for pid,sets,vf in ((a,2,0),(b,1,1)):
        mm=[m for m in maps if m.get("pid")==pid]; uu=[u for u in unmaps if u.get("pid")==pid]
        need(len(mm)==len(uu)==sets*3, "mapping ownership counts")
        for n in range(sets):
            for k in range(3):
                m,u=mm[n*3+k],uu[n*3+2-k]
                need(m.get("kind")==u.get("kind")==k and m.get("flags")==[1,2,3][k] and
                     m.get("iova")==[0x100000,0x200000,0x400000][k] and
                     m.get("size")==u.get("requested")==u.get("returned")==[4096,4096,131072][k] and pos(m)<pos(u), "exact owner unmap extents and permissions")
        if sets==2:
            need(pos(c)<pos(uu[0])<pos(mm[3])<pos(snapshots[2]) and all(mm[k]["vaddr"]!=mm[k+3]["vaddr"] for k in range(3)), "old mappings revoked before genuinely new allocations")
        cc=[r for r in controls if r.get("vf")==vf]
        need([r.get("command") for r in cc]==([3,7,5,6,7,5,4] if vf==0 else [3,7,5,4]) and all(r.get("errno")==r.get("fw_status")==0 for r in cc), "driver acquire/rearm/fence/reset/rearm/final cleanup")
        ee=[r for r in rows("OWNER_EXIT") if r.get("pid")==pid]; rr=[r for r in rows("M3_OWNER_REAP") if r.get("pid")==pid]
        need(len(ee)==len(rr)==1 and ee[0].get("baseline_fds",0)>=3 and ee[0].get("final_fds")==ee[0]["baseline_fds"] and
             all(ee[0].get(k)==0 for k in ("fds","maps","eventfds","buffers")) and rr[0].get("exit_code")==0 and rr[0].get("pidfd") is True and
             pos(cc[-2])<pos(uu[-3])<pos(ee[0])<pos(rr[0])<pos(cc[-1]), "fence before final owner close/reap/release")
    flrs=re.findall(r"TOY_FLR vf=(\d+) epoch=(\d+) status=(\d+)",device)
    need(len(flrs)==4 and all(int(v)<2 and int(status)==0 for v,ep,status in flrs), "PF reset is not mislabeled as an extra PCI FLR")
    dma=re.findall(r"TOY_DMA_OK vf=(\d+) rid=([0-9a-f]+) role=(\w+) addr=[0-9a-f]+ length=\d+ write=\d+ epoch=(\d+)",device)
    need(len(dma)==15 and all(int(rid,16)==0x101+int(v) for v,rid,role,ep in dma), "all actual DMA uses its VF requester")
    for vf,ep in ((0,old_epoch),(0,new_epoch),(1,clears[1]["epoch"])):
        need({r[2] for r in dma if int(r[0])==vf and int(r[3])==ep}=={"sq","payload_read","payload_write","cq","cq_phase"}, "verified DMA roles at old/new generations")
    final=rows("RECOVERY_FINAL")
    need(len(final)==8 and {r.get("vf") for r in final}==set(range(8)) and all(r.get("flags")==1 and
         all(r.get(k)==0 for k in ("state","reserve","outstanding","lease")) for r in final), "all FREE, no retained quota or lease")
    return {"status":"PASS","scope":scenario+"_only","healthy_completions":bw[-1]["completed"],"recovered_completions":8192,
            "cancelled_requests":1,"stale_callbacks":callback_count,"evidence":{"reset-generations.json":snapshots+clears,
            "guard-digests.json":guards+retired,"delayed-callbacks.json":counters,"healthy-progress.json":healthy+progress,
            "reset-handshake.json":[r for r in events if r.get("event","").startswith("RECOVERY_") or r.get("event")=="M3_CONTROL"],
            "vfio-unmap-results.json":unmaps,"callback-timeline.json":{"callbacks":[list(row) for row in callbacks],"counters":counters,
            "barriers":records,"ordering_scope":"Each raw stream retains its own order; serial phase checkpoints bracket QMP barriers."}}}
