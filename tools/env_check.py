#!/usr/bin/env python3
"""Audit the build host without changing system configuration."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ('gcc', 'g++', 'make', 'git', 'python3', 'pkg-config', 'ninja',
            'meson', 'bison', 'flex', 'bc', 'cpio', 'xz', 'gzip', 'rsync',
            'nasm', 'iasl', 'lspci', 'clang', 'gcc-14')
OPTIONAL = ('lcov', 'sparse', 'cppcheck', 'shellcheck', 'ccache')
PACKAGES = ('build-essential', 'git', 'python3', 'python3-venv', 'pkg-config',
            'ninja-build', 'meson', 'libglib2.0-dev', 'libpixman-1-dev',
            'zlib1g-dev', 'libslirp-dev', 'libelf-dev', 'libssl-dev', 'bison',
            'flex', 'bc', 'cpio', 'xz-utils', 'gzip', 'rsync', 'nasm',
            'acpica-tools', 'uuid-dev', 'pciutils', 'clang', 'lcov', 'sparse',
            'cppcheck', 'shellcheck', 'libclang-rt-21-dev', 'gcc-14', 'python3-markdown-it')
OPTIONAL_PACKAGES = {'lcov', 'sparse', 'cppcheck', 'shellcheck'}


def command(argv: list[str], timeout: int = 30) -> dict:
    try:
        result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=timeout,
                                cwd=ROOT, check=False)
        return {'argv': argv, 'returncode': result.returncode,
                'status': 'PASS' if result.returncode == 0 else 'FAIL',
                'output': result.stdout}
    except FileNotFoundError as exc:
        return {'argv': argv, 'returncode': None, 'status': 'NOT_AVAILABLE',
                'output': str(exc)}
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or b''
        return {'argv': argv, 'returncode': None, 'status': 'TIMEOUT',
                'output': output.decode(errors='replace') if isinstance(output, bytes) else output}


def audit() -> dict:
    checks = {}
    for name, argv in {
        'os_release': ['cat', '/etc/os-release'],
        'kernel': ['uname', '-a'], 'cpu': ['lscpu'],
        'memory': ['free', '-b'], 'disk': ['df', '-B1', str(ROOT)],
        'mount': ['findmnt', '-T', str(ROOT), '-o', 'TARGET,FSTYPE,OPTIONS', '--json'],
        'python': [sys.executable, '--version'], 'gcc': ['gcc', '--version'],
        'gcc_target': ['gcc', '-dumpmachine'], 'clang': ['clang', '--version'],
        'clang_target': ['clang', '-dumpmachine'], 'kvm_device': ['ls', '-l', '/dev/kvm'],
        'identity': ['id'],
        'handbook_renderer': [sys.executable, '-c', 'from markdown_it import MarkdownIt; print(MarkdownIt.__name__)'],
    }.items():
        checks[name] = command(argv)
    failures = []
    if checks['handbook_renderer']['status'] != 'PASS':
        failures.append('Handbook renderer unavailable: install python3-markdown-it for this Python interpreter')
    try:
        mount = json.loads(checks['mount']['output'])['filesystems'][0]
        fstype = mount['fstype']
        if fstype in {'9p', 'drvfs', 'vboxsf', 'fuse.vmhgfs-fuse'}:
            failures.append('Repository is on a forbidden host/shared filesystem')
    except (KeyError, IndexError, ValueError):
        failures.append('Unable to establish repository filesystem')
        fstype = None
    with tempfile.TemporaryDirectory(prefix='.env-probe-', dir=ROOT) as tmp:
        probe = Path(tmp)
        (probe / 'Case').write_text('upper')
        (probe / 'case').write_text('lower')
        (probe / 'link').symlink_to('Case')
        executable = probe / 'execute'
        executable.write_text('#!/bin/sh\nexit 0\n')
        executable.chmod(0o700)
        semantics = {'case_sensitive': (probe / 'Case').read_text() == 'upper',
                     'symlink': (probe / 'link').is_symlink(),
                     'executable': command([str(executable)])['status'] == 'PASS'}
        if not all(semantics.values()):
            failures.append('Required Linux filesystem semantics unavailable')
    tools = {name: {'path': shutil.which(name),
                    'status': 'PASS' if shutil.which(name) else 'NOT_AVAILABLE',
                    'required': name in REQUIRED} for name in REQUIRED + OPTIONAL}
    failures.extend('Required tool missing: ' + n for n in REQUIRED if not tools[n]['path'])
    packages = command(['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${db:Status-Abbrev}\n', *PACKAGES])
    package_policy = command(['apt-cache', 'policy', *PACKAGES])
    installed = {}
    for line in packages['output'].splitlines():
        fields = line.split('\t')
        if len(fields) == 3:
            installed[fields[0].split(':')[0]] = fields[2].startswith('ii')
    failures.extend('Required package missing: ' + name for name in PACKAGES
                    if name not in OPTIONAL_PACKAGES and not installed.get(name, False))
    available = shutil.disk_usage(ROOT).free
    if available < 60 * 1024**3:
        failures.append('Less than 60 GiB available on build filesystem')
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        if key in {'MemTotal', 'MemAvailable', 'SwapTotal', 'SwapFree'}:
            memory[key] = int(value.split()[0]) * 1024
    return {'schema_version': 1, 'timestamp_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'status': 'BLOCKED' if failures else 'PASS', 'repo_realpath': str(ROOT),
            'filesystem': fstype, 'filesystem_semantics': semantics,
            'disk_free_bytes': available, 'memory_bytes': memory,
            'build_jobs_default': 2, 'guest_memory_mib': 1536,
            'guest_kasan_memory_mib': 2048, 'single_heavy_build': True,
            'tools': tools, 'packages': packages, 'package_policy': package_policy,
            'required_packages': sorted(set(PACKAGES) - OPTIONAL_PACKAGES),
            'optional_packages': sorted(OPTIONAL_PACKAGES),
            'checks': checks, 'blockers': failures,
            'kvm': {'status': 'NOT_RUN', 'reason': 'Device/flags are not a KVM initialization test; TCG remains mandatory',
                    'device_accessible': os.access('/dev/kvm', os.R_OK | os.W_OK)},
            'host_platform': platform.platform()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'artifacts/environment/wsl.json')
    args = parser.parse_args()
    report = audit()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'output': str(args.output),
                      'blockers': report['blockers']}))
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
