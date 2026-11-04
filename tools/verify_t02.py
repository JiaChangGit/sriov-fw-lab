#!/usr/bin/env python3
"""Execute every T02 subset against one current build; never approve the full M3 gate."""
import json
import subprocess
import sys
from types import SimpleNamespace
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from run_vm import KERNEL_REPORT
from validate_m3 import validate_t02_provision, validate_t02_owner, validate_t02_pci
from verify_m2 import validate_saved_run


def main():
    out = artifact_dir("t02")
    result = {**provenance(), "id": "T02", "status": "FAIL", "m3_status": "NOT_RUN", "subsets": []}
    try:
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"] = require_m1(binary, ROOT / "build/kernel-stress/arch/x86/boot/bzImage",
                                               ROOT / "build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"] = [validate_saved_run(mode, "stress") for mode in ("probe", "handoff", "negative")]
        result["builds"] = require_guest_builds(binary, ROOT / "build/kernel-kasan/arch/x86/boot/bzImage",
                                                ROOT / "build/initramfs-kasan/initramfs.cpio.gz", "kasan")
        hashes = None
        for scenario in ("T02-provision", "T02-owner", "T02-pci"):
            run_logged([sys.executable, str(ROOT / "tools/run_vm.py"), "--gate", "m3-case", "--profile", "kasan",
                        "--accel", "tcg", "--case", scenario], out, scenario, timeout=1000)
            pointer = json.loads((ROOT / "artifacts/m3-case-latest.json").read_text())
            directory = ROOT / pointer["path"]
            report = json.loads((directory / "result.json").read_text())
            if report["status"] != "PASS" or report["unexpected_kernel_reports"]:
                raise RuntimeError("T02 subset did not pass")
            if hashes is None: hashes = report["artifact_hashes"]
            if report["artifact_hashes"] != hashes:
                raise RuntimeError("T02 subsets used different builds")
            for name, digest in report["evidence_hashes"].items():
                if sha256(directory / name) != digest:
                    raise RuntimeError("T02 raw evidence changed: " + name)
            serial = (directory / "serial.log").read_text()
            kernel = (directory / "kernel.log").read_text()
            device = (directory / "qemu.log").read_text()
            if any(KERNEL_REPORT.search(line) for line in (kernel + serial).splitlines()):
                raise RuntimeError("T02 unexpected kernel diagnostic")
            cases, events = parse_serial(serial)
            controller = json.loads((directory / "m3-barriers.json").read_text())
            inventory = {name: json.loads((directory / ("m3-qom-" + name + ".json")).read_text())
                         for name in ("cold", "initial", "final")} if scenario != "T02-owner" else {}
            state = SimpleNamespace(armed=controller["pending_failpoints"], records=controller["records"], inventory=inventory)
            if scenario == "T02-provision": checked = validate_t02_provision(cases, events, kernel, device, state)
            elif scenario == "T02-owner": checked = validate_t02_owner(cases, events, device)
            else: checked = validate_t02_pci(cases, events, kernel, device, state)
            result["subsets"].append({"scenario": scenario, "path": pointer["path"],
                                      "result_sha256": sha256(directory / "result.json"), "validation": checked})
        result.update(status="PASS", scope="T02_complete_fault_matrix", artifact_hashes=hashes)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, flush=True)
    write_json(out / "result.json", result)
    write_json(ROOT / "artifacts/t02-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "scope": "T02_only", "m3_status": "NOT_RUN", "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
