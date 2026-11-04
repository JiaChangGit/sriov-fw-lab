"""Reject an old execution result even if a newer build stamp matches sources."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from verify_m0 import validate_core_reuse


class CoreReuse(unittest.TestCase):
    def setUp(self):
        self.inputs = {'fwcore/fsm.c': 'current'}
        self.binaries = {'build/core/native/unit': 'binary'}
        self.stamp = {'status': 'PASS', 'inputs': self.inputs, 'binary_hashes': self.binaries}
        self.core = {'core_inputs': self.inputs, 'binary_hashes': self.binaries, 'core_build_sha256': 'stamp'}

    def check(self, core=None, stamp=None, binaries=None):
        validate_core_reuse(core or self.core, stamp or self.stamp, 'stamp', self.inputs, binaries or self.binaries)

    def test_matching_execution(self):
        self.check()

    def test_new_build_cannot_reuse_old_execution(self):
        core = deepcopy(self.core)
        core['core_inputs']['fwcore/fsm.c'] = 'old source'
        with self.assertRaises(RuntimeError): self.check(core=core)

    def test_old_binary_rejected(self):
        core = deepcopy(self.core)
        core['binary_hashes']['build/core/native/unit'] = 'old binary'
        with self.assertRaises(RuntimeError): self.check(core=core)

    def test_failed_build_rejected(self):
        stamp = {**self.stamp, 'status': 'FAIL'}
        with self.assertRaises(RuntimeError): self.check(stamp=stamp)

    def test_replaced_build_stamp_rejected(self):
        with self.assertRaises(RuntimeError): self.check(core={**self.core, 'core_build_sha256': 'old stamp'})

    def test_binary_modified_after_build_rejected(self):
        with self.assertRaises(RuntimeError): self.check(binaries={'build/core/native/unit': 'tampered'})

    def test_legacy_result_without_execution_binding_rejected(self):
        with self.assertRaises(RuntimeError): self.check(core={'status': 'PASS'})


if __name__ == '__main__': unittest.main()
