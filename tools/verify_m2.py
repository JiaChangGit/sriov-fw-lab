#!/usr/bin/env python3
"""Build firmware and verify all three mandatory M2 TCG boot paths sequentially."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from lab_common import ROOT, artifact_dir, checked_jobs, provenance, run_logged, sha256, write_json
from run_uefi import (MODES, PrerequisiteError, evidence_path, need, parse_serial, read_json, read_uefi_output,
                      require_firmware, require_guest_builds, require_m1, validate_handoff,
                      validate_negative, validate_probe)


def validate_saved_run(mode, profile):
    """Reparse raw boot evidence; a saved PASS label alone is insufficient."""
    pointer = read_json(ROOT / f"artifacts/m2-{mode}-latest.json")
    directory = evidence_path(pointer["path"])
    result = read_json(directory / "result.json")
    need(pointer["status"] == result["status"] == "PASS", f"M2 {mode} did not pass")
    need(result["mode"] == mode and result["profile"] == profile and result["accelerator"] == "tcg",
         "M2 boot mode/profile/accelerator mismatch")
    need(not result.get("timed_out"), "Timed out M2 boot cannot pass")
    for relative, expected in result["artifact_hashes"].items():
        path = (ROOT / relative).resolve()
        need(path.is_relative_to((ROOT / "build").resolve()) and sha256(path) == expected,
             "M2 boot artifact changed: " + relative)
    names = ("command.json", "serial.log", "qemu.log", "qmp.jsonl") + (() if mode == "probe" else ("kernel.log",))
    for name in names:
        need(result["evidence_hashes"][name] == sha256(directory / name), "M2 raw evidence changed: " + name)
    firmware = require_firmware("negative" if mode == "negative" else "positive")
    need(result["firmware"] == firmware, "M2 firmware build changed since boot")
    variables = result["nvram"]
    need(variables["fresh"] is True and variables["template_sha256"] == variables["initial_sha256"] ==
         firmware["stamp"]["images"]["OVMF_VARS.fd"]["sha256"], "M2 did not boot from matching fresh VARS")
    need(evidence_path(variables["path"]) == directory / "OVMF_VARS.fd" and
         variables["final_sha256"] == sha256(directory / "OVMF_VARS.fd"), "M2 VARS evidence changed")
    for relative, record in result["esp_files"].items():
        path = (directory / "esp" / relative).resolve()
        need(path.is_relative_to(directory / "esp") and record["sha256"] == sha256(path) and
             record["bytes"] == path.stat().st_size, "M2 ESP evidence changed")
    argv = read_json(directory / "command.json")["argv"]
    need("-kernel" not in argv and "-initrd" not in argv and "-append" not in argv and
         not any("pci=realloc" in arg or "assign-busses" in arg or "assign_busses" in arg for arg in argv),
         "M2 command bypassed firmware handoff")
    text = read_uefi_output(directory, mode)
    cases, events = parse_serial(text)
    probe = validate_probe(text, "negative-handoff" if mode == "negative" else mode)
    initrd_size = (ROOT / f"build/initramfs-{profile}/initramfs.cpio.gz").stat().st_size
    if mode == "negative":
        observation = validate_negative(text, cases, events, probe, initrd_size, result)
    else:
        need(result["exit_code"] == (1 if mode == "handoff" else 0), "M2 exit code mismatch")
        need(text.splitlines().count("LAB_RESULT=PASS") == 1 and "LAB_RESULT=FAIL" not in text and
             not any(case["status"] == "FAIL" for case in cases.values()), "M2 guest failed or final marker invalid")
        observation = (validate_handoff(text, cases, events, probe, initrd_size, result["host_checkpoints"])
                       if mode == "handoff" else {"status": "PASS", "probe_only": True})
    if mode == "handoff" or observation.get("linux_status") == "PASS":
        for side in ("initial", "final"):
            need(result["host_checkpoints"][side + "_sha256"] == sha256(directory / f"qom-{side}.json"),
                 "M2 QOM checkpoint evidence changed")
        need((directory / "qom-cold.json").is_file(), "M2 cold checkpoint missing")
    return {"id": mode, "status": "PASS", "path": str(directory.relative_to(ROOT)),
            "result_sha256": sha256(directory / "result.json"), "observation": observation,
            "probe_observations": probe, "nvram_path": variables["path"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--accel", choices=("tcg",), default="tcg", help="The mandatory M2 gate requires TCG")
    parser.add_argument("--profile", choices=("stress",), default="stress")
    parser.add_argument("--reuse-build", action="store_true", help="Use already-built firmware only after current-input and image checks")
    parser.add_argument("--reuse-runs", action="store_true", help="Revalidate raw evidence from current binaries; implies --reuse-build")
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    out = artifact_dir("m2")
    report = {"schema_version": 1, "gate": "m2", "status": "FAIL", **provenance(),
              "profile": args.profile, "accelerator": "tcg", "cases": [],
              "reuse_build": args.reuse_build or args.reuse_runs, "reuse_runs": args.reuse_runs}
    try:
        jobs = checked_jobs(args.jobs)
        need(args.timeout > 0, "Timeout must be positive")
        binary = ROOT / "build/qemu/qemu-system-x86_64"
        kernel = ROOT / f"build/kernel-{args.profile}/arch/x86/boot/bzImage"
        initrd = ROOT / f"build/initramfs-{args.profile}/initramfs.cpio.gz"
        report["guest_builds"] = require_guest_builds(binary, kernel, initrd, args.profile)
        report["prerequisite_m1"] = require_m1(binary, kernel, initrd)
        run_logged([sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tests/scenarios"),
                    "-p", "test_m2_controller.py", "-v"], out, "controller-unit", timeout=120)
        if not (args.reuse_build or args.reuse_runs):
            run_logged([sys.executable, str(ROOT / "tools/build_edk2.py"), "--jobs", str(jobs)],
                       out, "build-edk2", timeout=18000)
        report["firmware"] = {variant: require_firmware(variant) for variant in ("positive", "negative")}
        for mode in MODES:
            if not args.reuse_runs:
                run_logged([sys.executable, str(ROOT / "tools/run_uefi.py"), "--mode", mode,
                            "--profile", args.profile, "--accel", "tcg", "--timeout", str(args.timeout)],
                           out, mode, timeout=args.timeout + 120)
            report["cases"].append(validate_saved_run(mode, args.profile))
        need(len({case["nvram_path"] for case in report["cases"]}) == 3, "M2 boots reused a VARS store")
        report["status"] = "PASS"
    except PrerequisiteError as error:
        report.update(status="BLOCKED", error=str(error))
    except (OSError, RuntimeError, KeyError, ValueError, TypeError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    finally:
        completed = {case["id"] for case in report["cases"]}
        report["cases"].extend({"id": mode, "status": "NOT_RUN", "reason": "No accepted raw boot evidence in this aggregate"}
                               for mode in MODES if mode not in completed)
        write_json(out / "result.json", report)
        write_json(ROOT / "artifacts/m2-latest.json", {"path": str(out.relative_to(ROOT)), "status": report["status"]})
    print(json.dumps({"gate": "m2", "status": report["status"], "evidence": str(out.relative_to(ROOT)),
                      **({"error": report["error"]} if "error" in report else {})}), flush=True)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
