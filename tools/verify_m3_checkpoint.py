#!/usr/bin/env python3
"""Run a partial development checkpoint, or the complete M3 gate when requested."""
import json
import subprocess
import sys
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json


def main(complete=False):
    label = "m3" if complete else "m3-checkpoint"
    out = artifact_dir(label)
    steps = [
        ("stress-archive", ["tools/build_initramfs.py", "--profile", "stress", "--jobs", "2"], 600),
        ("m1-kasan", ["tools/run_vm.py", "--gate", "m1", "--profile", "kasan"], 1000),
        ("m1-stress", ["tools/run_vm.py", "--gate", "m1", "--profile", "stress"], 600),
        ("m2", ["tools/verify_m2.py", "--jobs", "2"], 20000),
        ("t01", ["tools/verify_t01.py"], 2600),
        ("t02", ["tools/verify_t02.py"], 3100),
        ("t03", ["tools/verify_t03.py"], 3200),
        ("t04", ["tools/verify_t04.py"], 3100),
        ("t05", ["tools/verify_t05.py"], 6100),
        ("t06", ["tools/verify_t06.py"], 1100),
        ("t09", ["tools/verify_t09.py"], 1100),
        ("t10", ["tools/run_vm.py", "--gate", "m3-case", "--profile", "kasan", "--case", "T10"], 1000),
        ("validators", ["-m", "unittest", "discover", "-s", "tests/scenarios", "-p", "test_m3*.py", "-v"], 120),
    ]
    if complete:
        steps.insert(0, ("m0", ["tools/verify_m0.py", "--reuse-core"], 1800))
        index = next(i for i, row in enumerate(steps) if row[0] == "t09")
        steps[index:index] = [("t07", ["tools/verify_recovery.py", "--case", "T07"], 1100),
                              ("t08", ["tools/verify_recovery.py", "--case", "T08"], 1100)]
    result = {**provenance(), "scope": "T01_T02_T03_T04_T05_T06_T09_T10_regression_checkpoint", "status": "FAIL",
              "m3_status": "NOT_RUN", "remaining_m3_cases": ["T07", "T08"],
              "steps": [{"id": name, "status": "NOT_RUN"} for name, _, _ in steps]}
    if complete:
        result.update(schema_version=1, gate="m3", profile="kasan", accelerator="tcg", seed=101,
                      scope="complete_T01_T10_with_current_M0_M1_M2", remaining_m3_cases=[],
                      cases=[{"id": f"T{i:02}", "status": "NOT_RUN"} for i in range(1,11)],
                      limitations=["T05 strict WRITE-only uses the explicit pinned toy-only guest IOMMU patch; stock Linux behavior differs.",
                                   "Software model and TCG; no physical-device performance claim."])
    def inputs():
        paths = [ROOT / "sources.lock.json", ROOT / "tests/scenarios.json", ROOT / "Makefile"]
        for folder in ("fwcore", "device", "include", "guest", "patches", "tools", "tests/scenarios"):
            paths.extend(p for p in (ROOT / folder).rglob("*") if p.is_file() and "__pycache__" not in p.parts)
        return {str(p.relative_to(ROOT)): sha256(p) for p in sorted(set(paths))}
    result["input_hashes"] = inputs()
    try:
        for index, (name, arguments, timeout) in enumerate(steps):
            write_json(out / "result.json", result)
            record = run_logged([sys.executable, *arguments], out, name, timeout=timeout)
            result["steps"][index].update(status="PASS", command=record, log_sha256=sha256(ROOT / record["log"]))
            if name in ("m1-kasan", "m1-stress", "t10"):
                pointer_name = "m3-case" if name == "t10" else "m1"
            elif name in ("m0", "m2", "t01", "t02", "t03", "t04", "t05", "t06", "t07", "t08", "t09"): pointer_name = name
            else: pointer_name = None
            if pointer_name:
                pointer = json.loads((ROOT / f"artifacts/{pointer_name}-latest.json").read_text())
                path = ROOT / pointer["path"] / "result.json"
                if pointer["status"] != "PASS" or json.loads(path.read_text())["status"] != "PASS":
                    raise RuntimeError("Stage exited successfully without a passing structured result: " + name)
                result["steps"][index].update(path=str(path.relative_to(ROOT)), sha256=sha256(path))
            if complete and name.startswith("t") and name[1:].isdigit():
                next(c for c in result["cases"] if c["id"] == name.upper()).update(
                    status="PASS", path=result["steps"][index]["path"], sha256=result["steps"][index]["sha256"])
        if inputs() != result["input_hashes"]:
            raise RuntimeError("Gate inputs changed during execution")
        for step in result["steps"]:
            if step.get("path") and sha256(ROOT / step["path"]) != step["sha256"]:
                raise RuntimeError("Stage result changed during execution: " + step["id"])
        result["status"] = "PASS"
        if complete:
            if any(c["status"] != "PASS" for c in result["cases"]):
                raise RuntimeError("Required M3 scenario missing")
            result["m3_status"] = "PASS"
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        result["status"] = "FAIL"
        if "index" in locals():
            result["steps"][index]["status"] = "FAIL"
            if complete:
                for case in result["cases"]:
                    if case["id"].lower() == result["steps"][index]["id"]:
                        case["status"] = "FAIL"
        print(error, flush=True)
    write_json(out / "result.json", result)
    write_json(ROOT / ("artifacts/" + label + "-latest.json"), {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "m3_status": result["m3_status"], "evidence": str(out.relative_to(ROOT))}), flush=True)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
