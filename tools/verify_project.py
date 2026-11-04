#!/usr/bin/env python3
"""Build and validate the supported M0-M3 project, offline after fetch-sources."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
from lab_common import ROOT, artifact_dir, checked_jobs, provenance, run_logged, sha256, write_json


# 驗收綁定來源內容；註解也計入 hash，因此改完註解後仍須建立新的驗收結果。
def input_hashes():
    paths = [ROOT / 'Makefile', ROOT / 'sources.lock.json']
    for folder in ('fwcore', 'device', 'include', 'guest', 'patches', 'tools', 'tests', 'uefi', 'spec'):
        paths.extend(p for p in (ROOT / folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    return {str(p.relative_to(ROOT)): sha256(p) for p in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    jobs = checked_jobs(args.jobs)
    out = artifact_dir('project')
    start = time.monotonic()
    # 串行執行大型建置與 VM，控制 16 GB 筆電的記憶體使用；任一步失敗就停止。
    steps = [
        ('environment', ['tools/env_check.py'], 120, None),
        ('m0', ['tools/verify_m0.py'], 2400, 'm0'),
        ('build-qemu', ['tools/build_qemu.py', '--jobs', str(jobs)], 18000, None),
        ('kernel-stress', ['tools/build_kernel.py', '--profile', 'stress', '--jobs', str(jobs)], 12000, None),
        ('kernel-kasan', ['tools/build_kernel.py', '--profile', 'kasan', '--jobs', str(jobs)], 12000, None),
        ('initramfs-stress', ['tools/build_initramfs.py', '--profile', 'stress', '--jobs', str(jobs)], 1200, None),
        ('initramfs-kasan', ['tools/build_initramfs.py', '--profile', 'kasan', '--jobs', str(jobs)], 1200, None),
        ('m3', ['tools/verify_m3.py'], 24000, 'm3'),
        ('validators', ['-m', 'unittest', 'discover', '-s', 'tests/scenarios', '-p', 'test_*.py', '-v'], 180, None),
        ('guest-native', ['tools/verify_guest_native.py'], 180, 'guest-native'),
    ]
    result = {**provenance(), 'schema_version': 1, 'gate': 'project', 'scope': 'M0-M3_T01-T10',
              'status': 'FAIL', 'accelerator': 'tcg', 'jobs': jobs,
              'build_started_empty': not (ROOT / 'build').exists() or not any((ROOT / 'build').iterdir()),
              'input_hashes': input_hashes(), 'steps': [{'id': name, 'status': 'NOT_RUN'} for name, *_ in steps]}
    try:
        for index, (name, arguments, timeout, pointer_name) in enumerate(steps):
            write_json(out / 'result.json', result)
            record = run_logged([sys.executable, *arguments], out, name, timeout=timeout)
            step = result['steps'][index]
            step.update(status='PASS', command=record, log_sha256=sha256(ROOT / record['log']))
            if pointer_name:
                pointer = json.loads((ROOT / f'artifacts/{pointer_name}-latest.json').read_text())
                path = ROOT / pointer['path'] / 'result.json'
                report = json.loads(path.read_text())
                if pointer['status'] != 'PASS' or report['status'] != 'PASS':
                    raise RuntimeError('Stage has no passing structured result: ' + name)
                step.update(path=str(path.relative_to(ROOT)), sha256=sha256(path))
        # 執行期間來源若改變，已跑的各步驟可能不是同一版本，不能接受為完整 PASS。
        if input_hashes() != result['input_hashes']:
            raise RuntimeError('Project inputs changed during verification')
        for step in result['steps']:
            if sha256(ROOT / step['command']['log']) != step['log_sha256']:
                raise RuntimeError('Stage log changed: ' + step['id'])
            if step.get('path') and sha256(ROOT / step['path']) != step['sha256']:
                raise RuntimeError('Stage result changed: ' + step['id'])
        result['status'] = 'PASS'
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
        if 'index' in locals(): result['steps'][index]['status'] = 'FAIL'
        print(error, flush=True)
    finally:
        result['elapsed_seconds'] = time.monotonic() - start
        write_json(out / 'result.json', result)
        write_json(ROOT / 'artifacts/project-latest.json', {'path': str(out.relative_to(ROOT)), 'status': result['status']})
    print(json.dumps({'status': result['status'], 'gate': 'project', 'evidence': str(out.relative_to(ROOT))}), flush=True)
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__': raise SystemExit(main())
