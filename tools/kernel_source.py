"""Apply locked guest-kernel test hooks in a separate, inspectable worktree."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from lab_common import ROOT, run_logged, sha256


def prepare(lock, logs):
    entry = lock["sources"]["linux"]
    upstream, work = ROOT / entry["path"], ROOT / "build/linux-src"
    if not work.exists():
        run_logged(["git", "worktree", "prune"], logs, "source-prune", cwd=upstream)
        run_logged(["git", "-c", "core.autocrlf=false", "worktree", "add", "--detach", str(work), entry["commit"]],
                   logs, "source-worktree", cwd=upstream, timeout=300)
    if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=work, text=True).strip() != entry["commit"]:
        raise RuntimeError("Kernel worktree source commit differs from lock")
    for index, record in enumerate(entry["patches"]):
        patch = ROOT / record["path"]
        if sha256(patch) != record["sha256"]:
            raise RuntimeError("Locked kernel patch changed: " + record["path"])
        forward = subprocess.run(["git", "apply", "--check", str(patch)], cwd=work, capture_output=True)
        if forward.returncode == 0:
            run_logged(["git", "apply", str(patch)], logs, f"source-patch-{index}", cwd=work)
        elif subprocess.run(["git", "apply", "--reverse", "--check", str(patch)], cwd=work, capture_output=True).returncode:
            raise RuntimeError("Kernel test patch conflicts with worktree: " + forward.stderr.decode(errors="replace"))
    inputs = source_inputs(lock)
    (logs / "source-diff.patch").write_bytes(subprocess.check_output(["git", "diff", "HEAD"], cwd=work))
    return work, inputs


def source_inputs(lock=None):
    lock = lock or json.loads((ROOT / "sources.lock.json").read_text())
    work = ROOT / "build/linux-src"
    if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=work, text=True).strip() != lock["sources"]["linux"]["commit"]:
        raise RuntimeError("Kernel worktree source commit differs from lock")
    patches = lock["sources"]["linux"]["patches"]
    if not patches:
        raise RuntimeError("Guest-kernel test patch is not pinned")
    allowed = {"drivers/pci/iov.c", "drivers/iommu/intel/iommu.c"}
    dirty = set(subprocess.check_output(["git", "diff", "--name-only", "HEAD"], cwd=work, text=True).splitlines())
    if dirty != allowed or subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard"], cwd=work).strip():
        raise RuntimeError("Unexpected kernel worktree modifications")
    for record in patches:
        if sha256(ROOT / record["path"]) != record["sha256"]:
            raise RuntimeError("Kernel patch hash mismatch")
    # 獨立 index 從 pinned HEAD 套 patch；反向 apply 成功不足以排除同檔的額外修改。
    # 此 index 不碰使用者工作樹及正常 index，也不建立或重設分支。
    with tempfile.TemporaryDirectory(prefix="toy-kernel-index-") as directory:
        env={**os.environ,"GIT_INDEX_FILE":str(Path(directory)/"index")}
        subprocess.run(["git","read-tree","HEAD"],cwd=work,env=env,check=True,capture_output=True)
        for record in patches:
            subprocess.run(["git","apply","--cached",str(ROOT/record["path"])],cwd=work,env=env,check=True,capture_output=True)
        expected=set(subprocess.check_output(["git","diff","--cached","--name-only","HEAD"],cwd=work,env=env,text=True).splitlines())
        if expected != allowed:
            raise RuntimeError("Locked kernel patches change an unexpected file set")
        for name in sorted(expected):
            content=subprocess.check_output(["git","show",":"+name],cwd=work,env=env)
            if (work/name).is_symlink() or (work/name).read_bytes()!=content:
                raise RuntimeError("Kernel worktree differs from exact pinned patches: "+name)
    return {"patches": patches, "patched_files": {name: sha256(work / name) for name in sorted(allowed)},
            "source_helper_sha256": sha256(ROOT / "tools/kernel_source.py")}
