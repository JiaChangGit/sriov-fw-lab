#!/usr/bin/env python3
"""Build pinned BusyBox and an offline, deterministic lab guest initramfs."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile

from lab_common import (ROOT, artifact_dir, checked_jobs, provenance,
                        resource_lock, run_logged, sha256, write_json)


# 僅開啟開機、診斷與管理節點所需的 applet，不連帶建置網路工具。
BUSYBOX_Y = (
    "BUSYBOX", "SHOW_USAGE", "FEATURE_VERBOSE_USAGE", "LONG_OPTS", "LFS",
    "FEATURE_DEVPTS", "FEATURE_INSTALLER", "INSTALL_APPLET_SYMLINKS", "SH_IS_ASH", "ASH",
    "ASH_OPTIMIZE_FOR_SIZE", "ASH_INTERNAL_GLOB", "ASH_ECHO", "ASH_PRINTF",
    "ASH_TEST", "ASH_CMDCMD", "FEATURE_SH_MATH", "FEATURE_SH_MATH_64",
    "CAT", "CHMOD", "CUT", "ECHO", "ENV", "FALSE", "TRUE", "GREP", "LS",
    "MKDIR", "MKNOD", "PRINTF", "READLINK", "FEATURE_READLINK_FOLLOW",
    "SLEEP", "SYNC", "TEST", "TEST1", "TEST2", "WC", "MOUNT",
    "FEATURE_MOUNT_FLAGS", "UMOUNT", "DMESG", "INSMOD", "RMMOD", "LSMOD", "FEATURE_CMDLINE_MODULE_OPTIONS",
    "POWEROFF", "HALT", "REBOOT",
)
BUSYBOX_CONFIG = "\n".join(f"CONFIG_{name}=y" for name in BUSYBOX_Y) + "\n" + (
    '# CONFIG_STATIC is not set\n# CONFIG_WERROR is not set\n'
    'CONFIG_EXTRA_CFLAGS="-std=gnu99"\nCONFIG_EXTRA_LDFLAGS=""\n'
)
REQUIRED_APPLETS = {"bin/sh", "bin/mount", "bin/mkdir", "bin/mknod", "bin/chmod",
                    "bin/cat", "bin/sleep", "bin/sync", "sbin/insmod", "sbin/poweroff"}


def guest_source_hashes():
    paths = [ROOT / "include/toy_regs.h", ROOT / "include/toy_uapi.h"]
    for directory in ("guest/common", "guest/admin", "guest/tenant"):
        paths.extend(path for path in (ROOT / directory).rglob("*")
                     if path.is_file() and (path.suffix in (".c", ".h") or path.name == "Makefile"))
    return {str(path.relative_to(ROOT)): sha256(path) for path in sorted(paths)}


def capture(argv, logs, name, *, cwd=ROOT, env=None):
    run_logged(argv, logs, name, cwd=cwd, timeout=120, env=env)
    return (logs / f"{name}.log").read_text().strip()


def normalized(path):
    return Path(os.path.normpath(str(path)))


class ElfClosure:
    """Copy ELF dependencies by reading metadata, never by running host ldd."""

    def __init__(self, rootfs, logs):
        self.rootfs = rootfs
        self.logs = logs
        self.copied = {}
        self.inspected = {}
        self.cache = {}
        cache = capture(["/sbin/ldconfig", "-p"], logs, "ldconfig")
        for line in cache.splitlines():
            match = re.match(r"\s*(\S+) \(([^)]+)\) => (/.+)", line)
            if match and "x86-64" in match[2]:
                self.cache.setdefault(match[1], []).append(Path(match[3]))

    def target(self, guest):
        guest = PurePosixPath(guest)
        if not guest.is_absolute() or ".." in guest.parts:
            raise RuntimeError(f"Unsafe archive destination: {guest}")
        path = self.rootfs / str(guest).lstrip("/")
        # 不讓已複製的絕對 symlink 導致 host 路徑遭寫入。
        for parent in path.parents:
            if parent == self.rootfs:
                break
            if parent.is_symlink():
                raise RuntimeError(f"Symlink in archive destination parent: {parent}")
        return path

    def copy_file(self, source, guest):
        source = normalized(source)
        guest = str(PurePosixPath(guest))
        key = (str(source), guest)
        if key in self.copied:
            return
        if not source.is_file() and not source.is_symlink():
            raise RuntimeError(f"Missing regular file: {source}")
        target = self.target(guest)
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_symlink():
            # 共享函式庫維持原有名稱與目標；自建 binary 則在 caller 解參照。
            link = os.readlink(source)
            if target.exists() or target.is_symlink():
                if not target.is_symlink() or os.readlink(target) != link:
                    raise RuntimeError(f"Conflicting archive destination: {guest}")
            else:
                target.symlink_to(link)
            self.copied[key] = {"source": str(source), "path": guest, "symlink": link}
            next_source = normalized(link if os.path.isabs(link) else source.parent / link)
            next_guest = os.path.normpath(link if os.path.isabs(link) else str(PurePosixPath(guest).parent / link))
            self.copy_file(next_source, next_guest)
        else:
            digest = sha256(source)
            if target.is_symlink() or (target.exists() and sha256(target) != digest):
                raise RuntimeError(f"Conflicting archive destination: {guest}")
            shutil.copyfile(source, target)
            target.chmod(0o755 if os.access(source, os.X_OK) else 0o644)
            self.copied[key] = {"source": str(source), "path": guest, "sha256": digest}

    def metadata(self, source):
        source = source.resolve(strict=True)
        if source in self.inspected:
            return self.inspected[source]
        with source.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                raise RuntimeError(f"Expected ELF binary: {source}")
        index = len(self.inspected)
        headers = capture(["readelf", "-W", "-h", "-l", "-d", str(source)],
                          self.logs, f"elf-{index:03d}")
        if not re.search(r"Class:\s+ELF64", headers) or not re.search(r"Machine:\s+Advanced Micro Devices X86-64", headers):
            raise RuntimeError(f"Expected x86_64 ELF: {source}")
        interp = re.search(r"Requesting program interpreter: ([^\]]+)", headers)
        needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", headers)
        search = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[([^\]]*)\]", headers)
        # 本機相依檔不得留下 host 專案絕對 RPATH，否則映像無法獨立執行。
        if search:
            raise RuntimeError(f"ELF RPATH/RUNPATH is not supported in this offline image: {source}: {search}")
        info = {"source": str(source), "sha256": sha256(source),
                "interpreter": interp[1] if interp else None, "needed": needed}
        self.inspected[source] = info
        return info

    def add(self, source, guest):
        self.copy_file(source, guest)
        pending = [Path(source)]
        visited = set()
        while pending:
            current = pending.pop().resolve(strict=True)
            if current in visited:
                continue
            visited.add(current)
            info = self.metadata(current)
            dependencies = []
            if info["interpreter"]:
                dependencies.append(Path(info["interpreter"]))
            for name in info["needed"]:
                if "/" in name:
                    raise RuntimeError(f"Unsupported DT_NEEDED path: {name}")
                candidates = self.cache.get(name, [])
                candidate = next((path for path in candidates if path.is_file()), None)
                if candidate is None:
                    raise RuntimeError(f"Unresolved ELF dependency {name} from {current}")
                dependencies.append(candidate)
            info["resolved"] = [str(path) for path in dependencies]
            for dependency in dependencies:
                self.copy_file(dependency, str(dependency))
                pending.append(dependency)

    def resolve_guest(self, guest):
        # 絕對連結相對於 guest /，不能使用 host Path.resolve()。
        guest = str(PurePosixPath(guest))
        for _ in range(40):
            target = self.target(guest)
            if not target.is_symlink():
                if not target.exists():
                    raise RuntimeError(f"Dangling guest path: {guest}")
                return target
            link = os.readlink(target)
            guest = os.path.normpath(link if os.path.isabs(link) else str(PurePosixPath(guest).parent / link))
        raise RuntimeError(f"Guest symlink loop: {guest}")

    def verify(self, binaries):
        for index, guest in enumerate(binaries):
            binary = self.resolve_guest(guest)
            info = self.metadata(binary)
            if info["interpreter"]:
                interpreter = self.resolve_guest(info["interpreter"])
                directories = sorted({str(self.target(record["path"]).parent)
                                      for record in self.copied.values()})
                output = capture([str(interpreter), "--inhibit-cache", "--library-path",
                                  ":".join(directories), "--list", str(binary)],
                                 self.logs, f"loader-check-{index:02d}")
                resolved = re.findall(r"=> (/\S+)", output)
                for path in resolved:
                    if not Path(path).is_relative_to(self.rootfs):
                        raise RuntimeError(f"Loader escaped staged image: {path}")
                if len(resolved) < len(info["needed"]):
                    raise RuntimeError(f"Incomplete loader dependency report: {guest}")
        for path in self.rootfs.rglob("*"):
            if path.is_symlink():
                self.resolve_guest("/" + str(path.relative_to(self.rootfs)))


def write_archive(rootfs, output, epoch):
    """Write Linux newc directly: stable inode/order/uid/gid/mtime, no root needed."""
    entries = [(".", stat.S_IFDIR | 0o755, b"", 0, 0)]
    manifest = []
    for path in sorted(rootfs.rglob("*"), key=lambda item: str(item.relative_to(rootfs))):
        name = path.relative_to(rootfs).as_posix()
        mode = path.lstat().st_mode
        if stat.S_ISREG(mode):
            data = path.read_bytes()
            manifest.append({"path": "/" + name, "mode": oct(mode), "sha256": sha256(path)})
        elif stat.S_ISLNK(mode):
            data = os.readlink(path).encode()
            manifest.append({"path": "/" + name, "mode": oct(mode), "symlink": os.readlink(path)})
        elif stat.S_ISDIR(mode):
            data = b""
        else:
            raise RuntimeError(f"Unexpected staging file type: {path}")
        entries.append((name, mode, data, 0, 0))
    # kernel 在啟動 /init 前就需要 /dev/console；devtmpfs mount 會取代它。
    for name, major, minor, permissions in (("dev/console", 5, 1, 0o600), ("dev/null", 1, 3, 0o666)):
        entries.append((name, stat.S_IFCHR | permissions, b"", major, minor))
        manifest.append({"path": "/" + name, "device": [major, minor], "mode": oct(stat.S_IFCHR | permissions)})
    entries.append(("TRAILER!!!", 0, b"", 0, 0))
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as stream:
        for inode, (name, mode, data, major, minor) in enumerate(entries, start=1):
            encoded = name.encode() + b"\0"
            values = (inode, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, epoch,
                      len(data), 0, 0, major, minor, len(encoded), 0)
            header = b"070701" + b"".join(f"{value:08x}".encode() for value in values)
            stream.write(header + encoded)
            stream.write(b"\0" * (-(len(header) + len(encoded)) % 4))
            stream.write(data)
            stream.write(b"\0" * (-len(data) % 4))
    temporary.replace(output)
    return manifest


def build(args, logs, result):
    jobs = checked_jobs(args.jobs)
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    pinned = lock["sources"]["busybox"]
    source = ROOT / pinned["path"]
    output = ROOT / "build/busybox"
    output.mkdir(parents=True, exist_ok=True)
    destination = ROOT / f"build/initramfs-{args.profile}"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "stamp.json").unlink(missing_ok=True)
    env = {**os.environ, "LC_ALL": "C", "TZ": "UTC", "KCONFIG_NOTIMESTAMP": "1"}
    commit = capture(["git", "rev-parse", "HEAD"], logs, "busybox-commit", cwd=source)
    if commit != pinned["commit"] or pinned.get("patches"):
        raise RuntimeError("BusyBox source differs from the pinned, unpatched baseline")
    dirty = capture(["git", "status", "--porcelain", "--untracked-files=no"], logs, "busybox-tree", cwd=source)
    if dirty:
        raise RuntimeError("Tracked BusyBox sources are dirty")
    epoch = int(capture(["git", "show", "-s", "--format=%ct", "HEAD"], logs, "source-epoch", cwd=source))
    env["SOURCE_DATE_EPOCH"] = str(epoch)
    compiler = capture(["gcc", "--version"], logs, "gcc-version") + "\n"
    triple = capture(["gcc", "-dumpmachine"], logs, "gcc-target") + "\n"
    expected = lock["compiler_provenance"]
    if compiler.strip() != expected["gcc_version"].strip() or triple.strip() != expected["gcc_target"].strip():
        raise RuntimeError("Native GCC changed since sources.lock.json; refresh toolchain provenance explicitly")
    requested = output / "requested.config"
    requested.write_text(BUSYBOX_CONFIG)
    env["KCONFIG_ALLCONFIG"] = str(requested)
    command = ["make", "-C", str(source), f"O={output}", "CC=gcc", "HOSTCC=gcc",
               "HOSTCFLAGS=-O2 -std=gnu99"]
    run_logged(command + ["allnoconfig"], logs, "busybox-configure", timeout=300, env=env)
    # BusyBox 的舊版 conf 在 -n 模式會覆蓋預設值；先建立全 n，再明確合併。
    overrides = {}
    for line in BUSYBOX_CONFIG.splitlines():
        match = re.match(r"(?:# )?(CONFIG_\w+)(?:=| is not set)", line)
        if match:
            overrides[match[1]] = line
    merged = []
    for line in (output / ".config").read_text().splitlines():
        match = re.match(r"(?:# )?(CONFIG_\w+)(?:=| is not set)", line)
        merged.append(overrides.pop(match[1], line) if match else line)
    (output / ".config").write_text("\n".join(merged + list(overrides.values())) + "\n")
    run_logged(command + ["oldconfig"], logs, "busybox-resolve-config", timeout=300, env=env)
    config = (output / ".config").read_text()
    missing = [name for name in BUSYBOX_Y if f"CONFIG_{name}=y\n" not in config]
    if missing or "CONFIG_STATIC=y\n" in config:
        raise RuntimeError(f"BusyBox final configuration rejected: missing={missing}")
    run_logged(command + [f"-j{jobs}", "busybox"], logs, "busybox-compile", timeout=1800, env=env)
    guest_inputs = guest_source_hashes()
    run_logged(["make", "-C", str(ROOT / "guest/tenant"), f"JOBS={jobs}"],
               logs, "guest-tools", timeout=300, env=env)
    if guest_source_hashes() != guest_inputs:
        raise RuntimeError("Guest sources changed during compilation; rebuild required")
    programs = {name: ROOT / "build/guest" / name for name in ("labd", "labctl", "tenant")}
    for name, path in programs.items():
        if not path.is_file():
            raise RuntimeError(f"Required M3 userspace program missing: {name}")
    kernel = ROOT / f"build/kernel-{args.profile}"
    module = kernel / "toy-pf/toy_pf.ko"
    for path in (kernel / "stamp.json", kernel / ".config", kernel / "arch/x86/boot/bzImage", module,
                 ROOT / "build/guest/m1-spike", ROOT / "build/guest/lab-exit", ROOT / "guest/init"):
        if not path.is_file():
            raise RuntimeError(f"Required build input missing: {path}")
    kernel_stamp = json.loads((kernel / "stamp.json").read_text())
    if kernel_stamp.get("status") != "PASS" or kernel_stamp.get("scope") != "kernel_and_module":
        raise RuntimeError("Kernel stamp does not report a completed kernel/module build")
    if kernel_stamp.get("source_commit") != lock["sources"]["linux"]["commit"]:
        raise RuntimeError("Kernel stamp source commit is stale")
    if kernel_stamp.get("module_sha256") != sha256(module) or kernel_stamp.get("bzimage_sha256") != sha256(kernel / "arch/x86/boot/bzImage"):
        raise RuntimeError("Kernel or PF module changed after its build stamp")
    lspci = shutil.which("lspci")
    if lspci is None:
        raise RuntimeError("Required pciutils lspci is not installed")
    applets = capture([str(output / "busybox"), "--list-full"], logs, "busybox-applets").splitlines()
    if REQUIRED_APPLETS - set(applets):
        raise RuntimeError(f"Required BusyBox applets absent: {sorted(REQUIRED_APPLETS - set(applets))}")
    with tempfile.TemporaryDirectory(prefix="rootfs-", dir=destination) as temporary:
        rootfs = Path(temporary)
        for directory in ("bin", "sbin", "usr/bin", "usr/sbin", "proc", "sys", "dev", "dev/pts", "run", "tmp", "etc", "lib/modules"):
            (rootfs / directory).mkdir(parents=True, exist_ok=True)
            (rootfs / directory).chmod(0o755)
        (rootfs / "tmp").chmod(0o1777)
        closure = ElfClosure(rootfs, logs)
        binaries = [(output / "busybox", "/bin/busybox"), (Path(lspci).resolve(), "/usr/bin/lspci"),
                    (ROOT / "build/guest/m1-spike", "/usr/bin/m1-spike"),
                    (ROOT / "build/guest/lab-exit", "/usr/bin/lab-exit")]
        binaries.extend((path, "/usr/bin/" + name) for name, path in programs.items())
        for source_binary, guest in binaries:
            closure.add(source_binary.resolve(), guest)
        for applet in applets:
            target = closure.target("/" + applet)
            if target.exists() or target.is_symlink():
                raise RuntimeError(f"BusyBox applet collision: {applet}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(os.path.relpath(rootfs / "bin/busybox", target.parent))
        closure.copy_file(ROOT / "guest/init", "/init")
        (rootfs / "init").chmod(0o755)
        closure.copy_file(module, "/lib/modules/toy_pf.ko")
        for ids in (Path("/usr/share/misc/pci.ids"), Path("/usr/share/hwdata/pci.ids")):
            if ids.exists():
                closure.copy_file(ids, str(ids))
        (rootfs / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n")
        (rootfs / "etc/group").write_text("root:x:0:\n")
        (rootfs / "etc/lab-profile").write_text(args.profile + "\n")
        closure.verify([guest for _, guest in binaries])
        run_logged([str(output / "busybox"), "sh", "-n", str(ROOT / "guest/init")], logs, "init-syntax", timeout=30, env=env)
        archive = destination / "initramfs.cpio.gz"
        manifest = write_archive(rootfs, archive, epoch)
        write_json(logs / "file-manifest.json", manifest)
        write_json(logs / "elf-closure.json", {"files": list(closure.copied.values()),
                                              "elf": list(closure.inspected.values())})
        packages = capture(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n"], logs, "installed-packages")
        if guest_source_hashes() != guest_inputs:
            raise RuntimeError("Guest sources changed while packaging; rebuild required")
        result.update(status="PASS", scope="busybox_and_lab_initramfs", jobs=jobs,
                      guest_source_hashes=guest_inputs,
                      guest_programs={name: {"path": str(path.relative_to(ROOT)), "sha256": sha256(path)}
                                      for name, path in programs.items()},
                      source_commit=commit, source_date_epoch=epoch,
                      input_hashes={"builder": sha256(Path(__file__)), "init": sha256(ROOT / "guest/init"),
                                    "busybox_config": sha256(output / ".config"),
                                    "kernel_config": sha256(kernel / ".config"),
                                    "kernel_stamp": sha256(kernel / "stamp.json"),
                                    "module": sha256(module),
                                    "m1_spike": sha256(ROOT / "build/guest/m1-spike"),
                                    "lab_exit": sha256(ROOT / "build/guest/lab-exit"),
                                    "installed_packages": hashlib.sha256(packages.encode()).hexdigest()},
                      archive=str(archive.relative_to(ROOT)), archive_sha256=sha256(archive),
                      busybox_sha256=sha256(output / "busybox"),
                      runtime_validation="NOT_RUN", evidence=str(logs.relative_to(ROOT)))
        write_json(destination / "stamp.json", result)
        write_json(output / "stamp.json", {"status": "PASS", "source_commit": commit,
                                           "config_sha256": sha256(output / ".config"),
                                           "binary_sha256": sha256(output / "busybox"),
                                           "gcc_version": compiler, "gcc_target": triple,
                                           "source_date_epoch": epoch,
                                           "sources_lock_sha256": sha256(ROOT / "sources.lock.json")})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("stress", "kasan"), default="stress")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    logs = artifact_dir("build-initramfs")
    result = {**provenance(), "status": "FAIL", "profile": args.profile}
    try:
        with resource_lock():
            build(args, logs, result)
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result.update(status="FAIL", error=str(error))
        print(error, flush=True)
    write_json(logs / "result.json", result)
    print(json.dumps({"status": result["status"], "evidence": str(logs.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
