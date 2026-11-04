#!/usr/bin/env python3
"""Verify one real VFIO reset plus close/reopen while the other VF keeps running."""
import json
import subprocess
import sys
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from run_vm import KERNEL_REPORT
from validate_t06 import validate_t06
from verify_m2 import validate_saved_run


def main():
    out = artifact_dir("t06")
    result = {**provenance(), "id":"T06", "status":"FAIL", "m3_status":"NOT_RUN"}
    try:
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"] = require_m1(binary, ROOT / "build/kernel-stress/arch/x86/boot/bzImage",
                                              ROOT / "build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"] = [validate_saved_run(mode,"stress") for mode in ("probe","handoff","negative")]
        result["builds"] = require_guest_builds(binary, ROOT / "build/kernel-kasan/arch/x86/boot/bzImage",
                                               ROOT / "build/initramfs-kasan/initramfs.cpio.gz", "kasan")
        run_logged([sys.executable,"tools/run_vm.py","--gate","m3-case","--profile","kasan","--case","T06"],out,"vfio-reset",timeout=1000)
        pointer = json.loads((ROOT / "artifacts/m3-case-latest.json").read_text())
        directory = ROOT / pointer["path"]
        report = json.loads((directory / "result.json").read_text())
        if report["status"] != "PASS" or report["unexpected_kernel_reports"]:
            raise RuntimeError("Actual T06 did not pass")
        for name, digest in report["evidence_hashes"].items():
            if sha256(directory/name) != digest: raise RuntimeError("Evidence changed: " + name)
        serial = (directory / "serial.log").read_text()
        if any(KERNEL_REPORT.search(line) for line in ((directory / "kernel.log").read_text()+serial).splitlines()):
            raise RuntimeError("Unexpected kernel diagnostic")
        cases, events = parse_serial(serial)
        checked = validate_t06(cases,events,(directory / "qemu.log").read_text())
        for name, rows in checked["evidence"].items():
            if json.loads((directory/name).read_text()) != rows: raise RuntimeError("Derived evidence mismatch: " + name)
        result.update(status="PASS", path=pointer["path"], result_sha256=sha256(directory/"result.json"),
                      validation=checked, artifact_hashes=report["artifact_hashes"])
    except (OSError,ValueError,KeyError,RuntimeError,subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error,flush=True)
    write_json(out/"result.json",result)
    write_json(ROOT/"artifacts/t06-latest.json",{"path":str(out.relative_to(ROOT)),"status":result["status"]})
    print(json.dumps({"status":result["status"],"scope":"T06_only","m3_status":"NOT_RUN","evidence":str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
