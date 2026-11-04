#!/usr/bin/env python3
"""Run the full lease-owned T01 lifecycle budget after current M1/M2."""
import json
import subprocess
import sys
from types import SimpleNamespace
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from run_vm import KERNEL_REPORT
from validate_m3 import validate_t01
from verify_m2 import validate_saved_run


def main():
    out = artifact_dir("t01")
    result = {**provenance(), "id": "T01", "status": "FAIL", "m3_status": "NOT_RUN"}
    try:
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"] = require_m1(binary, ROOT / "build/kernel-stress/arch/x86/boot/bzImage",
                                               ROOT / "build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"] = [validate_saved_run(mode, "stress") for mode in ("probe", "handoff", "negative")]
        result["builds"] = require_guest_builds(binary, ROOT / "build/kernel-kasan/arch/x86/boot/bzImage",
                                                ROOT / "build/initramfs-kasan/initramfs.cpio.gz", "kasan")
        run_logged([sys.executable, str(ROOT / "tools/run_vm.py"), "--gate", "m3-case", "--profile", "kasan",
                    "--accel", "tcg", "--case", "T01", "--timeout", "2400"], out, "cycles-100", timeout=2500)
        pointer = json.loads((ROOT / "artifacts/m3-case-latest.json").read_text())
        directory = ROOT / pointer["path"]
        report = json.loads((directory / "result.json").read_text())
        if report["status"] != "PASS" or report["unexpected_kernel_reports"]:
            raise RuntimeError("T01 lifecycle did not pass")
        for name, digest in report["evidence_hashes"].items():
            if sha256(directory / name) != digest:
                raise RuntimeError("T01 raw evidence changed: " + name)
        serial = (directory / "serial.log").read_text()
        kernel = (directory / "kernel.log").read_text()
        if any(KERNEL_REPORT.search(line) for line in (kernel + serial).splitlines()):
            raise RuntimeError("T01 unexpected kernel diagnostic")
        cases, events = parse_serial(serial)
        barriers = json.loads((directory / "m3-barriers.json").read_text())
        inventory = {name: json.loads((directory / ("m3-qom-" + name + ".json")).read_text())
                     for name in ("cold", "initial", "final")}
        controller = SimpleNamespace(records=barriers["records"], armed=barriers["pending_failpoints"], inventory=inventory)
        checked = validate_t01(cases, events, (directory / "qemu.log").read_text(), controller)
        for name, records in checked["evidence"].items():
            if json.loads((directory / name).read_text()) != records:
                raise RuntimeError("T01 derived evidence disagrees with raw logs: " + name)
        result.update(status="PASS", artifact_hashes=report["artifact_hashes"], path=pointer["path"],
                      result_sha256=sha256(directory / "result.json"), validation=checked)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, flush=True)
    write_json(out / "result.json", result)
    write_json(ROOT / "artifacts/t01-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "scope": "T01_only", "m3_status": "NOT_RUN", "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
