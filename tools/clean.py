#!/usr/bin/env python3
"""Remove only explicitly named project build/cache directories."""
import argparse
from pathlib import Path
import shutil
from lab_common import resource_lock

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {"build": "build", "cache": "downloads/wheels"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=TARGETS)
    args = parser.parse_args()
    lexical = ROOT / TARGETS[args.kind]
    resolved = lexical.resolve()
    expected = ROOT.resolve() / TARGETS[args.kind]
    if resolved != expected or not resolved.is_relative_to(ROOT.resolve()) or resolved == ROOT:
        raise SystemExit("Refusing cleanup: target escapes its exact project location")
    if lexical.is_symlink():
        raise SystemExit("Refusing cleanup of a symbolic link")
    with resource_lock():
        if resolved.exists():
            print("Removing", resolved)
            shutil.rmtree(resolved)
        else:
            print("Already absent", resolved)


if __name__ == "__main__":
    main()
