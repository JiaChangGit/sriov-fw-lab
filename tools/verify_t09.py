#!/usr/bin/env python3
"""Verify actual stale/malformed DMA submissions and replay their bytes under sanitizers."""
import json
import os
import subprocess
import sys
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from run_vm import KERNEL_REPORT
from validate_m3 import validate_t09
from verify_m2 import validate_saved_run


def sanitize_vectors(out, vectors):
    inputs = sorted((ROOT / "fwcore").glob("*.[ch]")) + [ROOT / "tests/scenarios/decoder_vectors.c"]
    binary = out / "decoder-vectors"
    data = out / "descriptor-input.bin"
    data.write_bytes(b"".join(bytes.fromhex(v["descriptor_hex"]) for v in vectors))
    run_logged(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "fwcore"), *[str(p) for p in inputs if p.suffix == ".c"],
                "-o", str(binary)], out, "decoder-compile", timeout=60)
    command = run_logged([str(binary), str(data)], out, "sanitizer", timeout=60,
                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
                     "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
    expected = [f"T09_DECODER vector={i} status={13 if i else 0} bounded_bytes=32" for i in range(6)]
    if (out / "sanitizer.log").read_text().splitlines() != expected:
        raise RuntimeError("Sanitizer decoder replay lacks exact six-vector evidence")
    return {"status": "PASS", "command": command, "inputs": {str(p.relative_to(ROOT)): sha256(p) for p in inputs},
            "binary_sha256": sha256(binary), "descriptor_input_sha256": sha256(data),
            "sanitizer_log_sha256": sha256(out / "sanitizer.log")}


def main():
    out = artifact_dir("t09")
    result = {**provenance(), "id": "T09", "status": "FAIL", "m3_status": "NOT_RUN"}
    try:
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"] = require_m1(binary, ROOT / "build/kernel-stress/arch/x86/boot/bzImage",
                                               ROOT / "build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"] = [validate_saved_run(mode, "stress") for mode in ("probe", "handoff", "negative")]
        result["builds"] = require_guest_builds(binary, ROOT / "build/kernel-kasan/arch/x86/boot/bzImage",
                                                ROOT / "build/initramfs-kasan/initramfs.cpio.gz", "kasan")
        run_logged([sys.executable, str(ROOT / "tools/run_vm.py"), "--gate", "m3-case", "--profile", "kasan",
                    "--accel", "tcg", "--case", "T09"], out, "guest", timeout=1000)
        pointer = json.loads((ROOT / "artifacts/m3-case-latest.json").read_text())
        directory = ROOT / pointer["path"]
        report = json.loads((directory / "result.json").read_text())
        if report["status"] != "PASS" or report["unexpected_kernel_reports"]:
            raise RuntimeError("T09 guest did not pass")
        for name, digest in report["evidence_hashes"].items():
            if sha256(directory / name) != digest:
                raise RuntimeError("T09 raw evidence changed: " + name)
        serial = (directory / "serial.log").read_text()
        kernel = (directory / "kernel.log").read_text()
        if any(KERNEL_REPORT.search(line) for line in (kernel + serial).splitlines()):
            raise RuntimeError("T09 unexpected kernel diagnostic")
        cases, events = parse_serial(serial)
        checked = validate_t09(cases, events, 16, (directory / "qemu.log").read_text())
        for name, records in checked["evidence"].items():
            if json.loads((directory / name).read_text()) != records:
                raise RuntimeError("T09 derived evidence disagrees with raw logs: " + name)
            write_json(out / name, records)
        result["sanitizers"] = sanitize_vectors(out, checked["evidence"]["descriptor-vectors.json"])
        result.update(status="PASS", artifact_hashes=report["artifact_hashes"], path=pointer["path"],
                      result_sha256=sha256(directory / "result.json"), validation=checked)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, flush=True)
    write_json(out / "result.json", result)
    write_json(ROOT / "artifacts/t09-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "scope": "T09_only", "m3_status": "NOT_RUN", "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
