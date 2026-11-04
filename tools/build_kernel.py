#!/usr/bin/env python3
"""Build an offline minimal Linux guest and its out-of-tree PF module."""
import argparse
import json
import os
import shutil
import subprocess
from lab_common import ROOT, artifact_dir, checked_jobs, provenance, resource_lock, run_logged, sha256, write_json
from check_config import check
from kernel_source import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("stress", "kasan"), default="stress")
    parser.add_argument("--jobs", type=int, default=2)
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--configure-only", action="store_true")
    scope.add_argument("--kernel-only", action="store_true", help="Build the kernel while PF sources are under development; no full build stamp")
    args = parser.parse_args()
    jobs = checked_jobs(args.jobs)
    logs = artifact_dir("build-kernel")
    result = {**provenance(), "profile": args.profile, "status": "FAIL", "jobs": jobs}
    source = ROOT / "build/linux-src"
    output = ROOT / f"build/kernel-{args.profile}"
    output.mkdir(parents=True, exist_ok=True)
    combined = output / "requested.config"
    fragments = [ROOT / "guest/configs/base.config"]
    if args.profile != "base": fragments.append(ROOT / f"guest/configs/{args.profile}.config")
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    compiler = "gcc-14"
    command = ["make", "-C", str(source), f"O={output}", "ARCH=x86_64",
               f"CC={compiler}", f"HOSTCC={compiler}"]
    try:
        with resource_lock():
            source, source_inputs = prepare(lock, logs)
            result["source_inputs"] = source_inputs
            combined.write_text("\n".join(path.read_text() for path in fragments))
            (output / "stamp.json").unlink(missing_ok=True)
            commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
            if commit != lock["sources"]["linux"]["commit"]:
                raise RuntimeError("Kernel source does not match lock")
            compiler_version = subprocess.check_output([compiler, "--version"], text=True)
            compiler_target = subprocess.check_output([compiler, "-dumpmachine"], text=True)
            expected = lock["kernel_toolchain"]
            if compiler_version != expected["version"] or compiler_target != expected["target"]:
                raise RuntimeError("Kernel compiler changed from sources.lock.json")
            result.update(toolchain=expected, source_commit=commit,
                          builder_sha256=sha256(ROOT / "tools/build_kernel.py"),
                          requested_config_sha256=sha256(combined))
            run_logged(command + [f"KCONFIG_ALLCONFIG={combined}", "allnoconfig"], logs, "config", timeout=300)
            config_check = check(output / ".config", args.profile)
            write_json(logs / "config-check.json", config_check)
            if config_check["status"] != "PASS":
                raise RuntimeError("Final kernel configuration rejected: " + json.dumps(config_check))
            if args.configure_only:
                result.update(status="PASS", scope="configuration_only", config=config_check)
            else:
                run_logged(command + ["-j", str(jobs), "bzImage", "modules"], logs, "compile", timeout=10800)
                if args.kernel_only:
                    result.update(status="PASS", scope="kernel_only", config=config_check,
                                  bzimage_sha256=sha256(output / "arch/x86/boot/bzImage"),
                                  module_status="NOT_RUN")
                    write_json(output / "kernel-stamp.json", result)
                    write_json(logs / "result.json", result)
                    print(json.dumps({"status": result["status"], "scope": result["scope"], "evidence": str(logs.relative_to(ROOT))}))
                    return 0
                # driver 在 build 內編譯；不安裝至 WSL，也不污染 guest 原始碼。
                module = output / "toy-pf"
                shutil.copytree(ROOT / "guest/pf", module, dirs_exist_ok=True)
                shutil.copytree(ROOT / "include", output / "include-lab", dirs_exist_ok=True)
                run_logged(command + ["-j", str(jobs), f"M={module}", f"EXTRA_CFLAGS=-I{output / 'include-lab'}", "modules"], logs, "pf-module", timeout=600)
                result.update(status="PASS", scope="kernel_and_module", config=config_check,
                              source_commit=commit, bzimage_sha256=sha256(output / "arch/x86/boot/bzImage"),
                              module_sha256=sha256(module / "toy_pf.ko"))
                write_json(output / "stamp.json", result)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error)
    write_json(logs / "result.json", result)
    print(json.dumps({"status": result["status"], "evidence": str(logs.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
