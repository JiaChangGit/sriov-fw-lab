#!/usr/bin/env python3
"""Build pinned OVMF SR-IOV variants and the portable-core UEFI probe offline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

from lab_common import ROOT, artifact_dir, checked_jobs, provenance, resource_lock, run_logged, sha256, write_json


def fingerprint():
    digest = hashlib.sha256()
    paths = [Path(__file__).resolve(), ROOT / "sources.lock.json", ROOT / "include/toy_regs.h"]
    for directory in ("uefi", "fwcore"):
        paths.extend(path for path in (ROOT / directory).rglob("*") if path.is_file())
    for path in sorted(paths):
        digest.update(str(path.relative_to(ROOT)).encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def git_head(source):
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()


def copy_if_changed(source, destination):
    """Keep generated configuration timestamps stable across unchanged builds."""
    if not destination.exists() or source.read_bytes() != destination.read_bytes():
        shutil.copy2(source, destination)


def write_if_changed(destination, content):
    encoded = content.encode("utf-8")
    if not destination.exists() or destination.read_bytes() != encoded:
        destination.write_bytes(encoded)


def export_submodules(source, work, pins, log):
    """Export immutable objects; compilation must never write into the source cache."""
    exported = []
    for pin in sorted(pins, key=lambda item: (item["path"].count("/"), item["path"])):
        relative = Path(pin["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise RuntimeError("Unsafe submodule path in source lock")
        cached, destination = source / relative, work / relative
        if git_head(cached) != pin["commit"]:
            raise RuntimeError(f"EDK2 submodule does not match lock: {relative}")
        destination.mkdir(parents=True, exist_ok=True)
        marker = destination / ".lab-export-commit"
        if marker.exists() and marker.read_text().strip() != pin["commit"]:
            raise RuntimeError(f"Stale isolated EDK2 submodule: {relative}; clean-build required")
        if not marker.exists():
            with tempfile.TemporaryFile() as stream:
                subprocess.run(["git", "archive", "--format=tar", pin["commit"]],
                               cwd=cached, stdout=stream, check=True)
                stream.seek(0)
                with tarfile.open(fileobj=stream) as archive:
                    archive.extractall(destination, filter="data")
            marker.write_text(pin["commit"] + "\n")
        exported.append({"path": str(relative), "commit": pin["commit"], "method": "git archive"})
    write_json(log / "submodules.json", exported)


def make_toolchain(directory, log):
    directory.mkdir(parents=True, exist_ok=True)
    # GCC5 是 EDK2 toolchain tag；實際編譯器版本明確固定為已安裝 GCC 14。
    mapping = {"gcc": "gcc-14", "gcc-ar": "gcc-ar-14", "gcc-nm": "gcc-nm-14",
               "gcc-ranlib": "gcc-ranlib-14", "ar": "ar", "ld": "ld",
               "objcopy": "objcopy", "objdump": "objdump", "nm": "nm", "ranlib": "ranlib"}
    records = {}
    for name, command in mapping.items():
        resolved = shutil.which(command)
        if resolved is None:
            raise RuntimeError(f"Missing explicit EDK2 tool: {command}")
        link = directory / name
        if link.is_symlink():
            if link.readlink() != Path(resolved):
                link.unlink()
                link.symlink_to(resolved)
        elif link.exists():
            raise RuntimeError(f"Refusing to overwrite non-symlink tool: {link}")
        else:
            link.symlink_to(resolved)
        records[name] = {"path": resolved, "sha256": sha256(resolved),
                         "version": subprocess.check_output([resolved, "--version"], text=True).splitlines()[0]}
    for command in ("g++", "nasm", "iasl"):
        resolved = shutil.which(command)
        if resolved is None:
            raise RuntimeError(f"Missing EDK2 host dependency: {command}")
        flag = "-v" if command == "iasl" else "--version"
        records[command] = {"path": resolved, "sha256": sha256(resolved),
                            "version": subprocess.check_output([resolved, flag], text=True).strip()}
    records["python"] = {"path": sys.executable, "version": sys.version}
    write_json(log / "toolchain.json", records)
    return records


def apply_patches(work, log, expected):
    records = []
    if {str(path.relative_to(ROOT)) for path in (ROOT / "uefi/patches").glob("*.patch")} != {entry["path"] for entry in expected}:
        raise RuntimeError("EDK2 patch inventory does not match source lock")
    for patch in sorted((ROOT / "uefi/patches").glob("*.patch")):
        entry = next(item for item in expected if item["path"] == str(patch.relative_to(ROOT)))
        if sha256(patch) != entry["sha256"]:
            raise RuntimeError(f"EDK2 patch does not match source lock: {patch.name}")
        target = work / entry["original_file"]
        if sha256(target) not in {entry["original_file_sha256"], entry["patched_file_sha256"]}:
            raise RuntimeError(f"EDK2 patch target has unexpected content: {target}")
        check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=work,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if check.returncode == 0:
            run_logged(["git", "apply", str(patch)], log, "patch-" + patch.stem, cwd=work)
        else:
            reverse = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                                     cwd=work, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if reverse.returncode != 0:
                raise RuntimeError(f"Cannot apply or verify existing EDK2 patch: {patch.name}: {check.stderr}")
        if sha256(target) != entry["patched_file_sha256"]:
            raise RuntimeError(f"EDK2 patch output does not match source lock: {target}")
        records.append(entry)
    write_json(log / "patches.json", records)
    return records


def check_pcd(output, enabled):
    headers = list(output.glob("X64/MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe/DEBUG/AutoGen.h"))
    if len(headers) != 1:
        raise RuntimeError("Missing unique built PciBusDxe AutoGen.h PCD evidence")
    text = headers[0].read_text()
    values = {}
    for name, expected_value in (("PcdSrIovSupport", enabled), ("PcdAriSupport", True),
                                 ("PcdSrIovSystemPageSize", True)):
        match = re.search(r"^#define\s+_PCD_VALUE_" + name + r"\s+(.+)$", text, re.M)
        if not match:
            raise RuntimeError(f"Built PciBusDxe has no fixed {name} value")
        token = match.group(1).strip().strip("()").upper().rstrip("UL")
        if token in {"TRUE", "FALSE"}:
            actual = int(token == "TRUE")
        elif re.fullmatch(r"0X[0-9A-F]+|[0-9]+", token):
            actual = int(token, 16 if token.startswith("0X") else 10)
        else:
            raise RuntimeError(f"Unsupported built {name} expression: {match.group(1)}")
        if actual != int(expected_value):
            raise RuntimeError(f"Built {name} is {match.group(1)}, expected {int(expected_value)}")
        values[name] = match.group(1).strip()
    return {"path": str(headers[0].relative_to(ROOT)), "sha256": sha256(headers[0]),
            "pcd_value": values["PcdSrIovSupport"], "values": values}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--prepare-only", action="store_true", help="Prepare offline sources; does not count as a build PASS")
    args = parser.parse_args()
    jobs = checked_jobs(args.jobs)
    log = artifact_dir("build-edk2")
    report = {**provenance(), "status": "FAIL", "jobs": jobs, "runtime_status": "NOT_RUN"}
    source = ROOT / "downloads/edk2"
    work = ROOT / "build/edk2-src"
    output = ROOT / "build/edk2"
    entry = json.loads((ROOT / "sources.lock.json").read_text())["sources"]["edk2"]
    try:
        with resource_lock():
            if git_head(source) != entry["commit"]:
                raise RuntimeError("EDK2 checkout does not match source lock")
            output.mkdir(parents=True, exist_ok=True)
            if not work.exists():
                run_logged(["git", "worktree", "prune"], log, "prune", cwd=source)
                run_logged(["git", "-c", "core.autocrlf=false", "worktree", "add", "--detach", str(work), entry["commit"]], log, "worktree", cwd=source)
            if git_head(work) != entry["commit"]:
                raise RuntimeError("Existing EDK2 worktree has a different commit; clean-build required")
            export_submodules(source, work, entry["submodules"], log)
            report["patches"] = apply_patches(work, log, entry.get("patches", []))
            package = work / "SriovFwPkg"
            shutil.copytree(ROOT / "uefi/SriovFwPkg", package, dirs_exist_ok=True)
            shutil.copytree(ROOT / "fwcore", package / "FwCore", dirs_exist_ok=True)
            (package / "Generated").mkdir(exist_ok=True)
            shutil.copy2(ROOT / "include/toy_regs.h", package / "Generated/toy_regs.h")
            conf = work / "Conf"
            conf.mkdir(exist_ok=True)
            for name in ("target", "tools_def", "build_rule"):
                copy_if_changed(work / f"BaseTools/Conf/{name}.template", conf / f"{name}.txt")
            toolchain = make_toolchain(output / "toolchain", log)
            env = os.environ.copy()
            # 釘選 EDK2 使用 hash(GUID) 選 stack cookie；固定 seed 避免無變更重編。
            env.update(WORKSPACE=str(work), EDK_TOOLS_PATH=str(work / "BaseTools"),
                       CONF_PATH=str(conf), PYTHON_COMMAND=sys.executable, PYTHONHASHSEED="0",
                       GCC5_BIN=str(output / "toolchain") + "/",
                       PATH=str(work / "BaseTools/BinWrappers/PosixLike") + ":" + env["PATH"])
            report.update(input_sha256=fingerprint(), source_commit=entry["commit"],
                          toolchain_tag="GCC5", toolchain=toolchain,
                          build_environment={"PYTHONHASHSEED": env["PYTHONHASHSEED"]})
            if args.prepare_only:
                report.update(status="NOT_RUN", preparation_status="PASS", scope="offline source preparation only")
            else:
                # 新建置若失敗，不可留下看似適用於新輸出的整體成功 stamp。
                (output / "stamp.json").unlink(missing_ok=True)
                run_logged(["make", "-C", str(work / "BaseTools"), "-j", str(jobs),
                            "CC=gcc-14", "CXX=g++"], log, "basetools", cwd=work, env=env, timeout=1800)
                common = [str(work / "BaseTools/BinWrappers/PosixLike/build"), "-a", "X64", "-b", "DEBUG", "-t", "GCC5", "-n", str(jobs)]
                run_logged(common + ["-p", "SriovFwPkg/SriovFwPkg.dsc", "-y", str(log / "probe-build-report.txt")],
                           log, "probe", cwd=work, env=env, timeout=1800)
                probe = work / "Build/SriovFwPkg/DEBUG_GCC5/X64/SriovProbe.efi"
                shutil.copy2(probe, output / "SriovProbe.efi")
                report["probe_sha256"] = sha256(output / "SriovProbe.efi")
                variants = {}
                for name, enabled in (("positive", True), ("negative", False)):
                    platform = work / "OvmfPkg" / f"LabOvmf-{name}.dsc"
                    original = (work / "OvmfPkg/OvmfPkgX64.dsc").read_text()
                    replacement = f"Build/LabOvmf-{name}"
                    modified, changes = re.subn(r"(?m)^(\s*OUTPUT_DIRECTORY\s*=\s*)Build/OvmfX64\s*$", lambda match: match[1] + replacement, original)
                    if changes != 1:
                        raise RuntimeError("Pinned OVMF output-directory declaration changed")
                    write_if_changed(platform, modified)
                    run_logged(common + ["-p", str(platform.relative_to(work)),
                               "--pcd", f"gEfiMdeModulePkgTokenSpaceGuid.PcdSrIovSupport={'TRUE' if enabled else 'FALSE'}",
                               "--pcd", "gEfiMdeModulePkgTokenSpaceGuid.PcdAriSupport=TRUE",
                               "--pcd", "gEfiMdeModulePkgTokenSpaceGuid.PcdSrIovSystemPageSize=0x1",
                               "-D", "FD_SIZE_4MB=TRUE", "-D", "BUILD_SHELL=FALSE",
                               "-D", "NETWORK_ISCSI_ENABLE=FALSE", "-D", "CC_MEASUREMENT_ENABLE=FALSE",
                               "-y", str(log / f"{name}-build-report.txt")],
                               log, name, cwd=work, env=env, timeout=7200)
                    variant_build = work / replacement / "DEBUG_GCC5"
                    pcd = check_pcd(variant_build, enabled)
                    destination = output / name
                    destination.mkdir(exist_ok=True)
                    images = {}
                    for image in ("OVMF_CODE.fd", "OVMF_VARS.fd"):
                        shutil.copy2(variant_build / "FV" / image, destination / image)
                        images[image] = {"sha256": sha256(destination / image), "bytes": (destination / image).stat().st_size}
                    if sum(image["bytes"] for image in images.values()) != 4 * 1024 * 1024:
                        raise RuntimeError("OVMF CODE/VARS sizes do not form the requested 4 MiB flash image")
                    variant = {"status": "PASS", "runtime_status": "NOT_RUN", "source_commit": entry["commit"],
                               "input_sha256": report["input_sha256"], "toolchain_tag": "GCC5", "pcd": pcd,
                               "images": images, "dsc_sha256": sha256(platform), "sriov_support": enabled}
                    write_json(destination / "stamp.json", variant)
                    variants[name] = variant
                report.update(status="PASS", variants=variants)
                write_json(output / "stamp.json", report)
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        report["error"] = str(error)
        print(error)
    write_json(log / "result.json", report)
    write_json(ROOT / "artifacts/build-edk2-latest.json", {"status": report["status"], "evidence": str(log.relative_to(ROOT))})
    print(json.dumps({"status": report["status"], "evidence": str(log.relative_to(ROOT))}))
    return 0 if report["status"] == "PASS" or report.get("preparation_status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
