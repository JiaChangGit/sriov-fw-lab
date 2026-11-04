#!/usr/bin/env python3
"""Build the pinned, locally patched x86_64 QEMU without network downloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from lab_common import ROOT, artifact_dir, checked_jobs, provenance, resource_lock, run_logged, sha256, write_json


def fingerprint(paths):
    value = hashlib.sha256()
    for path in sorted(paths):
        value.update(str(path.relative_to(ROOT)).encode())
        value.update(path.read_bytes())
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    jobs = checked_jobs(args.jobs)
    log = artifact_dir("build-qemu")
    report = {**provenance(), "status": "FAIL", "jobs": jobs}
    source = ROOT / "downloads/qemu"
    work = ROOT / "build/qemu-src"
    output = ROOT / "build/qemu"
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    entry = lock["sources"]["qemu"]
    patch = ROOT / "patches/qemu/0001-toy-sriov-build.patch"
    inputs = [ROOT / "sources.lock.json", patch, Path(__file__).resolve()]
    for folder in ("device", "fwcore", "include"):
        inputs.extend(p for p in (ROOT / folder).rglob("*") if p.is_file())
    expected = fingerprint(inputs)
    try:
        with resource_lock():
            if not (source / ".git").exists():
                raise RuntimeError("Pinned QEMU missing; run make fetch-sources")
            actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
            if actual != entry["commit"]:
                raise RuntimeError("QEMU source commit differs from lock")
            if b"\r\n" in (source / "configure").read_bytes():
                raise RuntimeError("QEMU configure still contains CRLF")
            output.mkdir(parents=True, exist_ok=True)
            if not work.exists():
                run_logged(["git", "worktree", "prune"], log, "prune", cwd=source)
                run_logged(["git", "-c", "core.autocrlf=false", "worktree", "add", "--detach", str(work), actual], log, "worktree", cwd=source)
            if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=work, text=True).strip() != actual:
                raise RuntimeError("Existing QEMU worktree is from another commit; clean-build required")
            check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=work, capture_output=True)
            if check.returncode == 0:
                run_logged(["git", "apply", str(patch)], log, "patch", cwd=work)
            else:
                reverse = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)], cwd=work, capture_output=True)
                if reverse.returncode != 0:
                    raise RuntimeError("QEMU integration patch conflicts with existing worktree")
            target = work / "hw/misc/sriov-fw-lab"
            target.mkdir(exist_ok=True)
            for path in (ROOT / "device").iterdir():
                if path.is_file():
                    shutil.copy2(path, target / path.name)
            for folder in ("fwcore", "include"):
                shutil.copytree(ROOT / folder, target / folder, dirs_exist_ok=True)
            for name in ("berkeley-softfloat-3", "berkeley-testfloat-3", "keycodemapdb", "dtc"):
                cached = ROOT / "downloads/qemu-subprojects" / name
                if not cached.is_dir():
                    raise RuntimeError(f"Missing offline subproject {name}; run fetch-sources")
                shutil.copytree(cached, work / "subprojects" / name, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".git"))
                overlay = work / "subprojects/packagefiles" / name
                if overlay.is_dir():
                    # 等同 pinned wrap 的 patch_directory，離線複製不觸發 Meson 下載。
                    shutil.copytree(overlay, work / "subprojects" / name, dirs_exist_ok=True)
            # 由 QEMU 自己建立 repo-local pyvenv；--disable-download 禁止隱藏下載。
            run_logged([str(work / "configure"), "--target-list=x86_64-softmmu", "--python=/usr/bin/python3",
                        "--disable-download", "--disable-docs", "--disable-werror", "--disable-gtk",
                        "--disable-sdl", "--disable-opengl", "--disable-vnc", "--disable-curl",
                        "--disable-slirp"], log, "configure", cwd=output, timeout=300)
            run_logged(["ninja", "-j", str(jobs), "qemu-system-x86_64"], log, "compile", cwd=output, timeout=7200)
            run_logged([str(output / "qemu-system-x86_64"), "--version"], log, "version")
            run_logged([str(output / "qemu-system-x86_64"), "-device", "toy-sriov-pf,help"], log, "device-help")
            report.update(status="PASS", input_sha256=expected,
                          binary_sha256=sha256(output / "qemu-system-x86_64"),
                          source_commit=actual, integration_patch_sha256=sha256(patch))
            write_json(output / "stamp.json", report)
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(error)
    write_json(log / "result.json", report)
    print(json.dumps({"status": report["status"], "evidence": str(log.relative_to(ROOT))}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
