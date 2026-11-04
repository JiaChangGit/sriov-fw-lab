#!/usr/bin/env python3
"""Build the portable core, native tests, coverage, sanitizers and libFuzzer."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json

FLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wvla', '-Wshadow',
         '-Wstrict-prototypes', '-pedantic', '-ffreestanding', '-fno-builtin',
         '-I' + str(ROOT / 'fwcore'), '-I' + str(ROOT / 'tests/unit')]


def source_hashes():
    paths = sorted((ROOT / 'fwcore').glob('*.[ch]'))
    for folder in ['tests/unit', 'tests/oracle', 'tests/fuzz']:
        paths += sorted(p for p in (ROOT / folder).glob('*') if p.suffix in ('.c', '.h', '.py'))
    paths += [Path(__file__), ROOT / 'tools/verify_core.py']
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.exists()}


def binary_hashes():
    paths = [ROOT / 'build/core/libfwcore.a']
    paths.extend(ROOT / 'build/core' / mode / name
                 for mode in ('native', 'sanitize', 'coverage') for name in ('unit', 'oracle'))
    paths.extend(ROOT / 'build/core/fuzz' / name for name in ('mailbox_state', 'descriptor_scheduler'))
    paths.extend(sorted((ROOT / 'build/core').glob('*.o')))
    return {str(path.relative_to(ROOT)): sha256(path) for path in paths}


def build(output=None):
    logs = Path(output) if output else artifact_dir('build-core')
    destination = ROOT / 'build/core'
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'build.json').unlink(missing_ok=True)
    initial_inputs = source_hashes()
    sources = list(map(str, sorted((ROOT / 'fwcore').glob('*.c'))))
    fake = str(ROOT / 'tests/unit/fake_hal.c')
    unit = str(ROOT / 'tests/unit/test_core.c')
    oracle = str(ROOT / 'tests/oracle/core_driver.c')
    compilers = {'gcc': shutil.which('gcc'), 'clang': shutil.which('clang')}
    if any(value is None for value in compilers.values()):
        raise RuntimeError('BLOCKED: GCC and Clang are required for M0')
    records = []
    records.append(run_logged([compilers['gcc'], '--version'], logs, 'gcc-version'))
    records.append(run_logged([compilers['clang'], '--version'], logs, 'clang-version'))
    objects = []
    for source in sources:
        obj = destination / (Path(source).stem + '.o')
        records.append(run_logged([compilers['gcc'], *FLAGS, '-O2', '-c', source, '-o', str(obj)], logs, 'object-' + obj.stem))
        objects.append(str(obj))
    records.append(run_logged(['ar', 'rcs', str(destination / 'libfwcore.a'), *objects], logs, 'archive'))
    for name, flags, compiler in [('native', ['-O2'], 'gcc'),
                                  ('sanitize', ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'], 'clang'),
                                  ('coverage', ['-O0', '-g', '--coverage'], 'gcc')]:
        directory = destination / name
        directory.mkdir(exist_ok=True)
        for main, binary in [(unit, 'unit'), (oracle, 'oracle')]:
            records.append(run_logged([compilers[compiler], *FLAGS, *flags, *sources, fake, main,
                                       '-o', str(directory / binary)], logs, name + '-' + binary, cwd=directory))
    fuzz = destination / 'fuzz'
    fuzz.mkdir(exist_ok=True)
    for target in ('mailbox_state', 'descriptor_scheduler'):
        records.append(run_logged([compilers['clang'], *FLAGS, '-O1', '-g', '-fno-omit-frame-pointer',
                                   '-fsanitize=fuzzer,address,undefined', '-fno-sanitize-recover=all',
                                   *sources, fake, str(ROOT / 'tests/fuzz' / (target + '.c')),
                                   '-o', str(fuzz / target)], logs, 'fuzz-' + target))
    if initial_inputs != source_hashes():
        raise RuntimeError('Core inputs changed during build')
    stamp = dict(schema_version=1, status='PASS', scope='core_build_only', provenance=provenance(),
                 inputs=initial_inputs, binary_hashes=binary_hashes(), records=records)
    write_json(destination / 'build.json', stamp)
    write_json(logs / 'result.json', stamp)
    print(json.dumps(dict(status='PASS', gate='build-core', evidence=str(logs.relative_to(ROOT)))), flush=True)
    return logs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        build(args.output)
    except (RuntimeError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
