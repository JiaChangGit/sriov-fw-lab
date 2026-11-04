#!/usr/bin/env python3
"""Run all T03 variants; never claim that the complete M3 gate passed."""
import json
import subprocess
import sys
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from validate_m3 import validate_t03
from verify_m2 import validate_saved_run


def main():
    out = artifact_dir("t03")
    result = {**provenance(), "id": "T03", "status": "FAIL", "m3_status": "NOT_RUN", "variants": []}
    try:
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"] = require_m1(binary, ROOT / "build/kernel-stress/arch/x86/boot/bzImage",
                                               ROOT / "build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"] = [validate_saved_run(mode, "stress") for mode in ("probe", "handoff", "negative")]
        result["builds"] = require_guest_builds(binary, ROOT / "build/kernel-kasan/arch/x86/boot/bzImage",
                                                ROOT / "build/initramfs-kasan/initramfs.cpio.gz", "kasan")
        run_logged([sys.executable, str(ROOT / "tools/verify_guest_native.py")], out, "native", timeout=120)
        native_pointer = json.loads((ROOT / "artifacts/guest-native-latest.json").read_text())
        native = ROOT / native_pointer["path"] / "result.json"
        if json.loads(native.read_text())["status"] != "PASS":
            raise RuntimeError("Native wire/UAPI/ring oracle failed")
        result["native"] = {"path": str(native.relative_to(ROOT)), "sha256": sha256(native)}
        for entries in (16, 32, 256):
            run_logged([sys.executable, str(ROOT / "tools/run_vm.py"), "--gate", "m3-case", "--profile", "kasan",
                        "--accel", "tcg", "--case", "T03", "--entries", str(entries)], out, f"entries-{entries}", timeout=1000)
            pointer = json.loads((ROOT / "artifacts/m3-case-latest.json").read_text())
            directory = ROOT / pointer["path"]
            report = json.loads((directory / "result.json").read_text())
            if report["status"] != "PASS" or report["unexpected_kernel_reports"]:
                raise RuntimeError("Guest variant did not pass")
            cases, events = parse_serial((directory / "serial.log").read_text())
            checked = validate_t03(cases, events, entries, (directory / "qemu.log").read_text())
            result["variants"].append({"entries": entries, "path": pointer["path"],
                "result_sha256": sha256(directory / "result.json"), "validation": checked})
        result["status"] = "PASS"
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, flush=True)
    result["remaining_cases"] = {f"T{i:02}": "NOT_RUN" for i in (1,2,4,5,6,7,8,9,10)}
    write_json(out / "result.json", result)
    write_json(ROOT / "artifacts/t03-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "scope": "T03_only", "m3_status": "NOT_RUN", "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
