#!/usr/bin/env python3
"""Resolve immutable upstream tags, fetch them, and record real source provenance."""
from __future__ import annotations

import argparse
import configparser
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    'qemu': ('https://gitlab.com/qemu-project/qemu.git', 'v9.2.0'),
    'linux': ('https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git', 'v6.12'),
    'edk2': ('https://github.com/tianocore/edk2.git', 'edk2-stable202411'),
    'busybox': ('https://git.busybox.net/busybox', '1_36_1'),
}
LOG = ROOT / 'artifacts/sources'
OFFICIAL_MIRRORS = {'linux': 'https://github.com/torvalds/linux.git'}


def pinned_patches(name, existing):
    records = {}
    for record in existing:
        path = (ROOT / record['path']).resolve(strict=True)
        if not path.is_relative_to(ROOT):
            raise RuntimeError('Locked patch escapes repository')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != record['sha256']:
            raise RuntimeError('Locked patch changed: ' + record['path'])
        records[record['path']] = dict(record)
    directories = [ROOT / 'patches' / name]
    if name == 'edk2':
        directories.append(ROOT / 'uefi/patches')
    for directory in directories:
        for path in sorted(directory.glob('**/*.patch')):
            relative = str(path.relative_to(ROOT))
            records.setdefault(relative, {'path': relative,
                                          'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    return [records[key] for key in sorted(records)]


def run(argv: list[str], name: str, cwd: Path = ROOT, timeout: int = 900) -> str:
    LOG.mkdir(parents=True, exist_ok=True)
    stamp = time.time_ns()
    path = LOG / f'{name}-{stamp}.log'
    start = time.monotonic()
    timed_out = False
    with path.open('w') as output:
        output.write(json.dumps({'argv': argv, 'cwd': str(cwd)}) + '\n')
        output.flush()
        try:
            environment = {**os.environ, 'GIT_TERMINAL_PROMPT': '0'}
            # Git 的設定只覆寫子程序，不修改使用者的全域設定。
            count = int(environment.get('GIT_CONFIG_COUNT', '0'))
            environment.update({'GIT_CONFIG_COUNT': str(count + 1),
                                f'GIT_CONFIG_KEY_{count}': 'core.autocrlf',
                                f'GIT_CONFIG_VALUE_{count}': 'false'})
            process = subprocess.Popen(argv, cwd=cwd, text=True, stdout=output,
                                       stderr=subprocess.STDOUT, start_new_session=True,
                                       env=environment)
            try:
                returncode = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                returncode = 124
                timed_out = True
        except OSError as exc:
            output.write(str(exc) + '\n')
            returncode = 127
        output.write('\n' + json.dumps({'returncode': returncode, 'timeout': timed_out,
                                 'elapsed_seconds': round(time.monotonic() - start, 3)}) + '\n')
    content = path.read_text().split('\n', 1)[1].rsplit('\n', 2)[0]
    if returncode:
        raise RuntimeError(f'{name}: exit {returncode}; evidence {path.relative_to(ROOT)}')
    return content


def git(cwd: Path, *args: str) -> str:
    return run(['git', '-c', 'core.autocrlf=false', *args], cwd.name + '-' + args[0], cwd)


def normalize_checkout(path: Path) -> int:
    """Restore LF where a pristine checkout was converted by host Git settings."""
    changed = 0
    refresh = []
    # checkout-index 會採用 stat cache；直接校正已證明僅換行不同的檔案。
    for item in git(path, 'ls-files', '--eol', '-z').split('\0'):
        if not item:
            continue
        metadata, filename = item.split('\t', 1)
        fields = metadata.split()
        if fields[0] != 'i/lf' or fields[1] not in {'w/lf', 'w/crlf'} or 'eol=crlf' in metadata:
            continue
        candidate = path / filename
        if candidate.is_symlink() or not candidate.resolve().is_relative_to(path.resolve()):
            raise RuntimeError(f'Unexpected tracked path during LF restoration: {candidate}')
        if fields[1] == 'w/crlf':
            content = candidate.read_bytes()
            candidate.write_bytes(content.replace(b'\r\n', b'\n'))
            changed += 1
        refresh.append(filename)
    if refresh:
        pathspec = LOG / f'{path.name}-lf-pathspec-{time.time_ns()}.bin'
        pathspec.write_bytes(b'\0'.join(name.encode() for name in refresh) + b'\0')
        git(path, 'add', '--renormalize', '--pathspec-from-file=' + str(pathspec), '--pathspec-file-nul')
        git(path, 'diff', '--cached', '--exit-code', 'HEAD')
    if git(path, 'status', '--porcelain', '--untracked-files=all').strip():
        raise RuntimeError(f'Checkout differs from pinned source after LF restoration: {path}')
    return changed


def pristine(path: Path) -> None:
    dirty = run(['git', '-c', 'core.autocrlf=true', 'status', '--porcelain', '--untracked-files=all'],
                path.name + '-pristine-check', cwd=path)
    if not dirty.strip():
        return
    # stat cache 可能仍記錄 CRLF 長度；以內容及 staged diff 二次確認。
    run(['git', '-c', 'core.autocrlf=true', 'diff', '--exit-code', 'HEAD'],
        path.name + '-pristine-content', cwd=path)
    git(path, 'diff', '--cached', '--exit-code', 'HEAD')
    if git(path, 'ls-files', '--others', '--exclude-standard').strip():
        raise RuntimeError(f'Refusing untracked files in source checkout: {path}')


def save(lock: dict) -> None:
    target = ROOT / 'sources.lock.json'
    temporary = target.with_suffix('.tmp')
    temporary.write_text(json.dumps(lock, indent=2) + '\n')
    temporary.replace(target)


def provenance() -> dict:
    info = {}
    for name, argv in {'gcc_version': ['gcc', '--version'], 'gcc_target': ['gcc', '-dumpmachine'],
                       'clang_version': ['clang', '--version'], 'clang_target': ['clang', '-dumpmachine'],
                       'python': [sys.executable, '--version'], 'meson': ['meson', '--version'],
                       'ninja': ['ninja', '--version']}.items():
        try:
            info[name] = run(argv, name, timeout=30)
        except RuntimeError as exc:
            info[name] = {'status': 'NOT_AVAILABLE', 'evidence': str(exc)}
    return info


def fetch_qemu_wraps(lock: dict) -> None:
    entry = lock['sources']['qemu']
    upstream = ROOT / entry['path']
    resolved = []
    for name in ('berkeley-softfloat-3', 'berkeley-testfloat-3', 'keycodemapdb', 'dtc'):
        wrap_path = upstream / 'subprojects' / (name + '.wrap')
        parser = configparser.ConfigParser()
        parser.read(wrap_path)
        url = parser['wrap-git']['url']
        revision = parser['wrap-git']['revision']
        if len(revision) != 40:
            raise RuntimeError(f'Unpinned QEMU wrap: {name}')
        path = ROOT / 'downloads/qemu-subprojects' / name
        path.mkdir(parents=True, exist_ok=True)
        if not (path / '.git').exists():
            git(path, 'init')
            git(path, 'remote', 'add', 'origin', url)
        if git(path, 'remote', 'get-url', 'origin').strip() != url:
            raise RuntimeError(f'Unexpected QEMU wrap origin: {path}')
        if git(path, 'status', '--porcelain').strip():
            raise RuntimeError(f'Dirty QEMU wrap checkout: {path}')
        git(path, 'config', '--local', 'core.autocrlf', 'false')
        try:
            actual = git(path, 'rev-parse', '--verify', 'HEAD').strip()
        except RuntimeError:
            actual = None
        if actual != revision:
            git(path, 'fetch', '--depth=1', 'origin', revision)
            git(path, 'checkout', '--detach', revision)
        if git(path, 'rev-parse', 'HEAD').strip() != revision:
            raise RuntimeError(f'QEMU wrap commit mismatch: {name}')
        normalize_checkout(path)
        resolved.append({'name': name, 'url': url, 'commit': revision,
                         'path': str(path.relative_to(ROOT)),
                         'tree': git(path, 'rev-parse', 'HEAD^{tree}').strip(),
                         'wrap_sha256': hashlib.sha256(wrap_path.read_bytes()).hexdigest(),
                         'packagefiles': [{'path': str(p.relative_to(upstream)),
                                           'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                                          for p in sorted((upstream / 'subprojects/packagefiles' / name).rglob('*'))
                                          if p.is_file()]})
        print(f'qemu wrap {name}: fetched {revision}', flush=True)
    entry['meson_wrap_sources'] = resolved
    lock['python_wheels'] = [{'path': str(p.relative_to(ROOT)),
                              'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
                              'source_commit': entry['commit'], 'installation_status': 'NOT_RUN'}
                             for p in sorted((upstream / 'python/wheels').glob('*.whl'))]
    lock['python_wheel_note'] = 'Bundled QEMU wheels hashed. Actual venv installation must be recorded by build manifest; no system pip changes.'
    save(lock)


def resolve(lock: dict, names: list[str]) -> None:
    for name in names:
        url, tag = SOURCES[name]
        fetched_url = url
        try:
            refs = run(['git', 'ls-remote', '--tags', url, f'refs/tags/{tag}', f'refs/tags/{tag}^{{}}'],
                       name + '-resolve', timeout=60)
        except RuntimeError:
            if name not in OFFICIAL_MIRRORS:
                raise
            fetched_url = OFFICIAL_MIRRORS[name]
            refs = run(['git', 'ls-remote', '--tags', fetched_url, f'refs/tags/{tag}', f'refs/tags/{tag}^{{}}'],
                       name + '-mirror-resolve', timeout=60)
        parsed = dict(line.split()[::-1] for line in refs.splitlines() if line.strip())
        tag_oid = parsed.get(f'refs/tags/{tag}')
        commit = parsed.get(f'refs/tags/{tag}^{{}}', tag_oid)
        if not commit or len(commit) != 40:
            raise RuntimeError(f'No valid full commit resolved for {name}:{tag}')
        existing = lock['sources'].get(name, {})
        if existing.get('commit') not in (None, commit):
            raise RuntimeError(f'Upstream tag moved for {name}; refusing to rewrite lock')
        lock['sources'][name] = {**existing, 'url': url, 'canonical_url': url,
                                  'fetch_url': existing.get('fetch_url', fetched_url),
                                  'tag': tag, 'tag_object': tag_oid,
                                  'commit': commit, 'source_status': existing.get('source_status', 'RESOLVED'),
                                  'compatibility_status': existing.get('compatibility_status', 'NOT_RUN')}
        save(lock)
        print(f'{name}: resolved {commit}', flush=True)


def fetch(lock: dict, names: list[str]) -> None:
    for name in names:
        entry = lock['sources'][name]
        path = ROOT / 'downloads' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not (path / '.git').exists():
            if path.exists() and any(path.iterdir()):
                raise RuntimeError(f'Refusing nonempty non-Git source directory: {path}')
            path.mkdir(exist_ok=True)
            git(path, 'init')
            git(path, 'remote', 'add', 'origin', entry['url'])
        if git(path, 'remote', 'get-url', 'origin').strip() != entry['url']:
            raise RuntimeError(f'Unexpected remote: {path}')
        # 僅在確認沒有其他修改後，修復 Windows 全域 autocrlf 造成的換行。
        pristine(path)
        git(path, 'config', '--local', 'core.autocrlf', 'false')
        try:
            actual = git(path, 'rev-parse', '--verify', 'HEAD').strip()
        except RuntimeError:
            actual = None
        if actual != entry['commit']:
            if git(path, 'status', '--porcelain').strip():
                raise RuntimeError(f'Refusing to replace dirty source checkout: {path}')
            try:
                git(path, 'fetch', '--depth=1', entry.get('fetch_url', entry['url']),
                    f'refs/tags/{entry["tag"]}:refs/tags/{entry["tag"]}')
            except RuntimeError:
                if name not in OFFICIAL_MIRRORS or entry.get('fetch_url') == OFFICIAL_MIRRORS[name]:
                    raise
                entry['fetch_url'] = OFFICIAL_MIRRORS[name]
                git(path, 'fetch', '--depth=1', entry['fetch_url'],
                    f'refs/tags/{entry["tag"]}:refs/tags/{entry["tag"]}')
            git(path, 'checkout', '--detach', entry['commit'])
        git(path, 'checkout-index', '-a', '-f')
        normalized_count = normalize_checkout(path)
        actual = git(path, 'rev-parse', 'HEAD').strip()
        if actual != entry['commit']:
            raise RuntimeError(f'Commit verification failed: {name}')
        entry['tree'] = git(path, 'rev-parse', 'HEAD^{tree}').strip()
        if (path / '.gitmodules').exists():
            git(path, 'submodule', 'sync', '--recursive')
            git(path, 'submodule', 'update', '--init', '--recursive', '--depth=1', '--jobs=2')
            submodules = []
            for line in git(path, 'submodule', 'status', '--recursive').splitlines():
                if not line or line[0] != ' ':
                    raise RuntimeError(f'Uninitialized or mismatched submodule: {line}')
                parts = line.strip().split()
                subpath = path / parts[1]
                pristine(subpath)
                git(subpath, 'config', '--local', 'core.autocrlf', 'false')
                git(subpath, 'checkout-index', '-a', '-f')
                normalized_count += normalize_checkout(subpath)
                submodules.append({'path': parts[1], 'commit': parts[0]})
            entry['submodules'] = submodules
            entry['gitmodules_sha256'] = hashlib.sha256((path / '.gitmodules').read_bytes()).hexdigest()
        else:
            entry['submodules'] = []
        tracked = git(path, 'ls-tree', '-r', '--full-tree', 'HEAD')
        entry['tracked_tree_manifest_sha256'] = hashlib.sha256(tracked.encode()).hexdigest()
        entry['path'] = str(path.relative_to(ROOT))
        entry['source_status'] = 'FETCHED'
        entry['checkout_line_endings'] = 'core.autocrlf=false; tracked source files rechecked from immutable Git index'
        entry['lf_restored_files_last_fetch'] = normalized_count
        entry['patches'] = pinned_patches(name, entry.get('patches', []))
        save(lock)
        print(f'{name}: fetched and verified {actual}; {len(entry["submodules"])} submodules', flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resolve-only', action='store_true')
    parser.add_argument('--component', action='append', choices=SOURCES)
    args = parser.parse_args()
    names = args.component or list(SOURCES)
    lock_path = ROOT / 'sources.lock.json'
    lock = json.loads(lock_path.read_text()) if lock_path.exists() else {
        'schema_version': 1, 'sources': {}, 'candidate_matrix_status': 'NOT_RUN',
        'generated_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
    lock['compiler_provenance'] = provenance()
    lock.setdefault('python_wheels', [])
    lock.setdefault('python_wheel_note', 'No pip wheels installed by source fetch; build records required if venv wheels are installed.')
    try:
        resolve(lock, names)
        if not args.resolve_only:
            fetch(lock, names)
            if 'qemu' in names:
                fetch_qemu_wraps(lock)
    except (RuntimeError, OSError, ValueError) as exc:
        LOG.mkdir(parents=True, exist_ok=True)
        failure = {'status': 'BLOCKED', 'error': str(exc),
                   'timestamp_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
        (LOG / 'failure.json').write_text(json.dumps(failure, indent=2) + '\n')
        print(json.dumps(failure), file=sys.stderr)
        return 1
    print(json.dumps({'status': 'RESOLVED' if args.resolve_only else 'FETCHED',
                      'lock': str(lock_path)}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
