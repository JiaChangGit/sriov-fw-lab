"""The release entry point must reject interrupted stages and changed sources."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import verify_project as gate


class ProjectGate(unittest.TestCase):
    def exercise(self, failure=None, changed=False, result_failed=False):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            out = root / 'artifacts/project/run'
            out.mkdir(parents=True)
            for name in ('Makefile', 'sources.lock.json'): (root / name).write_text('original')
            def run(argv, directory, name, **kwargs):
                log = out / (name + '.log')
                log.write_text('aggregate mock: no real build executed\n')
                if name == failure: raise RuntimeError('injected failure')
                if name in ('m0', 'm3', 'guest-native'):
                    result = root / f'artifacts/{name}/run/result.json'
                    result.parent.mkdir(parents=True)
                    result.write_text(json.dumps({'status': 'FAIL' if result_failed and name == 'm3' else 'PASS'}))
                    (root / f'artifacts/{name}-latest.json').write_text(json.dumps(
                        {'path': str(result.parent.relative_to(root)), 'status': 'PASS'}))
                if changed and name == 'validators': (root / 'Makefile').write_text('changed')
                return {'log': str(log.relative_to(root)), 'exit_code': 0}
            with patch.object(gate, 'ROOT', root), patch.object(gate, 'artifact_dir', return_value=out), \
                 patch.object(gate, 'provenance', return_value={}), patch.object(gate, 'run_logged', side_effect=run), \
                 patch.object(sys, 'argv', ['verify_project.py']):
                rc = gate.main()
            return rc, json.loads((out / 'result.json').read_text())

    def test_complete_control_path(self):
        rc, result = self.exercise()
        self.assertEqual(rc, 0)
        self.assertTrue(result['build_started_empty'])
        self.assertEqual(len(result['steps']), 10)

    def test_kernel_failure_stops_later_stages(self):
        rc, result = self.exercise(failure='kernel-kasan')
        self.assertEqual(rc, 1)
        self.assertEqual(result['steps'][-1]['status'], 'NOT_RUN')

    def test_source_change_rejected(self):
        rc, result = self.exercise(changed=True)
        self.assertEqual(rc, 1)
        self.assertIn('inputs changed', result['error'])

    def test_exit_zero_with_failed_result_rejected(self):
        rc, result = self.exercise(result_failed=True)
        self.assertEqual(rc, 1)
        self.assertIn('structured result', result['error'])


if __name__ == '__main__': unittest.main()
