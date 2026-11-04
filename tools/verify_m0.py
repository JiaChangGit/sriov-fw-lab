#!/usr/bin/env python3
"""Run environment/schema/source checks and the complete native core gate."""
import argparse
import json
import subprocess
import sys
from build_core import binary_hashes, source_hashes
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json


# 舊 PASS 必須同時對應來源、binary 和當時的 build stamp，不能只檢查檔案仍存在。
def validate_core_reuse(core, stamp, stamp_sha256, current_inputs, current_binaries):
    """A previous PASS belongs to the build actually executed, not any newer build."""
    if (stamp.get('status') != 'PASS' or stamp.get('inputs') != current_inputs or
            stamp.get('binary_hashes') != current_binaries or
            core.get('core_inputs') != current_inputs or
            core.get('binary_hashes') != current_binaries or
            core.get('core_build_sha256') != stamp_sha256):
        raise RuntimeError('Core execution does not match current build inputs/binaries; rerun verify-m0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reuse-core", action="store_true", help="Reuse a completed full core run only if its build inputs still match")
    args = parser.parse_args()
    out = artifact_dir("m0")
    report = {"schema_version": 1, "gate": "m0", "status": "FAIL", **provenance(), "cases": []}
    try:
        for name, script in (("environment", "env_check.py"), ("abi-schema", "gen_abi.py"), ("source-evidence", "source_evidence.py")):
            argv = [sys.executable, str(ROOT / "tools" / script)]
            if name == "abi-schema": argv.append("--check")
            record = run_logged(argv, out, name)
            report["cases"].append({"id": name, "status": "PASS", "evidence": [record["log"]]})
        lock = json.loads((ROOT / "sources.lock.json").read_text())
        for name in ("qemu", "linux", "edk2", "busybox"):
            item = lock["sources"][name]
            directory = ROOT / item["path"]
            actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=directory, text=True).strip()
            if actual != item["commit"] or item["source_status"] != "FETCHED":
                raise RuntimeError("Source pin mismatch: " + name)
            for submodule in item["submodules"]:
                child = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=directory / submodule["path"], text=True).strip()
                if child != submodule["commit"]: raise RuntimeError("Submodule mismatch: " + submodule["path"])
        report["cases"].append({"id": "source-pins", "status": "PASS", "evidence": ["sources.lock.json"]})
        if not args.reuse_core:
            run_logged([sys.executable, str(ROOT / "tools/verify_core.py")], out, "core-gate", timeout=1800)
        latest = json.loads((ROOT / "artifacts/verify-core-latest.json").read_text())
        core = json.loads((ROOT / latest["path"] / "result.json").read_text())
        core_directory = (ROOT / latest['path']).resolve()
        if not core.get('evidence_hashes'):
            raise RuntimeError('Core run lacks recorded evidence hashes; rerun verify-m0')
        for name, digest in core['evidence_hashes'].items():
            evidence = (core_directory / name).resolve()
            if not evidence.is_relative_to(core_directory) or sha256(evidence) != digest:
                raise RuntimeError('Core evidence changed: ' + name)
        stamp = json.loads((ROOT / "build/core/build.json").read_text())
        validate_core_reuse(core, stamp, sha256(ROOT / 'build/core/build.json'), source_hashes(), binary_hashes())
        # 核對案例與執行預算；少跑 fuzz/oracle 的快速檢查不能冒充完整 M0。
        required = {"I8-portability", "native-unit", "sanitize-unit", "coverage-unit", "sanitize-oracle",
                    "coverage-oracle", "coverage", "fuzz-mailbox_state", "fuzz-descriptor_scheduler"}
        passed = {case["id"] for case in core["cases"] if case["status"] == "PASS"}
        if core["status"] != "PASS" or not required.issubset(passed) or stamp["inputs"] != source_hashes():
            raise RuntimeError("Core verification incomplete or build inputs changed")
        if core["operations_per_seed"] < 100000 or len(core["seeds"]) < 4 or core["fuzz_runs_per_target"] < 100000 or core["fuzz_exploration_seconds_per_target"] < 300 or core["core_line_coverage_percent"] < 90:
            raise RuntimeError("M0 required budget/coverage not met")
        report["cases"].append({"id": "core", "status": "PASS", "evidence": [latest["path"] + "/result.json"]})
        report.update(status="PASS", core_line_coverage_percent=core["core_line_coverage_percent"],
                      seeds=core["seeds"], operations_per_seed=core["operations_per_seed"], core_inputs=stamp["inputs"])
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(error)
    write_json(out / "result.json", report)
    write_json(ROOT / "artifacts/m0-latest.json", {"path": str(out.relative_to(ROOT)), "status": report["status"]})
    print(json.dumps({"status": report["status"], "gate": "m0", "evidence": str(out.relative_to(ROOT))}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
