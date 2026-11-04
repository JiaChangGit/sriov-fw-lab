import copy
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from m3_controller import M3Controller


class Stream:
    def __init__(self): self.sent = []
    def sendall(self, value): self.sent.append(value)


class Device:
    toy_path = "/machine/peripheral/toy"
    def __init__(self):
        self.values = {}
        self.calls = []

    def fault(self, name, value):
        self.calls.append((name, value))
        self.values[name] = value
        if name.endswith("name"):
            prefix = name[:-4]
            self.values[prefix + "seen"] = 0
            self.values[prefix + "fired"] = 0
        return {"property": name, "value": value, "ack": len(self.calls)}

    def execute(self, command, args):
        assert command == "qom-get" and args["path"] == self.toy_path
        return self.values[args["property"]]


class Barriers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.c = M3Controller(Path(self.tmp.name), "T02")
        self.d, self.s = Device(), Stream()
        self.arm = {"event": "M3_BARRIER", "scenario": "T02", "phase": "request", "barrier": "arm-1",
                    "action": "failpoint-arm", "slot": "forward", "name": "provision.before_commit", "vf": 2, "hit": 1}

    def tearDown(self): self.tmp.cleanup()

    def test_order_hit_and_disarm(self):
        self.c(self.arm, self.d, self.s)
        self.assertEqual(self.d.calls, [("failpoint-vf", 2), ("failpoint-hit", 1), ("failpoint-name", "provision.before_commit")])
        self.assertEqual(self.s.sent, [b"M3_HOST_ACK arm-1\n"])
        self.d.values.update({"failpoint-seen": 1, "failpoint-fired": 1})
        check = {**self.arm, "barrier": "check-1", "action": "failpoint-check"}
        self.c(check, self.d, self.s)
        self.assertEqual(self.d.calls[-1], ("failpoint-name", ""))
        self.assertEqual(self.c.finish()["barriers"], 2)

    def test_no_hit_does_not_acknowledge(self):
        self.c(self.arm, self.d, self.s)
        with self.assertRaisesRegex(RuntimeError, "did not fire"):
            self.c({**self.arm, "barrier": "check", "action": "failpoint-check"}, self.d, self.s)
        self.assertEqual(len(self.s.sent), 1)
        with self.assertRaisesRegex(RuntimeError, "unchecked"):
            self.c.finish()

    def test_bad_scope_never_mutates(self):
        for patch in ({"vf": 8}, {"vf": True}, {"hit": 0}, {"hit": -1}, {"name": "unknown"},
                      {"slot": "any"}, {"scenario": "T03"}, {"barrier": "x\nquit"}, {"phase": "ack"}):
            with self.subTest(patch=patch):
                c = M3Controller(Path(self.tmp.name), "T02")
                with self.assertRaises(RuntimeError): c({**self.arm, **patch}, self.d, self.s)
                self.assertEqual(self.d.calls, [])
                self.assertEqual(self.s.sent, [])

    def test_duplicate_does_not_rearm(self):
        self.c(self.arm, self.d, self.s)
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            self.c(self.arm, self.d, self.s)
        self.assertEqual(len(self.d.calls), 3)

    def test_slots_independent(self):
        self.c(self.arm, self.d, self.s)
        self.c({**self.arm, "barrier": "arm-2", "slot": "unwind", "name": "provision.abort_release"}, self.d, self.s)
        self.assertEqual(set(self.c.armed), {"forward", "unwind"})

    def test_unknown_qmp_mutation_rejected(self):
        for name, value in (("path", "/tmp/data"), ("fw-hang-all", 1), ("fault-vf", 9), ("delay-cpl-ticks", -1)):
            c = M3Controller(Path(self.tmp.name), "T02")
            with self.assertRaises(RuntimeError):
                c({**self.arm, "action": "qmp-set", "property": name, "value": value}, self.d, self.s)
        self.assertEqual(self.d.calls, [])

    def test_failed_ack_is_not_recorded_as_pass(self):
        class BrokenStream:
            def sendall(self, value): raise BrokenPipeError("guest disconnected")
        with self.assertRaises(BrokenPipeError):
            self.c(self.arm, self.d, BrokenStream())
        self.assertEqual(self.c.records[-1]["status"], "FAIL")


if __name__ == "__main__": unittest.main()
