#!/usr/bin/env python3
"""Run an offline OVMF probe, continuous Linux handoff, or disabled-PCD observation."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time

from build_edk2 import fingerprint as edk2_fingerprint
from build_initramfs import guest_source_hashes
from build_qemu import fingerprint as qemu_fingerprint
from check_config import check as check_kernel_config
from lab_common import ROOT, artifact_dir, provenance, resource_lock, sha256, write_json
from qmp import QMP
from run_vm import KERNEL_REPORT, validate_m1_payload
from vm_runtime import monitor_guest

MODES = ("probe", "handoff", "negative")
M2_IDS = {"M2_CORE", "M2_CAPS", "M2_WINDOWS", "M2_MAILBOX"}


class PrerequisiteError(RuntimeError):
    pass


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_json(path):
    with Path(path).open(encoding="utf-8") as stream:
        return json.load(stream)


def evidence_path(relative):
    path = (ROOT / relative).resolve()
    if not path.is_relative_to((ROOT / "artifacts").resolve()):
        raise RuntimeError("Evidence path escapes artifacts")
    return path


def parse_serial(text):
    cases, events = {}, []
    for line in text.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(item, dict):
            continue
        if "event" in item:
            events.append(item)
        if "id" in item and "status" in item:
            need(item["id"] not in cases, "Duplicate guest case: " + str(item["id"]))
            cases[item["id"]] = item
    return cases, events


# 兩份 UART log 各自保留原始順序；合併文字只用於語意檢查，不代表跨 channel 的全域時序。
def read_uefi_output(directory, mode):
    """Keep raw UARTs separate; join only for semantic checks, never rewrite logs.

    UART0 preserves the firmware-to-kernel ordering. UART1 carries Linux JSON
    and checkpoint ACKs; no wall-clock ordering between channels is implied.
    """
    need(mode in MODES, "Invalid UEFI output mode")
    serial = (directory / "serial.log").read_text(errors="replace")
    if mode == "probe":
        return serial
    boot = (directory / "kernel.log").read_text(errors="replace")
    need("Linux version " in boot, "Separate firmware/kernel UART evidence missing")
    return boot + "\n" + serial


# 僅接受目前 QEMU/kernel/initramfs 的 M1 證據，並重新核對 raw logs 與核心診斷。
def require_m1(binary, kernel, initrd):
    """Require the actual direct-boot result for these three binary artifacts."""
    try:
        latest = read_json(ROOT / "artifacts/m1-latest.json")
        directory = evidence_path(latest["path"])
        report = read_json(directory / "result.json")
        need(latest["status"] == report["status"] == "PASS", "M1 is not PASS")
        need(report["accelerator"] == "tcg", "Mandatory M1 TCG run missing")
        need(report["exit_code"] == 1 and not report["unexpected_kernel_reports"], "M1 exit or diagnostics invalid")
        need(report["host_checkpoints"]["status"] == "PASS", "M1 host checkpoints missing")
        for path in (binary, kernel, initrd):
            need(report["artifact_hashes"][str(path.relative_to(ROOT))] == sha256(path),
                 "M1 artifact changed: " + str(path.relative_to(ROOT)))
        for name in ("serial.log", "kernel.log"):
            need(report["evidence_hashes"][name] == sha256(directory / name), "M1 raw evidence changed: " + name)
        text = (directory / "serial.log").read_text(errors="replace")
        kernel_text = (directory / "kernel.log").read_text(errors="replace")
        need("Linux version " in kernel_text, "M1 separate kernel UART missing")
        need(text.splitlines().count("LAB_RESULT=PASS") == 1 and "LAB_RESULT=FAIL" not in text,
             "M1 raw final marker invalid")
        cases, events = parse_serial(text)
        validate_m1_payload(cases, events)
        validate_checkpoint_events(events)
        need(not any(KERNEL_REPORT.search(line) for line in (kernel_text + "\n" + text).splitlines()), "M1 raw diagnostics changed")
        cold = read_json(directory / "qom-cold.json")
        need(all(value == 0 for value in cold["owned"].values()), "M1 cold checkpoint has device-owned work")
        for side in ("initial", "final"):
            need(report["host_checkpoints"][side + "_sha256"] == sha256(directory / ("qom-" + side + ".json")),
                 "M1 checkpoint evidence changed")
        return {"status": "PASS", "path": str(directory.relative_to(ROOT)),
                "result_sha256": sha256(directory / "result.json"),
                "serial_sha256": sha256(directory / "serial.log"),
                "kernel_sha256": sha256(directory / "kernel.log"),
                "cold_sha256": sha256(directory / "qom-cold.json")}
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        raise PrerequisiteError("Current-binary M1 prerequisite unavailable: " + str(error)) from error


def require_guest_builds(binary, kernel, initrd, profile):
    """Reject changed binaries, mismatched source pins, and stale copied inputs."""
    try:
        lock = read_json(ROOT / "sources.lock.json")
        qemu = read_json(binary.parent / "stamp.json")
        need(qemu["status"] == "PASS" and qemu["binary_sha256"] == sha256(binary), "QEMU build stamp mismatch")
        need(qemu["source_commit"] == lock["sources"]["qemu"]["commit"], "QEMU source pin mismatch")
        inputs = [ROOT / "sources.lock.json", ROOT / "patches/qemu/0001-toy-sriov-build.patch", ROOT / "tools/build_qemu.py"]
        for directory in ("device", "fwcore", "include"):
            inputs.extend(path for path in (ROOT / directory).rglob("*") if path.is_file())
        need(qemu["input_sha256"] == qemu_fingerprint(inputs), "QEMU inputs changed after build")
        kernel_dir = ROOT / f"build/kernel-{profile}"
        kernel_stamp = read_json(kernel_dir / "stamp.json")
        from kernel_source import source_inputs
        need(kernel_stamp.get("source_inputs") == source_inputs(lock) and
             kernel_stamp["builder_sha256"] == sha256(ROOT / "tools/build_kernel.py"), "Kernel patched source/builder changed")
        need(kernel_stamp["status"] == "PASS" and kernel_stamp["scope"] == "kernel_and_module" and
             kernel_stamp["profile"] == profile, "Kernel build/profile not PASS")
        need(kernel_stamp["source_commit"] == lock["sources"]["linux"]["commit"], "Linux source pin mismatch")
        need(kernel_stamp["bzimage_sha256"] == sha256(kernel) and
             kernel_stamp["module_sha256"] == sha256(kernel_dir / "toy-pf/toy_pf.ko"), "Kernel/module binary changed")
        need(kernel_stamp["config"]["config_sha256"] == sha256(kernel_dir / ".config") and
             check_kernel_config(kernel_dir / ".config", profile)["status"] == "PASS", "Kernel configuration mismatch")
        fragments = [ROOT / "guest/configs/base.config", ROOT / f"guest/configs/{profile}.config"]
        need((kernel_dir / "requested.config").read_text() == "\n".join(path.read_text() for path in fragments),
             "Kernel configuration fragments changed")
        for directory, copied in (("guest/pf", "toy-pf"), ("include", "include-lab")):
            for path in (ROOT / directory).rglob("*"):
                if path.is_file():
                    need(sha256(path) == sha256(kernel_dir / copied / path.relative_to(ROOT / directory)),
                         "PF module source changed after build: " + str(path.relative_to(ROOT)))
        archive = read_json(initrd.parent / "stamp.json")
        need(archive["status"] == "PASS" and archive["profile"] == profile and
             archive["archive_sha256"] == sha256(initrd), "Initramfs build/profile mismatch")
        need(archive["source_commit"] == lock["sources"]["busybox"]["commit"], "BusyBox source pin mismatch")
        checked = {"builder": ROOT / "tools/build_initramfs.py", "init": ROOT / "guest/init",
                   "kernel_config": kernel_dir / ".config", "kernel_stamp": kernel_dir / "stamp.json",
                   "module": kernel_dir / "toy-pf/toy_pf.ko", "m1_spike": ROOT / "build/guest/m1-spike",
                   "lab_exit": ROOT / "build/guest/lab-exit", "busybox_config": ROOT / "build/busybox/.config"}
        for name, path in checked.items():
            need(archive["input_hashes"][name] == sha256(path), "Initramfs input changed: " + name)
        need(archive.get("guest_source_hashes") == guest_source_hashes(), "Guest userspace source inputs changed")
        programs = archive.get("guest_programs", {})
        need(set(programs) == {"labd", "labctl", "tenant"}, "Full guest programs missing from archive")
        for name, record in programs.items():
            path = ROOT / "build/guest" / name
            need(record["path"] == str(path.relative_to(ROOT)) and record["sha256"] == sha256(path),
                 "Guest program changed: " + name)
        return {"status": "PASS", "qemu_stamp_sha256": sha256(binary.parent / "stamp.json"),
                "kernel_stamp_sha256": sha256(kernel_dir / "stamp.json"),
                "initramfs_stamp_sha256": sha256(initrd.parent / "stamp.json")}
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        raise PrerequisiteError("Current guest build prerequisite unavailable: " + str(error)) from error


def validate_checkpoint_events(events):
    names = ("M1_COLD_CHECKPOINT", "M1_INITIAL_CHECKPOINT", "M1_FINAL_CHECKPOINT")
    selected = [event["event"] for event in events if event.get("event") in names]
    need(selected == list(names), "Cold, initial, and final host checkpoint order invalid")
    acknowledgements = [event for event in events if event.get("event") == "M1_HOST_CHECKPOINT_ACK"]
    need([event.get("checkpoint") for event in acknowledgements] == list(names) and
         all(event.get("status") == "PASS" for event in acknowledgements), "Host checkpoint acknowledgement evidence missing")
    ordered = [event.get("event") for event in events if event.get("event") in
               (*names, "warmup_enable_snapshot", "warmup_disable_snapshot")]
    need(ordered == [names[0], "warmup_enable_snapshot", "warmup_disable_snapshot", names[1], names[2]],
         "Warmup must occur between cold and initial checkpoints")
    positions = {name: next(index for index, item in enumerate(events) if item.get("event") == name) for name in names}
    for index, item in enumerate(events):
        if item.get("event") in ("enable_snapshot", "disable_snapshot"):
            need(positions[names[1]] < index < positions[names[2]], "Measured cycle outside initial/final checkpoints")
    for kind, expected in (("warmup_enable_snapshot", {"cycle": 0, "enabled": 4, "ledger_credit": 32}),
                           ("warmup_disable_snapshot", {"cycle": 0, "enabled": 0, "ledger_credit": 0,
                                                       "outstanding": 0, "vf_links": 0})):
        matching = [event for event in events if event.get("event") == kind]
        need(len(matching) == 1 and all(matching[0].get(key) == value for key, value in expected.items()),
             "Single complete warmup cycle evidence missing: " + kind)


def require_firmware(variant):
    base = ROOT / "build/edk2"
    try:
        stamp, selected = read_json(base / "stamp.json"), read_json(base / variant / "stamp.json")
        lock = read_json(ROOT / "sources.lock.json")
        need(stamp["status"] == selected["status"] == "PASS", "EDK2 build not PASS")
        need(stamp["source_commit"] == selected["source_commit"] == lock["sources"]["edk2"]["commit"],
             "EDK2 source pin mismatch")
        need(stamp["input_sha256"] == selected["input_sha256"] == edk2_fingerprint(), "EDK2 inputs changed since build")
        need(stamp["variants"][variant] == selected, "CODE/VARS variant stamp is from a different build")
        need(selected["sriov_support"] is (variant == "positive"), "Wrong SR-IOV firmware variant")
        need(stamp["probe_sha256"] == sha256(base / "SriovProbe.efi"), "UEFI probe hash mismatch")
        total = 0
        for name in ("OVMF_CODE.fd", "OVMF_VARS.fd"):
            path = base / variant / name
            expected = selected["images"][name]
            need(expected["sha256"] == sha256(path) and expected["bytes"] == path.stat().st_size,
                 "OVMF image/stamp mismatch: " + name)
            total += path.stat().st_size
        need(total == 4 * 1024 * 1024, "CODE/VARS do not form a 4 MiB flash image")
        pcd_path = (ROOT / selected["pcd"]["path"]).resolve()
        need(pcd_path.is_relative_to((ROOT / "build").resolve()) and
             selected["pcd"]["sha256"] == sha256(pcd_path), "Built PCD evidence changed")
        return {"variant": variant, "build_stamp_sha256": sha256(base / "stamp.json"),
                "variant_stamp_sha256": sha256(base / variant / "stamp.json"), "stamp": selected}
    except (OSError, KeyError, ValueError, RuntimeError) as error:
        raise PrerequisiteError("Current EDK2 prerequisite unavailable: " + str(error)) from error


def validate_probe(text, mode):
    """Check raw observations independently of the probe's PASS labels."""
    lines = text.splitlines()
    need(lines.count("SRIOV_PROBE_START ABI=1.0") == 1, "Expected exactly one firmware probe boot")
    need(lines.count("PROBE_MODE=" + mode) == 1, "Probe mode differs from requested mode")
    need(lines.count("PROBE_PASS") == 1 and "PROBE_FAILED" not in text, "Firmware probe did not pass exactly once")
    need(lines.count("CORE_SELFTEST=PASS") == 1, "UEFI portable core selftest evidence absent")
    need(lines.count("MAILBOX_VERSION=1.0") == 1 and
         lines.count("MAILBOX_UNKNOWN_MAJOR=REJECTED status=7") == 1, "Mailbox positive/unknown-major evidence absent")
    before = text.split("PROBE_PASS", 1)[0]
    cases, _ = parse_serial(before)
    need(M2_IDS <= cases.keys() and all(cases[name]["status"] == "PASS" for name in M2_IDS),
         "Required M2 cases missing before PROBE_PASS")
    values = {}
    for key, token in re.findall(r"\b([A-Z][A-Z0-9_]*)=(0x[0-9a-fA-F]+|[0-9]+)\b", before):
        need(key not in values, "Duplicate numeric probe field: " + key)
        values[key] = int(token, 16 if token.startswith("0x") else 10)
    for key, expected in {"CAP_TOTAL_VFS": 8, "CAP_INITIAL_VFS": 8, "VF_DEVICE_ID": 0xe002,
                          "VF_OFFSET": 1, "VF_STRIDE": 1, "PF_MSIX_VECTORS": 2}.items():
        need(values.get(key) == expected, "Unexpected firmware capability " + key)
    negative = mode.startswith("negative")
    supported, system = values.get("SRIOV_SUPPORTED_PAGE_SIZE", 0), values.get("SRIOV_SYSTEM_PAGE_SIZE", -1)
    need(supported > 0 and system >= 0 and (negative or system > 0) and
         (system == 0 or (system & (system - 1) == 0 and system & supported == system)),
         "Invalid SR-IOV supported/selected page size")
    need("SRIOV_CONTROL" in values and not values["SRIOV_CONTROL"] & 1, "Probe unexpectedly enabled VFs")
    locations = re.findall(r"^PF_LOCATION segment=(\d+) bus=(\d+) device=(\d+) function=(\d+)$", before, re.M)
    need(len(locations) == 1, "Unique PF location missing")
    segment, bus, device, function = map(int, locations[0])
    need(0 <= segment < 65536 and 0 <= bus < 256 and 0 <= device < 32 and 0 <= function < 8,
         "PF location out of range")
    pf = f"{segment:04x}:{bus:02x}:{device:02x}.{function:x}"
    standard = [(int(offset, 16), int(cap, 16)) for offset, cap in
                re.findall(r"^PCI_CAP offset=0x([0-9a-fA-F]+) id=0x([0-9a-fA-F]+)$", before, re.M)]
    extended = [(int(offset, 16), int(cap, 16), int(version)) for offset, cap, version in
                re.findall(r"^PCI_EXT_CAP offset=0x([0-9a-fA-F]+) id=0x([0-9a-fA-F]+) version=(\d+)$", before, re.M)]
    need(0 < len(standard) <= 48 and len({item[0] for item in standard}) == len(standard) and
         all(0x40 <= offset <= 0xfc and offset % 4 == 0 for offset, _ in standard) and
         {0x10, 0x11} <= {cap for _, cap in standard}, "Standard capability evidence invalid")
    need(0 < len(extended) <= 64 and len({item[0] for item in extended}) == len(extended) and
         all(0x100 <= offset <= 0xffc and offset % 4 == 0 and version > 0 for offset, _, version in extended) and
         {0xd, 0xe, 0x10} == {cap for _, cap, _ in extended}, "Extended capability evidence invalid")
    bars = {}
    for index, size in ((0, 65536), (2, 16384)):
        prefix = "PF_ASSIGNED_BAR" + str(index)
        start, end = values.get(prefix + "_BASE", 0), values.get(prefix + "_END", 0)
        need(values.get(prefix + "_LENGTH") == size and start > 0 and end == start + size - 1,
             "PF BAR assignment evidence invalid")
        bars["PF_BAR" + str(index)] = {"start": start, "end": end, "size": size}
    for index in (0, 2):
        prefix = "VF_BAR" + str(index)
        size = values.get(prefix + "_REQUIRED")
        start, end = values.get(prefix + "_ASSIGNED_BASE", -1), values.get(prefix + "_ASSIGNED_END", -1)
        need(size == values["CAP_TOTAL_VFS"] * 16384 and start >= 0 and end == start + size - 1,
             "VF aperture requirement/assignment evidence invalid")
        bars[prefix] = {"start": start, "end": end, "size": size}
    windows = []
    pattern = (r"^BRIDGE_WINDOW segment=(\d+) bdf=([0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]) "
               r"secondary=(\d+) subordinate=(\d+) base=0x([0-9a-fA-F]+) limit=0x([0-9a-fA-F]+)$")
    for bridge_segment, bridge_bdf, secondary, subordinate, base, limit in re.findall(pattern, before, re.M):
        window = {"segment": int(bridge_segment), "bdf": bridge_bdf.lower(), "secondary": int(secondary),
                  "subordinate": int(subordinate), "start": int(base, 16), "end": int(limit, 16)}
        need(window["segment"] == segment and window["secondary"] <= bus <= window["subordinate"] and
             window["start"] <= window["end"], "Upstream bridge window invalid")
        for name, bar in bars.items():
            if negative and name.startswith("VF_"):
                continue
            need(bar["start"] > 0 and window["start"] <= bar["start"] <= bar["end"] <= window["end"],
                 name + " lies outside firmware upstream window")
        windows.append(window)
    need(windows and values.get("BRIDGE_MATCH_COUNT") == len(windows), "Bridge window observations incomplete")
    need(len({(window["segment"], window["bdf"]) for window in windows}) == len(windows), "Duplicate bridge window")
    assertion = "OBSERVE_ONLY" if negative else "CHECKED"
    need("VF_WINDOW_ASSERTION=" + assertion in before, "Wrong VF aperture observation mode")
    return {"status": "PASS", "pf": pf, "values": values, "bars": bars, "windows": windows,
            "standard_caps": standard, "extended_caps": extended,
            "vf_window_assertion": assertion}


def validate_handoff_transport(text, initrd_size):
    need(text.count("HANDOFF_START=LINUX_EFI_STUB") == 1 and text.count("Linux version ") == 1,
         "Expected exactly one continuous Linux handoff")
    need(text.index("PROBE_PASS") < text.index("HANDOFF_START=LINUX_EFI_STUB"), "Invalid handoff ordering")
    need("HANDOFF_RETURNED" not in text and "Linux version " in text, "Linux did not take control")
    need(text.index("HANDOFF_START=LINUX_EFI_STUB") < text.index("Linux version "), "Linux boot preceded handoff")
    sizes = re.findall(r"^INITRD_LOADFILE2_BYTES=(\d+)$", text, re.M)
    need(len(sizes) == 1 and int(sizes[0]) == initrd_size, "EFI LoadFile2 did not deliver exact initrd")
    options = re.findall(r"^HANDOFF_LOAD_OPTIONS=(.+)$", text, re.M)
    need(len(options) == 1 and "console=ttyS0" in options[0] and "rdinit=/init" in options[0] and
         "intel_iommu=on" in options[0] and "iommu.strict=1" in options[0] and
         not re.search(r"pci=realloc|assign[-_]busses", options[0]), "Unsafe or incomplete handoff options")
    linux = text[text.index("Linux version "):]
    reports = [line for line in linux.splitlines() if KERNEL_REPORT.search(line)]
    need(not reports, "Unexpected Linux kernel diagnostics: " + repr(reports[:5]))
    return linux


def validate_handoff(text, cases, events, probe, initrd_size, host_checkpoints, negative=False):
    validate_handoff_transport(text, initrd_size)
    need(host_checkpoints["status"] == "PASS", "Continuous handoff host checkpoints missing")
    semantic = validate_m1_payload(cases, events)
    validate_checkpoint_events(events)
    need(cases["M1_GROUPS"]["pf"] == probe["pf"], "Linux PF location differs from firmware observation")
    vf_names = {group["vf"] for group in cases["M1_GROUPS"]["groups"]}
    resources = [event for event in events if event.get("event") == "pci_resource"]
    need(len(resources) == 10, "Expected Linux PF and four VF BAR0/BAR2 observations")
    seen, relations = set(), []
    for resource in resources:
        bdf, index = resource.get("bdf"), resource.get("bar")
        need((bdf, index) not in seen and bdf in vf_names | {probe["pf"]} and index in (0, 2),
             "Unexpected or duplicate Linux BAR resource")
        seen.add((bdf, index))
        start, end, flags = (int(resource[name], 16) for name in ("start", "end", "flags"))
        size = 65536 if bdf == probe["pf"] and index == 0 else 16384
        need(resource["size"] == size and end == start + size - 1 and flags & 0x200 and not flags & 0x2000,
             "Invalid Linux resource type or size")
        name = ("PF_BAR" if bdf == probe["pf"] else "VF_BAR") + str(index)
        aperture = probe["bars"][name]
        in_aperture = aperture["start"] <= start <= end <= aperture["end"]
        in_windows = all(window["start"] <= start <= end <= window["end"] for window in probe["windows"])
        if not negative:
            need(in_aperture, "Linux resource moved outside firmware aperture")
            need(in_windows, "Linux resource lies outside firmware upstream window")
        if bdf == probe["pf"] and not negative:
            need(start == aperture["start"] and end == aperture["end"], "Linux changed PF BAR allocation")
        relations.append({"bdf": bdf, "bar": index, "start": start, "end": end, "firmware_aperture": name,
                          "inside_firmware_aperture": in_aperture, "inside_firmware_windows": in_windows})
    return {"status": "PASS", "initrd_bytes": initrd_size, "m1_semantics": semantic,
            "linux_resource_relations": relations, "unexpected_kernel_reports": []}


def validate_negative(text, cases, events, probe, initrd_size, report):
    """Observe allocation behavior; unrelated errors cannot become expected failures."""
    linux = validate_handoff_transport(text, initrd_size)
    if report["exit_code"] == 1 and text.splitlines().count("LAB_RESULT=PASS") == 1 and "LAB_RESULT=FAIL" not in text:
        observed = validate_handoff(text, cases, events, probe, initrd_size, report["host_checkpoints"], negative=True)
        observed.update(linux_outcome="ENABLE4_AND_M1_PASS", linux_status="PASS")
        return observed
    need(report["exit_code"] == 3 and "LAB_RESULT=PASS" not in text and "LAB_RESULT=FAIL" in text,
         "Negative run has unexplained exit or marker disagreement")
    boots = []
    for line in linux.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(item, dict) and item.get("case") == "GUEST_BOOT":
            boots.append(item)
    need(len(boots) == 1 and boots[0].get("status") == "PASS", "Negative Linux guest did not initialize successfully")
    errors = re.findall(r"^M1_ERROR stage=(\S+) errno=(\d+) message=(.+)$", linux, re.M)
    need(len(errors) == 1 and errors[0][0] == "enable_four_vfs" and errors[0][1] == "12",
         "Negative failure is not the pinned Linux MMIO allocation path")
    need(cases.get("M1_FAILURE", {}).get("status") == "FAIL" and cases["M1_FAILURE"].get("errno") == 12,
         "Negative enable errno lacks structured corroboration")
    need(not any(event.get("event") in ("enable_snapshot", "warmup_enable_snapshot", "vfio_open") for event in events),
         "Negative failure occurred after successful enable; requires investigation")
    messages = [line for line in linux.splitlines() if probe["pf"] in line and
                ("not enough MMIO resources for SR-IOV" in line or "SR-IOV: IOV BARS not allocated" in line)]
    need(messages, "Missing same-PF kernel allocation error evidence")
    need("PROVISION_UNWIND_RETAINED" not in linux and "DESTROY_RETAINED" not in linux,
         "Negative failure leaked provisioned firmware quota")
    return {"status": "PASS", "linux_status": "FAIL", "linux_outcome": "ENABLE4_REJECTED_MMIO_RESOURCES",
            "errno": 12, "kernel_evidence": messages,
            "source_evidence": "downloads/linux/drivers/pci/iov.c:sriov_enable",
            "initrd_bytes": initrd_size, "unexpected_kernel_reports": [],
            "explanation": "The disabled-PCD firmware boot reached Linux, but its observed MMIO allocation prevented VF enable."}


def terminate(process):
    if process is not None and process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)


def run_profile(mode, profile="stress", accel="tcg", seed=101, timeout=None):
    out = artifact_dir("m2-" + mode)
    report = {"schema_version": 1, "gate": "m2", "mode": mode, "status": "FAIL", **provenance(),
              "accelerator": accel, "profile": profile, "seed": seed, "cases": [], "limitations": []}
    started = time.monotonic()
    binary = ROOT / "build/qemu/qemu-system-x86_64"
    kernel = ROOT / f"build/kernel-{profile}/arch/x86/boot/bzImage"
    initrd = ROOT / f"build/initramfs-{profile}/initramfs.cpio.gz"
    variant = "negative" if mode == "negative" else "positive"
    probe_mode = "negative-handoff" if mode == "negative" else mode
    firmware = ROOT / "build/edk2" / variant
    code, template, probe_image = firmware / "OVMF_CODE.fd", firmware / "OVMF_VARS.fd", ROOT / "build/edk2/SriovProbe.efi"
    variables = out / "OVMF_VARS.fd"
    process = qmp = None
    try:
        need(mode in MODES and profile in ("stress", "kasan") and accel == "tcg", "Invalid VM profile")
        need(timeout is None or timeout > 0, "Timeout must be positive")
        report["guest_builds"] = require_guest_builds(binary, kernel, initrd, profile)
        report["prerequisite_m1"] = require_m1(binary, kernel, initrd)
        report["firmware"] = require_firmware(variant)
        paths = (binary, kernel, initrd, probe_image, code, template)
        report["artifact_hashes"] = {str(path.relative_to(ROOT)): sha256(path) for path in paths}
        esp = out / "esp"
        (esp / "EFI/BOOT").mkdir(parents=True)
        shutil.copyfile(probe_image, esp / "EFI/BOOT/BOOTX64.EFI")
        (esp / "lab-mode.txt").write_text(probe_mode + "\n", encoding="ascii")
        if mode in ("handoff", "negative"):
            shutil.copyfile(kernel, esp / "linux.efi")
            shutil.copyfile(initrd, esp / "initramfs.cpio.gz")
            cmdline = f"console=ttyS0 rdinit=/init intel_iommu=on iommu.strict=1 toy_iommu_strict_wo=1 panic=-1 lab_gate=m1 lab_seed={seed}"
            (esp / "cmdline.txt").write_text(cmdline + "\n", encoding="ascii")
        report["esp_files"] = {str(path.relative_to(esp)): {"sha256": sha256(path), "bytes": path.stat().st_size}
                               for path in sorted(esp.rglob("*")) if path.is_file()}
        # 每次在新 artifact 目錄建立 VARS；exclusive create 禁止覆寫既有 store。
        with template.open("rb") as source, variables.open("xb") as destination:
            shutil.copyfileobj(source, destination)
        variables.chmod(0o600)
        report["nvram"] = {"fresh": True, "path": str(variables.relative_to(ROOT)),
                           "template_sha256": sha256(template), "initial_sha256": sha256(variables)}
        need(report["nvram"]["initial_sha256"] == report["nvram"]["template_sha256"], "Fresh VARS copy mismatch")
        with resource_lock(), tempfile.TemporaryDirectory(prefix="sriov-efi-") as temporary:
            # 取得互斥鎖後再次檢查，避免 preflight 與 VM 啟動之間另一個 build 更新輸出。
            need(report["artifact_hashes"] == {str(path.relative_to(ROOT)): sha256(path) for path in paths},
                 "Build artifacts changed between preflight and resource lock")
            run = Path(temporary)
            run.chmod(0o700)
            socket, serial_socket = run / "qmp.sock", run / "serial.sock"
            # Probe-only speaks UART0. Linux boots retain firmware/kernel on
            # UART0 and reserve UART1 for the bidirectional test protocol.
            uart_args = [] if mode == "probe" else ["-serial", f"file:{out / 'kernel.log'}"]
            uart_args += ["-chardev", f"socket,id=uart,path={serial_socket},server=on,wait=off", "-serial", "chardev:uart"]
            argv = [str(binary), "-machine", "q35",
                    "-accel", "tcg,thread=multi", "-cpu", "max",
                    "-m", "2048M" if profile == "kasan" else "1536M", "-smp", "2", "-display", "none", "-monitor", "none", "-S",
                    *uart_args,
                    "-nic", "none", "-no-reboot", "-device", "intel-iommu,intremap=on",
                    "-device", "pcie-root-port,id=rp1,chassis=1,slot=1", "-device", "toy-sriov-pf,bus=rp1,id=toy,total-vfs=8",
                    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-qmp", f"unix:{socket},server=on,wait=off",
                    "-drive", f"if=pflash,unit=0,format=raw,readonly=on,file={code}",
                    "-drive", f"if=pflash,unit=1,format=raw,file={variables}",
                    "-drive", f"if=none,id=esp,format=raw,readonly=on,file=fat:ro:{esp}",
                    "-device", "virtio-blk-pci,drive=esp,bootindex=1"]
            write_json(out / "command.json", {"argv": argv, "cwd": str(ROOT),
                       "vvfat_source": "downloads/qemu/block/vvfat.c:vvfat_parse_filename"})
            try:
                with (out / "qemu.log").open("wb") as log:
                    process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                               start_new_session=True, env={**os.environ}, umask=0o077)
                    report["qemu_pid"] = process.pid
                    qmp = QMP(socket, out / "qmp.jsonl")
                    socket.chmod(0o600)
                    report["qom_path"] = qmp.toy_path
                    duration = timeout or (1200 if profile == "kasan" else 600)
                    report["timeout_seconds"] = duration
                    report.update(monitor_guest(process, qmp, serial_socket, out, duration))
            finally:
                terminate(process)
                if qmp is not None:
                    qmp.close()
                    qmp = None
        text = read_uefi_output(out, mode)
        # UART 原始 CRLF 保存在檔案；解析使用 Python 通用換行，證據不改寫。
        cases, events = parse_serial(text)
        report["cases"] = list(cases.values())
        report["probe_observations"] = validate_probe(text, probe_mode)
        if mode != "negative":
            need(not any(case["status"] == "FAIL" for case in cases.values()), "A guest case explicitly failed")
            need(text.splitlines().count("LAB_RESULT=PASS") == 1 and "LAB_RESULT=FAIL" not in text,
                 "Missing, duplicate, or contradictory final marker")
            need(report["exit_code"] == (1 if mode == "handoff" else 0), "Final marker and QEMU exit code disagree")
        if mode == "handoff":
            report["handoff_validation"] = validate_handoff(text, cases, events, report["probe_observations"],
                                                          initrd.stat().st_size, report["host_checkpoints"])
        elif mode == "negative":
            report["negative_observation"] = validate_negative(text, cases, events, report["probe_observations"],
                                                              initrd.stat().st_size, report)
        report["status"] = "PASS"
    except PrerequisiteError as error:
        report.update(status="BLOCKED", error=str(error))
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, EOFError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)):
            report["timed_out"] = True
    finally:
        terminate(process)
        if qmp is not None:
            qmp.close()
        if variables.exists():
            report.setdefault("nvram", {})["final_sha256"] = sha256(variables)
        report["elapsed_seconds"] = time.monotonic() - started
        report["evidence_hashes"] = {name: sha256(out / name) for name in
                                    ("command.json", "serial.log", "kernel.log", "qemu.log", "qmp.jsonl") if (out / name).is_file()}
        write_json(out / "result.json", report)
        write_json(ROOT / f"artifacts/m2-{mode}-latest.json", {"path": str(out.relative_to(ROOT)), "status": report["status"]})
    print(json.dumps({"status": report["status"], "mode": mode, "evidence": str(out.relative_to(ROOT)),
                      **({"error": report["error"]} if "error" in report else {})}), flush=True)
    return report, out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=MODES, default="probe")
    parser.add_argument("--profile", choices=("stress", "kasan"), default="stress")
    parser.add_argument("--accel", choices=("tcg",), default="tcg")
    parser.add_argument("--seed", type=int, default=101)
    parser.add_argument("--timeout", type=int)
    args = parser.parse_args()
    result, _ = run_profile(args.mode, args.profile, args.accel, args.seed, args.timeout)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
