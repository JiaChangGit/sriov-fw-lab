#!/usr/bin/env python3
"""Run both-domain T04 at all ring sizes after current M1/M2; M3 remains incomplete."""
import json
import subprocess
import sys
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from run_vm import KERNEL_REPORT
from validate_m3 import validate_t04
from verify_m2 import validate_saved_run


def main():
    out = artifact_dir("t04")
    result = {**provenance(), "id": "T04", "status": "FAIL", "m3_status": "NOT_RUN", "variants": []}
    try:
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"] = require_m1(binary, ROOT / "build/kernel-stress/arch/x86/boot/bzImage",
                                               ROOT / "build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"] = [validate_saved_run(mode, "stress") for mode in ("probe", "handoff", "negative")]
        result["builds"] = require_guest_builds(binary, ROOT / "build/kernel-kasan/arch/x86/boot/bzImage",
                                                ROOT / "build/initramfs-kasan/initramfs.cpio.gz", "kasan")
        hashes = None
        for entries in (16, 32, 256):
            run_logged([sys.executable, str(ROOT / "tools/run_vm.py"), "--gate", "m3-case", "--profile", "kasan",
                        "--accel", "tcg", "--case", "T04", "--entries", str(entries)], out, f"entries-{entries}", timeout=1000)
            pointer = json.loads((ROOT / "artifacts/m3-case-latest.json").read_text())
            directory = ROOT / pointer["path"]
            report = json.loads((directory / "result.json").read_text())
            if report["status"] != "PASS" or report["unexpected_kernel_reports"]:
                raise RuntimeError("T04 variant did not pass")
            if hashes is None: hashes = report["artifact_hashes"]
            if report["artifact_hashes"] != hashes:
                raise RuntimeError("T04 variants used different builds")
            for name, digest in report["evidence_hashes"].items():
                if sha256(directory / name) != digest:
                    raise RuntimeError("T04 raw evidence changed: " + name)
            serial = (directory / "serial.log").read_text()
            kernel = (directory / "kernel.log").read_text()
            if any(KERNEL_REPORT.search(line) for line in (kernel + serial).splitlines()):
                raise RuntimeError("T04 unexpected kernel diagnostic")
            cases, events = parse_serial(serial)
            checked = validate_t04(cases, events, entries, (directory / "qemu.log").read_text())
            for name, records in checked["evidence"].items():
                if json.loads((directory / name).read_text()) != records:
                    raise RuntimeError("T04 derived evidence disagrees with raw logs: " + name)
            result["variants"].append({"entries": entries, "path": pointer["path"],
                "result_sha256": sha256(directory / "result.json"), "validation": checked})
        result.update(status="PASS", artifact_hashes=hashes)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, flush=True)
    write_json(out / "result.json", result)
    write_json(ROOT / "artifacts/t04-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "scope": "T04_only", "m3_status": "NOT_RUN", "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
