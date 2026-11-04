#!/usr/bin/env python3
"""Explicit, logged Ubuntu dependency installation (run as root in WSL)."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = """build-essential git python3 python3-venv pkg-config ninja-build meson
libglib2.0-dev libpixman-1-dev zlib1g-dev libslirp-dev libelf-dev libssl-dev
bison flex bc cpio xz-utils gzip rsync nasm acpica-tools uuid-dev pciutils
clang lcov sparse cppcheck shellcheck libclang-rt-21-dev gcc-14 python3-markdown-it""".split()


def main():
    if os.geteuid() != 0:
        raise SystemExit("Run explicitly as root inside Ubuntu; no sudo prompt is used.")
    directory = ROOT / "artifacts" / "dependencies" / datetime.datetime.now(datetime.UTC).strftime("%Y%m%dT%H%M%SZ")
    directory.mkdir(parents=True)
    commands = [
        ["apt-cache", "policy", *PACKAGES],
        ["apt-get", "update"],
        ["apt-get", "install", "-y", "--no-install-recommends", *PACKAGES],
        ["dpkg-query", "-W", "-f=${Package}\t${Version}\t${Architecture}\n", *PACKAGES],
    ]
    results = []
    for index, argv in enumerate(commands):
        print("Running:", " ".join(argv), flush=True)
        log = directory / f"{index:02d}.log"
        with log.open("wb") as output:
            result = subprocess.run(argv, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                    env={**os.environ, "DEBIAN_FRONTEND": "noninteractive"}, timeout=1200)
        results.append({"argv": argv, "exit_code": result.returncode, "log": str(log.relative_to(ROOT))})
        (directory / "result.json").write_text(json.dumps(results, indent=2) + "\n")
        if result.returncode:
            print(log.read_text(errors="replace")[-6000:])
            return result.returncode
    print("Dependency installation complete:", directory)
    return 0


if __name__ == "__main__":
    sys.exit(main())
