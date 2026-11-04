#!/usr/bin/env python3
"""Run native core and an independently authored Python model on identical input."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import time

from model import EDGES, Model


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def operation(rng, m):
    i = rng.randrange(8)
    v, h = m.v[i], m.h[i]
    pick = rng.randrange(100)
    if pick < 2:
        return 'H', [rng.choice([0, 4, 5])]
    if pick < 4:
        return 'X', [rng.randrange(32)]
    if pick < 7:
        return 'B', []
    if pick < 19:
        legal = [event for state, event in EDGES if state == v['state']]
        event = rng.choice(legal) if rng.random() < .7 else rng.randrange(16)
        return 'E', [i, event]
    if pick < 28:
        return 'C', [i, rng.choice([0, 1, 4, 8, 12, 16, 32, 64, 65])]
    if pick < 36:
        return 'F', [i, rng.randrange(2)]
    if pick < 42 and v['state'] in (2, 5) and h['fence'] and not h['kill']:
        return 'Q', [i]
    if pick < 48:
        return 'A', [i, h['epoch'] - (rng.random() < .2), h['ready']]
    if pick < 55:
        return 'R', [i, rng.choice([0, 1, 4, 8, 12, 16, 32, 64, 65])]
    if pick < 58:
        return 'G', []
    if pick < 63:
        return 'T', [m.tick + rng.choice([1, 99, 100, 101, 1000])]
    if pick < 67:
        return 'P', []
    if pick < 70:
        return 'O', [i, int(rng.random() < .96)]
    if pick < 87:
        return 'S', [rng.choice([0, 1, 4, 8, 16, 24, 32, 63, 64, 128, 0xffffffff]) for _ in range(8)]
    if m.cache_valid and rng.random() < .15:
        r = list(m.cached)
        if rng.random() < .3:
            r[4] ^= 1
        return 'M', r
    seq = m.last + 1
    if rng.random() < .1:
        seq = max(0, seq - 2)
    op = rng.choice(list(range(1, 10)) + [0x20, 0x24, 99])
    if not m.negotiated and rng.random() < .65:
        op = 1
    args = [0, 0, 0, 0]
    target = 65535 if op in (1, 9) else i
    if op == 1:
        args = [rng.choice([1, 1, 1, 2]), 0, 0, 0]
    elif op in (3, 8):
        args[0] = rng.choice([0, 4, 8, 12, 16, 65])
    elif op == 6:
        args = [h['epoch'] & 0xffffffff, h['epoch'] >> 32, h['ready'], 0]
    if rng.random() < .03:
        args[3] = 1
    return 'M', [m.session, seq, op, target, *args]


def run_trace(binary, directory, name, commands):
    directory.mkdir(parents=True, exist_ok=True)
    inp, expected, actual = [directory / f'{name}.{suffix}' for suffix in ('input', 'expected', 'actual')]
    model = Model()
    count = 0
    kinds = Counter()
    statuses = Counter()
    start = time.monotonic()
    with inp.open('w') as ins, expected.open('w') as outs:
        for op, args in commands(model):
            values = model.step(op, args)
            ins.write(op + (' ' + ' '.join(map(str, args)) if args else '') + '\n')
            outs.write(' '.join(map(str, values)) + '\n')
            count += 1
            kinds[op] += 1
            statuses[values[0]] += 1
    with inp.open('rb') as ins, actual.open('wb') as outs, (directory / f'{name}.stderr.log').open('wb') as err:
        process = subprocess.run([str(binary)], stdin=ins, stdout=outs, stderr=err, timeout=300, check=False)
    if process.returncode:
        raise RuntimeError(f'{name}: native exit={process.returncode}')
    with expected.open() as reference, actual.open() as observed:
        for line_no, (left, right) in enumerate(zip(reference, observed, strict=True), 1):
            if left != right:
                diff = dict(operation=line_no, expected=left.strip(), actual=right.strip())
                (directory / f'{name}.failure.json').write_text(json.dumps(diff, indent=2))
                raise AssertionError(f'{name}: mismatch operation {line_no}; inspect {name}.failure.json')
    result = dict(name=name, status='PASS', operations=count, kinds=dict(kinds), status_counts=dict(statuses),
                  input_sha256=sha(inp), expected_sha256=sha(expected), actual_sha256=sha(actual),
                  elapsed_seconds=time.monotonic()-start, exit_code=process.returncode)
    (directory / f'{name}.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    return result


def qos_commands(model):
    for reserves in ([8, 8, 8, 8], [4, 8, 12, 16]):
        yield 'G', []
        for i, reserve in enumerate(reserves):
            yield 'C', [i, reserve]
            yield 'E', [i, 1]
            yield 'Q', [i]
            yield 'A', [i, model.h[i]['epoch'], model.h[i]['ready']]
        for _ in range(1000):
            yield 'S', [1000] * 4 + [0] * 4
        normalized = [model.v[i]['serviced'] / reserves[i] for i in range(4)]
        jain = sum(normalized) ** 2 / (4 * sum(n * n for n in normalized))
        assert jain >= .99
        print(json.dumps(dict(case='normalized_jain', reserves=reserves, rounds=1000, jain=jain)), flush=True)
    yield 'G', []
    for i in range(4):
        yield 'C', [i, 8]
        yield 'E', [i, 1]
        yield 'Q', [i]
        yield 'A', [i, model.h[i]['epoch'], model.h[i]['ready']]
    for _ in range(1000):
        yield 'S', [1000, 8, 8, 8, 0, 0, 0, 0]
    assert [model.v[i]['serviced'] for i in range(4)] == [40000, 8000, 8000, 8000]
    yield 'F', [0, 0]
    for _ in range(100):
        yield 'S', [1000] * 4 + [0] * 4


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--operations', type=int, default=100000)
    p.add_argument('--seeds', type=int, nargs='+', default=[101, 202, 303, 404])
    a = p.parse_args()
    results = []
    for seed in a.seeds:
        def commands(model, seed=seed):
            rng = random.Random(seed)
            for _ in range(a.operations):
                yield operation(rng, model)
        results.append(run_trace(a.binary, a.output, f'seed-{seed}', commands))
    results.append(run_trace(a.binary, a.output, 'qos', qos_commands))
    summary = dict(status='PASS', seeds=a.seeds, operations_per_seed=a.operations, results=results,
                   independent_oracle_sha256=sha(Path(__file__).with_name('model.py')))
    (a.output / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
