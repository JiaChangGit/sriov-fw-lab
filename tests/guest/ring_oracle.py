"""Compare compiled tenant cursors to an unbounded FIFO and stepping cursors."""
from collections import deque
import json
import random
import subprocess
import sys


def main(binary):
    inputs, expected = [], []
    for entries in (16, 32, 256):
        for seed in (101, 202, 303, 404):
            randomizer = random.Random(seed)
            head = tail = (1 << 32) - 2 * entries
            hindex = tindex = 0
            hphase = tphase = 1
            queue = deque()
            outstanding_ids = set()
            for _ in range(10000):
                if randomizer.randrange(2) and len(queue) < entries:
                    assert tail & 65535 not in outstanding_ids
                    outstanding_ids.add(tail & 65535)
                    queue.append(tail)
                    tail += 1
                    tindex += 1
                    if tindex == entries:
                        tindex = 0
                        tphase = 1 - tphase
                elif queue:
                    assert queue.popleft() == head
                    outstanding_ids.remove(head & 65535)
                    head += 1
                    hindex += 1
                    if hindex == entries:
                        hindex = 0
                        hphase = 1 - hphase
                inputs.append(f"{tail & 0xffffffff} {head & 0xffffffff} {entries}\n")
                expected.append((len(queue), tindex, tphase, hindex, hphase))
            assert head > 1 << 32 and tail > 1 << 32
    result = subprocess.run([binary], input="".join(inputs), text=True, capture_output=True, timeout=30, check=True)
    rows = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    assert rows == expected, "Compiled tenant cursor differs from independent FIFO"
    print(json.dumps({"status": "PASS", "entries": [16, 32, 256], "seeds": [101, 202, 303, 404],
                      "operations_per_variant_seed": 10000, "comparisons": len(rows),
                      "u32_wrap_crossed": True, "duplicate_outstanding_ids": 0}))


if __name__ == "__main__":
    main(sys.argv[1])
