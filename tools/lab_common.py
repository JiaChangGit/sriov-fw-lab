"""Shared provenance, bounded command execution and build serialization."""
from contextlib import contextmanager
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", dir=path.parent, delete=False, encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        temporary = Path(stream.name)
    temporary.replace(path)


def git_head():
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def provenance():
    lock = ROOT / "sources.lock.json"
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                           capture_output=True, text=True, check=True)
    return {
        "repo_commit": git_head(),
        "repo_dirty": bool(dirty.stdout),
        "sources_lock_sha256": sha256(lock) if lock.exists() else None,
        "spec_sha256": sha256(ROOT / "sriov_fw_lab_v3_0_complete_build_spec.html"),
        "collected_at": datetime.datetime.now(datetime.UTC).isoformat(),
    }


def artifact_dir(name):
    parent = ROOT / "artifacts" / name
    parent.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%SZ")
    return Path(tempfile.mkdtemp(prefix=stamp + "-", dir=parent))


def run_logged(argv, directory, name, *, cwd=ROOT, timeout=1800, env=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    log = directory / (name + ".log")
    start = time.monotonic()
    record = {"argv": list(map(str, argv)), "cwd": str(cwd), "timeout_seconds": timeout,
              "log": str(log.relative_to(ROOT))}
    print("RUN", " ".join(record["argv"]), flush=True)
    with log.open("wb") as stream:
        try:
            process = subprocess.Popen(record["argv"], cwd=cwd, stdout=stream,
                                       stderr=subprocess.STDOUT, env=env,
                                       start_new_session=True)
            process.wait(timeout=timeout)
            record.update(exit_code=process.returncode, timed_out=False)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            record.update(exit_code=None, timed_out=True)
    record["elapsed_seconds"] = time.monotonic() - start
    write_json(directory / (name + ".json"), record)
    if record["exit_code"] != 0:
        print(log.read_text(errors="replace")[-6000:], flush=True)
        raise RuntimeError(f"Command failed; evidence: {log}")
    return record


@contextmanager
def resource_lock():
    """Serialize upstream compilation and VM runs even across separate tools."""
    path = ROOT / "artifacts" / ".resource.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            yield
        finally:
            fcntl.flock(stream, fcntl.LOCK_UN)


def checked_jobs(value):
    jobs = int(value)
    if jobs not in (1, 2, 3):
        raise ValueError("JOBS must be 1, 2, or explicitly selected 3")
    if jobs == 3:
        available = next(int(line.split()[1]) * 1024 for line in
                         Path("/proc/meminfo").read_text().splitlines()
                         if line.startswith("MemAvailable:"))
        if available < 5 * 1024 ** 3:
            raise ValueError("JOBS=3 requires at least 5 GiB MemAvailable")
    return jobs
