#!/usr/bin/env python3
"""Run all mandatory M0 core evidence. Reduced budgets are explicitly NOT_RUN."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from build_core import binary_hashes, build, source_hashes
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json


def portability(output):
    issues = []
    for path in sorted((ROOT / 'fwcore').glob('*.[ch]')):
        source = path.read_text()
        if re.search(r'\b(malloc|calloc|realloc|free|alloca|pthread_\w+|sleep|usleep|mmap)\s*\(', source):
            issues.append(str(path.relative_to(ROOT)) + ': prohibited heap/OS call')
        for include in re.findall(r'^#include\s+<([^>]+)>', source, re.M):
            if include not in ('stdint.h', 'stddef.h', 'Uefi.h'):
                issues.append(str(path.relative_to(ROOT)) + ': prohibited include ' + include)
    records = []
    for obj in sorted((ROOT / 'build/core').glob('*.o')):
        result = subprocess.run(['nm', '-S', str(obj)], capture_output=True, text=True, check=True)
        records.append(dict(object=str(obj.relative_to(ROOT)), symbols=result.stdout))
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[-2] in tuple('BbCcDdGgSs'):
                issues.append(str(obj.name) + ': mutable global ' + parts[-1])
        if re.search(r'\bU\s+(malloc|calloc|realloc|free|alloca|pthread_\w+|mmap)\b', result.stdout):
            issues.append(obj.name + ': forbidden imported symbol')
    # 明確函式呼叫圖排除 recursion；HAL 間接呼叫的非重入約束另由 adapter 契約負責。
    functions = {}
    for path in sorted((ROOT / 'fwcore').glob('*.c')):
        text = path.read_text()
        pattern = re.compile(r'^\s*(?:static\s+)?(?:void|fw_status|fw_u\d+|fw_u8)\s+(\w+)\s*\([^;]*?\)\s*\{', re.M)
        for match in pattern.finditer(text):
            depth, cursor = 1, match.end()
            while depth:
                depth += (text[cursor] == '{') - (text[cursor] == '}')
                cursor += 1
            functions[match.group(1)] = set(re.findall(r'\b(\w+)\s*\(', text[match.end():cursor-1]))
    def visit(name, ancestors):
        if name in ancestors:
            issues.append('recursion: ' + ' -> '.join([*ancestors, name]))
            return
        for callee in functions[name] & functions.keys():
            visit(callee, [*ancestors, name])
    for name in functions:
        visit(name, [])
    report = dict(status='FAIL' if issues else 'PASS', issues=issues, objects=records,
                  direct_call_graph={key: sorted(value & functions.keys()) for key, value in functions.items()},
                  vla_check='GCC and Clang -Wvla -Werror',
                  limitations=['HAL adapters must not reenter the same fw_context from callbacks'])
    write_json(output / 'portability.json', report)
    if issues:
        raise RuntimeError('Core portability check failed: ' + '; '.join(issues))
    return report


def coverage(output):
    folder = ROOT / 'build/core/coverage'
    merged = {}
    for notes in sorted(folder.glob('*.gcno')):
        if notes.stem.endswith(('fake_hal', 'test_core', 'core_driver')):
            continue
        run_logged(['gcov', '--json-format', '--branch-probabilities', str(notes)], output, 'gcov-' + notes.stem, cwd=folder)
        report = folder / (notes.name + '.gcov.json.gz')
        if not report.exists():
            report = folder / (notes.stem + '.gcov.json.gz')
        data = json.loads(gzip.decompress(report.read_bytes()))
        for file in data['files']:
            source = Path(file['file']).resolve()
            if source.parent != ROOT / 'fwcore':
                continue
            counts = merged.setdefault(str(source.relative_to(ROOT)), {})
            for line in file['lines']:
                counts[line['line_number']] = max(counts.get(line['line_number'], 0), line['count'])
    if set(merged) != {str(p.relative_to(ROOT)) for p in (ROOT / 'fwcore').glob('*.c')}:
        raise RuntimeError('Coverage missing one or more core source files')
    total = sum(len(lines) for lines in merged.values())
    covered = sum(sum(count > 0 for count in lines.values()) for lines in merged.values())
    percent = 100 * covered / total
    summary = dict(status='PASS' if percent >= 90 else 'FAIL', covered_lines=covered, executable_lines=total,
                   line_coverage_percent=percent, files={name: dict(covered=sum(n>0 for n in counts.values()),
                   executable=len(counts), uncovered=[line for line, n in counts.items() if not n]) for name, counts in merged.items()})
    write_json(output / 'coverage.json', summary)
    if percent < 90:
        raise RuntimeError(f'Core line coverage {percent:.2f}% is below 90%')
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    parser.add_argument('--fuzz-seconds', type=int, default=300)
    parser.add_argument('--fuzz-runs', type=int, default=100000)
    parser.add_argument('--operations', type=int, default=100000)
    parser.add_argument('--no-build', action='store_true')
    args = parser.parse_args()
    output = args.output or artifact_dir('verify-core')
    output.mkdir(parents=True, exist_ok=True)
    result = dict(schema_version=1, gate='m0-core', status='NOT_RUN', provenance=provenance(),
                  seeds=[101,202,303,404], operations_per_seed=args.operations,
                  fuzz_runs_per_target=args.fuzz_runs, fuzz_exploration_seconds_per_target=args.fuzz_seconds,
                  cases=[], limitations=[])
    start = time.monotonic()
    try:
        if not args.no_build:
            build(output / 'build')
        stamp = json.loads((ROOT / 'build/core/build.json').read_text())
        if stamp['status'] != 'PASS' or stamp['inputs'] != source_hashes() or stamp.get('binary_hashes') != binary_hashes():
            raise RuntimeError('Build inputs changed; rebuild core instead of reusing stale artifacts')
        result.update(core_inputs=stamp['inputs'], core_build_sha256=sha256(ROOT / 'build/core/build.json'),
                      binary_hashes=stamp['binary_hashes'])
        portability(output)
        result['cases'].append(dict(id='I8-portability', status='PASS', evidence='portability.json'))
        for name in ('native', 'sanitize', 'coverage'):
            if name == 'coverage':
                for counter in (ROOT / 'build/core/coverage').glob('*.gcda'):
                    counter.unlink()
            environment = os.environ.copy()
            environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
            record = run_logged([str(ROOT / 'build/core' / name / 'unit')], output, name + '-unit', env=environment)
            if 'CORE_UNIT_RESULT=PASS' not in (output / (name+'-unit.log')).read_text():
                raise RuntimeError('Missing unit suite final result')
            result['cases'].append(dict(id=name+'-unit', status='PASS', evidence=record['log']))
        for name in ('sanitize', 'coverage'):
            record = run_logged([sys.executable, str(ROOT / 'tests/oracle/run_oracle.py'), '--binary',
                str(ROOT / 'build/core' / name / 'oracle'), '--output', str(output / (name+'-oracle')),
                '--operations', str(args.operations)], output, name + '-oracle', timeout=600)
            result['cases'].append(dict(id=name+'-oracle', status='PASS', evidence=record['log']))
        cov = coverage(output)
        result['core_line_coverage_percent'] = cov['line_coverage_percent']
        result['cases'].append(dict(id='coverage', status='PASS', evidence='coverage.json'))
        for target in ('mailbox_state', 'descriptor_scheduler'):
            corpus = output / 'corpus' / target
            corpus.mkdir(parents=True, exist_ok=True)
            (corpus / 'zero').write_bytes(bytes(64))
            (corpus / 'incremental').write_bytes(bytes(range(128)))
            binary = str(ROOT / 'build/core/fuzz' / target)
            base = [binary, str(corpus), '-max_len=512', '-timeout=5', '-rss_limit_mb=1024', '-seed=101',
                    '-artifact_prefix=' + str(corpus) + '/']
            run_logged([*base, '-runs=' + str(args.fuzz_runs)], output, target + '-runs', timeout=300)
            run_logged([*base, '-max_total_time=' + str(args.fuzz_seconds)], output, target + '-explore', timeout=args.fuzz_seconds + 90)
            result['cases'].append(dict(id='fuzz-'+target, status='PASS', evidence=[target+'-runs.log',target+'-explore.log']))
        if (source_hashes() != result['core_inputs'] or binary_hashes() != result['binary_hashes'] or
                sha256(ROOT / 'build/core/build.json') != result['core_build_sha256']):
            raise RuntimeError('Core inputs or binaries changed during verification')
        complete = args.operations >= 100000 and args.fuzz_runs >= 100000 and args.fuzz_seconds >= 300
        result['status'] = 'PASS' if complete else 'NOT_RUN'
        if not complete:
            result['limitations'].append('Reduced development budget; mandatory M0 fuzz/property gate NOT_RUN')
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        result['status'] = 'FAIL'
        result['error'] = str(error)
        print(str(error), file=sys.stderr, flush=True)
    finally:
        result['elapsed_seconds'] = time.monotonic() - start
        result['evidence_hashes'] = {str(p.relative_to(output)): sha256(p)
            for p in sorted(output.rglob('*')) if p.is_file() and p.suffix in ('.log', '.json')
            and p != output / 'result.json' and 'corpus' not in p.relative_to(output).parts}
        write_json(output / 'result.json', result)
        write_json(ROOT / 'artifacts/verify-core-latest.json', dict(path=str(output.relative_to(ROOT)), status=result['status']))
        print(json.dumps(dict(status=result['status'], gate='m0-core', evidence=str(output.relative_to(ROOT)))), flush=True)
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
