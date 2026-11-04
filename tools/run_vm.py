#!/usr/bin/env python3
"""Run one offline TCG guest and validate structured case results plus debug exit."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import tempfile
import time
from lab_common import ROOT, artifact_dir, provenance, resource_lock, sha256, write_json
from qmp import QMP
from vm_runtime import monitor_guest

REQUIRED = {"m1": {"M1_CAPS", "M1_GROUPS", "M1_TYPE1", "T01"}, "m3-case": {"T03"}}
M3_CASE_IDS = {"T01": "T01", "T03": "T03", "T04": "T04", "T09": "T09", "T02-provision": "T02_PROVISION", "T02-owner": "T02_OWNER", "T02-pci": "T02_PCI", "T10": "T10"}
M3_CASE_IDS["T05"] = "T05"
M3_CASE_IDS["T06"] = "T06"
M3_CASE_IDS.update(T07="T07", T08="T08")
KERNEL_REPORT = re.compile(r"BUG:|WARNING:|KASAN:|KCSAN:|Kernel panic|general protection fault|possible circular locking dependency|INFO: task .* blocked")


# Guest 的 PASS 字串只是一項輸入；還要核對實際 capabilities、groups、mapping 與回收紀錄。
def validate_m1_payload(cases, events):
    """Cross-check quantitative results against every enumerated group and operation."""
    def need(condition, reason):
        if not condition:
            raise RuntimeError("M1 semantic validation failed: " + reason)

    def entries(kind):
        return [event for event in events if event.get("event") == kind]

    def exact_fields(item, expected, label):
        for name, value in expected.items():
            need(item.get(name) == value, label + "." + name)

    need(REQUIRED["m1"] <= cases.keys(), "required IDs")
    need(all(cases[name].get("status") == "PASS" for name in REQUIRED["m1"]), "required statuses")
    exact_fields(cases["M1_CAPS"], {"pf_vectors": 2, "vf_vectors": 2, "vfs": 4,
                 "unique_requesters": True, "flr_declared": True}, "M1_CAPS")
    exact_fields(cases["T01"], {"cycles": 100, "vfs_per_cycle": 4, "final_ledger": 0,
                 "remaining_vf_functions": 0, "remaining_vfio_fds": 0,
                 "remaining_vfio_maps": 0, "fd_growth": 0}, "T01")
    exact_fields(cases["M1_TYPE1"], {"viable_groups": 4, "simultaneous_containers": 4,
                 "maps": 4, "exact_unmaps": 4, "remaining_fds": 0, "remaining_maps": 0,
                 "vfio_reset_epoch_checked": True}, "M1_TYPE1")
    group_case = cases["M1_GROUPS"]
    groups, pf = group_case.get("groups"), group_case.get("pf")
    bdf = re.compile(r"^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$")
    need(isinstance(pf, str) and bdf.fullmatch(pf), "PF BDF")
    need(isinstance(groups, list) and len(groups) == 4 and all(isinstance(g, dict) for g in groups), "four group records")
    vf_names, group_ids = [], []
    for group in groups:
        vf, gid, members = group.get("vf"), group.get("group_id"), group.get("members")
        need(isinstance(vf, str) and bdf.fullmatch(vf) and vf != pf, "VF BDF")
        need(type(gid) is int and gid >= 0, "numeric group ID")
        need(isinstance(members, list) and members and all(isinstance(m, str) and bdf.fullmatch(m) for m in members), "complete group member list")
        need(len(members) == len(set(members)) and vf in members and pf not in members, "group ownership")
        observed = [e for e in entries("group_member") if e.get("group") == gid]
        need(len(observed) == len(members) and {e.get("bdf") for e in observed} == set(members), "member evidence matches snapshot")
        for member in observed:
            if member["bdf"] != vf:
                need(member.get("driver") == "none" and type(member.get("class")) is int and member["class"] >> 8 == 0x0604, "additional member must be driverless bridge")
        vf_names.append(vf)
        group_ids.append(gid)
    need(len(set(vf_names)) == len(set(group_ids)) == 4, "four independent VF groups")
    caps = entries("function_caps")
    need(len(caps) == 5 and {e.get("bdf") for e in caps} == {pf, *vf_names},
         "initial PF and all four VF capability observations")
    for cap in caps:
        is_pf = cap["bdf"] == pf
        exact_fields(cap, {"kind": "PF" if is_pf else "VF", "vectors": 2,
                           "enumerated_vendor": 0x1234, "enumerated_device": 0xe001 if is_pf else 0xe002},
                     "function_caps " + cap["bdf"])
        for field, low, high in (("msix_offset", 0x40, 0xf4), ("pcie_offset", 0x40, 0xc4),
                                 ("ari_offset", 0x100, 0xff8), ("acs_offset", 0x100, 0xff8)):
            offset = cap.get(field)
            need(type(offset) is int and low <= offset <= high and offset % 4 == 0,
                 cap["bdf"] + "." + field + " bounds/alignment")
        need(cap["msix_offset"] != cap["pcie_offset"] and cap["ari_offset"] != cap["acs_offset"],
             "distinct standard and extended capabilities")
        if is_pf:
            sriov = cap.get("sriov_offset")
            need(type(sriov) is int and 0x100 <= sriov <= 0xfc0 and sriov % 4 == 0,
                 "PF SR-IOV capability bounds/alignment")
            intervals = sorted(((cap["ari_offset"], 8), (cap["acs_offset"], 8), (sriov, 0x40)))
            need(all(start + size <= next_start for (start, size), (next_start, _) in zip(intervals, intervals[1:])),
                 "non-overlapping PF extended capabilities")
            exact_fields(cap, {"raw_vendor": 0x1234, "raw_device": 0xe001}, "PF raw identity")
        else:
            need(cap.get("flr") is True and "sriov_offset" not in cap, "VF FLR and no PF-only SR-IOV field")
    sriov_caps = entries("sriov_capability")
    need(len(sriov_caps) == 1, "one initial PF SR-IOV observation")
    pf_caps = next(e for e in caps if e["bdf"] == pf)
    sriov_cap = sriov_caps[0]
    exact_fields(sriov_cap, {"pf": pf, "offset": pf_caps["sriov_offset"], "total_vfs": 8, "initial_vfs": 8},
                 "initial SR-IOV capability")
    for field in ("first_vf_offset", "stride", "supported_page_sizes", "system_page_size"):
        need(type(sriov_cap.get(field)) is int and sriov_cap[field] > 0, "SR-IOV " + field)
    for group in groups:
        need(set(group["members"]) & set(vf_names) == {group["vf"]}, "no other VF in group")
    links = entries("vf_link")
    need(len(links) == 4 and {e.get("vf") for e in links} == set(vf_names), "four physfn/virtfn observations")
    need(all(e.get("physfn") == pf for e in links) and len({e.get("rid") for e in links}) == 4, "unique requester evidence")
    opens = entries("vfio_open")
    need(len(opens) == 4 and {e.get("vf") for e in opens} == set(vf_names), "four actual VFIO opens")
    need(all(type(e.get("container_fd")) is int and e["container_fd"] >= 0 for e in opens) and
         len({e["container_fd"] for e in opens}) == 4, "separate simultaneous container FDs")
    lookup = {g["vf"]: g["group_id"] for g in groups}
    need(all(e.get("group") == lookup[e["vf"]] and e.get("vectors") == 2 and e.get("reset_supported") is True for e in opens), "VFIO group and capabilities")
    maps, unmaps = entries("vfio_map"), entries("vfio_unmap")
    need(len(maps) == len(unmaps) == 4 and {e.get("vf") for e in maps} == set(vf_names) and
         {e.get("vf") for e in unmaps} == set(vf_names), "mapping ownership records")
    for mapped in maps:
        size = mapped.get("size")
        need(type(size) is int and size > 0 and not size & (size - 1), "page-sized mapping")
        need(mapped.get("permissions") == "READ_WRITE", "explicit map direction")
        need(next(e for e in unmaps if e["vf"] == mapped["vf"]).get("size") == size, "exact unmap size")
    resets = entries("vfio_reset_observed")
    need(len(resets) == 4 and {e.get("vf") for e in resets} == set(vf_names), "reset snapshots for all VFs")
    need(sum(e.get("target") is True for e in resets) == 1, "one targeted FLR")
    for reset in resets:
        before, after, flags = reset.get("before_epoch"), reset.get("after_epoch"), reset.get("hw_flags")
        need(type(before) is int and type(after) is int and before > 0, "reset epoch values")
        need(after > before if reset.get("target") is True else after == before, "target epoch advance and other VF stability")
        need(type(flags) is int and flags & 1 and not flags & 16, "reset fence and invalid queue")
    enabled, disabled = entries("enable_snapshot"), entries("disable_snapshot")
    need(len(enabled) == len(disabled) == 100, "one hundred enable and disable observations")
    need([e.get("cycle") for e in enabled] == list(range(1, 101)) and
         [e.get("cycle") for e in disabled] == list(range(1, 101)), "continuous cycle IDs")
    need(all(e.get("enabled") == 4 and e.get("ledger_credit") == 32 for e in enabled), "every enabled ledger")
    need(all(e.get("enabled") == 0 and e.get("ledger_credit") == 0 and e.get("outstanding") == 0 and e.get("vf_links") == 0 for e in disabled), "every disabled ledger and cleanup")
    return {"status": "PASS", "groups_checked": 4, "containers_checked": 4,
            "exact_unmaps_checked": 4, "enable_disable_cycles_checked": 100,
            "function_capabilities_checked": 5, "ari_capabilities_checked": 5, "acs_capabilities_checked": 5}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gate", choices=REQUIRED, default="m1")
    parser.add_argument("--profile", choices=("stress", "kasan"), default="stress")
    parser.add_argument("--accel", choices=("tcg",), default="tcg")
    parser.add_argument("--seed", type=int, default=101)
    parser.add_argument("--timeout", type=int)
    parser.add_argument("--case", choices=M3_CASE_IDS, default="T03")
    parser.add_argument("--entries", type=int, choices=(16,32,256), default=16)
    parser.add_argument("--dma-fault", type=int, choices=range(7), default=0)
    args = parser.parse_args()
    out = artifact_dir(args.gate)
    result = {"schema_version": 1, "gate": args.gate, "status": "FAIL", **provenance(),
              "accelerator": args.accel, "profile": args.profile, "seed": args.seed,
              "cases": [], "unexpected_kernel_reports": [], "limitations": []}
    result["controller_inputs"] = {name: sha256(ROOT / "tools" / name)
        for name in ("run_vm.py", "vm_runtime.py", "qmp.py", "validate_m3.py", "validate_t06.py", "validate_recovery.py", "m3_controller.py")}
    (out / "controller-source").mkdir()
    for name in result["controller_inputs"]:
        shutil.copy2(ROOT / "tools" / name, out / "controller-source" / name)
    binary = ROOT / "build/qemu/qemu-system-x86_64"
    kernel = ROOT / f"build/kernel-{args.profile}/arch/x86/boot/bzImage"
    initrd = ROOT / f"build/initramfs-{args.profile}/initramfs.cpio.gz"
    process = None
    qmp = None
    event_handler = None
    start = time.monotonic()
    try:
        for path in (binary, kernel, initrd):
            if not path.is_file():
                result["status"] = "BLOCKED"
                raise RuntimeError("Required build artifact missing: " + str(path.relative_to(ROOT)))
        result["artifact_hashes"] = {str(path.relative_to(ROOT)): sha256(path) for path in (binary, kernel, initrd)}
        if args.gate == "m3-case":
            from run_uefi import require_guest_builds
            from validate_m3 import validate_t01, validate_t03, validate_t04, validate_t09, validate_t02_provision, validate_t02_owner, validate_t02_pci, validate_t10
            from validate_m3 import validate_t05
            from validate_t06 import validate_t06
            from validate_recovery import validate_recovery
            from m3_controller import M3Controller
            event_handler = M3Controller(out, args.case)
            if args.profile != "kasan" or args.accel != "tcg":
                raise RuntimeError("M3 development cases require the mandatory KASAN/TCG profile")
            result["scope"] = "individual_case_development_run_not_m3_gate"
            result["builds"] = require_guest_builds(binary, kernel, initrd, args.profile)
        with resource_lock(), tempfile.TemporaryDirectory(prefix="sriov-") as temporary:
            run = Path(temporary)
            run.chmod(0o700)
            socket = run / "qmp.sock"
            serial_socket = run / "serial.sock"
            # UART0 收 kernel 診斷，UART1 傳 JSON 與 ACK，避免 printk 插入半行協定資料。
            uart_args = ["-serial", f"file:{out / 'kernel.log'}"]
            uart_args += ["-chardev", f"socket,id=uart,path={serial_socket},server=on,wait=off", "-serial", "chardev:uart"]
            argv = [str(binary), "-machine", "q35",
                    "-accel", "tcg,thread=multi",
                    "-cpu", "max" if args.accel == "tcg" else "host", "-m", "2048M" if args.profile == "kasan" else "1536M",
                    "-smp", "2", "-display", "none", "-monitor", "none", "-S",
                    *uart_args,
                    "-nic", "none", "-no-reboot", "-device", "intel-iommu,intremap=on",
                    "-device", "pcie-root-port,id=rp1,chassis=1,slot=1", "-device", "toy-sriov-pf,bus=rp1,id=toy,total-vfs=8",
                    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-qmp", f"unix:{socket},server=on,wait=off",
                    "-kernel", str(kernel), "-initrd", str(initrd), "-append",
                    f"console=ttyS0 rdinit=/init intel_iommu=on iommu.strict=1 toy_iommu_strict_wo=1 panic=-1 lab_gate={args.gate} lab_seed={args.seed} lab_case={args.case} lab_entries={args.entries} lab_dma_fault={args.dma_fault}"]
            write_json(out / "command.json", {"argv": argv, "cwd": str(ROOT)})
            with (out / "qemu.log").open("wb") as log:
                process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                                           env={**os.environ}, umask=0o077)
                result["qemu_pid"] = process.pid
                qmp = QMP(socket, out / "qmp.jsonl")
                socket.chmod(0o600)
                result["qom_path"] = qmp.toy_path
                result["fault_properties"] = qmp.properties
                timeout = args.timeout or (900 if args.profile == "kasan" else 300)
                try:
                    result.update(monitor_guest(process, qmp, serial_socket, out, timeout, event_handler))
                    if event_handler is not None:
                        result["controller_barriers"] = event_handler.finish()
                except subprocess.TimeoutExpired:
                    result["timed_out"] = True
                    raise RuntimeError("Guest exceeded configured deadline")
            text = (out / "serial.log").read_text(errors="replace")
            kernel_text = (out / "kernel.log").read_text(errors="replace")
            if "Linux version " not in kernel_text:
                raise RuntimeError("Separate kernel UART evidence missing")
            result["unexpected_kernel_reports"] += [line for line in kernel_text.splitlines() if KERNEL_REPORT.search(line)]
            cases = {}
            events = []
            for line in text.splitlines():
                if KERNEL_REPORT.search(line): result["unexpected_kernel_reports"].append(line)
                try: item = json.loads(line)
                except json.JSONDecodeError: continue
                if isinstance(item, dict) and "event" in item:
                    events.append(item)
                if isinstance(item, dict) and "id" in item and "status" in item:
                    if item["id"] in cases: raise RuntimeError("Duplicate case result: " + item["id"])
                    cases[item["id"]] = item
            result["cases"] = list(cases.values())
            required = REQUIRED[args.gate] if args.gate == "m1" else {M3_CASE_IDS[args.case]}
            missing = required - cases.keys()
            if missing: raise RuntimeError("Missing required cases: " + ", ".join(sorted(missing)))
            if any(cases[name]["status"] != "PASS" for name in required):
                raise RuntimeError("Required guest case did not pass")
            if result["exit_code"] != 1 or text.splitlines().count("LAB_RESULT=PASS") != 1 or "LAB_RESULT=FAIL" in text:
                raise RuntimeError("Guest final marker/debug-exit disagreement")
            if result["unexpected_kernel_reports"]:
                raise RuntimeError("Unexpected kernel diagnostics")
            if args.gate == "m1":
                result["semantic_validation"] = validate_m1_payload(cases, events)
                if result["host_checkpoints"]["status"] != "PASS":
                    raise RuntimeError("M1 QEMU object inventory checkpoints missing")
            elif args.gate == "m3-case":
                device_text = (out / "qemu.log").read_text(errors="replace")
                result["semantic_validation"] = (validate_recovery(cases, events, device_text, args.case, event_handler)
                    if args.case in ("T07", "T08") else validate_t06(cases, events, device_text)
                    if args.case == "T06" else validate_t05(cases, events, args.dma_fault, device_text)
                    if args.case == "T05" else validate_t01(cases, events, device_text, event_handler)
                    if args.case == "T01" else validate_t03(cases, events, args.entries, device_text)
                    if args.case == "T03" else validate_t04(cases, events, args.entries, device_text)
                    if args.case == "T04" else validate_t10(cases, events, args.entries, device_text)
                    if args.case == "T10" else validate_t09(cases, events, args.entries, device_text)
                    if args.case == "T09" else validate_t02_owner(cases, events, device_text)
                    if args.case == "T02-owner" else validate_t02_pci(cases, events, kernel_text, device_text, event_handler)
                    if args.case == "T02-pci" else validate_t02_provision(cases, events, kernel_text, device_text, event_handler))
                if args.case in ("T01", "T04", "T05", "T06", "T07", "T08", "T09"):
                    for name, records in result["semantic_validation"]["evidence"].items():
                        write_json(out / name, records)
                if args.case in ("T07", "T08"):
                    (out / "fault-ack.log").write_text("\n".join(line for line in device_text.splitlines() if "TOY_FAULT_ACK" in line)+"\n")
                    result["evidence_aliases"] = {"qmp.log":"qmp.jsonl"}
                if args.case == "T05":
                    if "TOY_STRICT_WO enabled=1" not in kernel_text:
                        raise RuntimeError("T05 requires the explicitly enabled, pinned guest strict-WO hook")
                    (out / "iommu-faults.log").write_text("\n".join(line for line in device_text.splitlines()
                        if "TOY_DMA_ERROR" in line or "TOY_FAULT" in line or "vtd" in line.lower()) + "\n")
            result["status"] = "PASS"
    except (OSError, RuntimeError, ValueError, EOFError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, flush=True)
    finally:
        if qmp is not None: qmp.close()
        if process is not None and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            result["termination_exit_code"] = process.returncode
        result["elapsed_seconds"] = time.monotonic() - start
        result["evidence_hashes"] = {name: sha256(out / name) for name in
            ("command.json", "serial.log", "kernel.log", "qemu.log", "qmp.jsonl", "groups.json",
             "vfio-maps.json", "buffer-digests.json", "dma-requester-evidence.json",
             "lifecycle-counters.json", "vfio-ownership.json", "descriptor-vectors.json", "counter-snapshots.json",
             "vf-rid-map.json", "device-fault-counters.json", "guard-digests.json", "iommu-faults.log",
             "m3-barriers.json", "m3-qom-cold.json", "m3-qom-initial.json", "m3-qom-final.json",
             "reset-generations.json", "delayed-callbacks.json", "healthy-progress.json", "reset-handshake.json",
             "vfio-unmap-results.json", "callback-timeline.json", "fault-ack.log") if (out / name).is_file()}
        write_json(out / "result.json", result)
        write_json(ROOT / f"artifacts/{args.gate}-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
