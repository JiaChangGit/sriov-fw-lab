"""Reject corrupted acceptance evidence using reduced fixtures from actual VM runs."""
import copy
import json
import gzip
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from validate_m3 import validate_t01, validate_t02_provision, validate_t02_owner, validate_t02_pci, validate_t04, validate_t05, validate_t09, validate_t10


class Semantics(unittest.TestCase):
    def fixture(self, name):
        if name in ("t01", "t05"):
            return json.loads(gzip.decompress((Path(__file__).parent / ("fixtures/" + name + ".json.gz")).read_bytes()))
        return json.loads((Path(__file__).parent / "fixtures" / (name + ".json")).read_text())

    def lifecycle(self, data):
        return validate_t01(data["cases"], data["events"], data["device"],
               SimpleNamespace(armed={}, records=data["records"], inventory=data["inventory"]))

    def test_real_full_lifecycle(self):
        self.assertEqual(self.lifecycle(self.fixture("t01"))["status"], "PASS")

    def test_real_descriptors(self):
        data = self.fixture("t09")
        self.assertEqual(validate_t09(data["cases"], data["events"], 16, data["device"])["status"], "PASS")

    def dma_fault(self, data):
        return validate_t05(data["cases"], data["events"], data["variant"], data["device"])

    def test_real_dma_faults(self):
        for data in self.fixture("t05"):
            with self.subTest(variant=data["variant"]):
                self.assertEqual(self.dma_fault(data)["status"], "PASS")

    def test_corrupt_dma_faults_rejected(self):
        for event, field, value in (("T05_FAULT", "payload_changed", 99),
                ("T05_FAULT", "cpu_rollback", True), ("T05_FAULT", "completion_hex", ""),
                ("T05_COUNTERS", "dma_fault", 99), ("T05_COUNTERS", "stale_cpl", 1),
                ("OWNER_MAP", "flags", 7), ("OWNER_HELD_CHECK", "crc", -1),
                ("OWNER_WORKLOAD", "completed", 0), ("OWNER_PATTERN_COMMAND", "result", -1),
                ("OWNER_UNMAP", "returned", 0), ("OWNER_EXIT", "final_fds", 99),
                ("M3_OWNER_REAP", "exit_code", 1), ("T05_POST_DISABLE", "flags", 1),
                ("T05_GLOBAL_RECOVER", "vfs", 4)):
            with self.subTest(event=event, field=field):
                data = self.fixture("t05")[1]
                next(r for r in data["events"] if r.get("event") == event)[field] = value
                with self.assertRaises(RuntimeError): self.dma_fault(data)

    def test_dma_fault_requester_and_healthy_order_rejected(self):
        for change in ("fault_rid", "healthy_rid", "healthy_order", "missing_fault"):
            data = self.fixture("t05")[0]
            if change == "fault_rid": data["device"] = data["device"].replace("TOY_DMA_ERROR vf=0 rid=0101", "TOY_DMA_ERROR vf=0 rid=0102")
            elif change == "healthy_rid": data["device"] = data["device"].replace("TOY_DMA_OK vf=1 rid=0102", "TOY_DMA_OK vf=1 rid=0101")
            elif change == "healthy_order":
                work = next(r for r in data["events"] if r.get("event") == "OWNER_WORKLOAD")
                data["events"].remove(work)
                data["events"].insert(0, work)
            else: data["events"].remove(next(r for r in data["events"] if r.get("event") == "T05_FAULT"))
            with self.assertRaises(RuntimeError): self.dma_fault(data)

    def test_corrupt_descriptors_rejected(self):
        for kind, field, value in (("T09_VECTOR", "descriptor_hex", "00" * 32),
                ("T09_VECTOR", "completion_hex", "00" * 32), ("T09_VECTOR", "irq_after", 999),
                ("T09_VECTOR", "cq_after", 999), ("T09_VECTOR", "sq_head", 999),
                ("T09_VECTOR", "payload_unchanged", False), ("T09_VECTOR", "completion_status", 0),
                ("T09_COUNTERS", "stale_desc", 5), ("T09_COUNTERS", "stale_cpl", 1),
                ("OWNER_HELD_CHECK", "bytewise_unchanged", False)):
            with self.subTest(kind=kind, field=field):
                data = self.fixture("t09")
                next(r for r in data["events"] if r.get("event") == kind)[field] = value
                with self.assertRaises(RuntimeError): validate_t09(data["cases"], data["events"], 16, data["device"])

    def test_missing_descriptor_rejected(self):
        data = self.fixture("t09")
        data["events"].remove(next(r for r in data["events"] if r.get("event") == "T09_VECTOR"))
        with self.assertRaises(RuntimeError): validate_t09(data["cases"], data["events"], 16, data["device"])

    def test_lifecycle_leaks_rejected(self):
        for event, field, value in (("T01_LIFECYCLE", "contexts", 2), ("T01_LIFECYCLE", "work_busy", 1),
                ("T01_LIFECYCLE", "events", 1), ("T01_LIFECYCLE", "children", 1),
                ("T01_LIFECYCLE", "fds", 99), ("OWNER_EXIT", "final_fds", 99),
                ("OWNER_UNMAP", "returned", 0), ("M3_OWNER_REAP", "exit_code", 42),
                ("T01_CYCLE_END", "pci_devices_absent", 0), ("T01_LEDGER", "lease", 1)):
            with self.subTest(event=event, field=field):
                data = self.fixture("t01")
                next(r for r in data["events"] if r.get("event") == event)[field] = value
                with self.assertRaises(RuntimeError): self.lifecycle(data)

    def test_lifecycle_budget_and_inventory_rejected(self):
        for mode in ("cycle", "owner", "inventory"):
            data = self.fixture("t01")
            if mode == "inventory": data["inventory"]["final"]["owned"]["worker-pending"] = 1
            else:
                name = "T01_CYCLE_BEGIN" if mode == "cycle" else "M3_PEER_VERIFIED"
                data["events"].remove(next(r for r in data["events"] if r.get("event") == name))
            with self.assertRaises(RuntimeError): self.lifecycle(data)

    def provision(self, data):
        return validate_t02_provision(data["cases"], data["events"], data["kernel"], data["device"],
               SimpleNamespace(armed={}, records=data["records"], inventory=data["inventory"]))

    def busy(self, data):
        return validate_t10(data["cases"], data["events"], 16, data["device"])

    def pci(self, data):
        return validate_t02_pci(data["cases"], data["events"], data["kernel"], data["device"],
               SimpleNamespace(armed={}, records=data["records"], inventory=data["inventory"]))

    def test_real_partial_pci(self):
        self.assertEqual(self.pci(self.fixture("t02_pci"))["status"], "PASS")

    def test_real_isolation(self):
        data = self.fixture("t04")
        self.assertEqual(validate_t04(data["cases"], data["events"], 16, data["device"])["status"], "PASS")

    def test_corrupt_isolation_rejected(self):
        for corruption in ("rid", "group", "pid", "iova", "guard", "digest", "reap", "crc", "ordering", "final"):
            with self.subTest(corruption=corruption):
                data = self.fixture("t04")
                rows = lambda kind: [r for r in data["events"] if r.get("event") == kind]
                if corruption == "rid": data["device"] = data["device"].replace("rid=0102", "rid=0101")
                elif corruption == "group": rows("T04_GROUP")[1]["group"] = rows("T04_GROUP")[0]["group"]
                elif corruption == "pid": rows("M3_PEER_VERIFIED")[1]["pid"] = rows("M3_PEER_VERIFIED")[0]["pid"]
                elif corruption == "iova": rows("OWNER_MAP")[3]["iova"] += 4096
                elif corruption == "guard": rows("OWNER_HELD_CHECK")[0]["bytewise_unchanged"] = False
                elif corruption == "digest": rows("OWNER_HELD_CHECK")[0]["crc"] ^= 1
                elif corruption == "reap": rows("M3_OWNER_REAP")[0]["exit_code"] = 1
                elif corruption == "crc": rows("OWNER_PATTERN_COMMAND")[1]["result"] ^= 1
                elif corruption == "final": rows("T04_FINAL")[0]["lease"] = 42
                else:
                    row = rows("OWNER_HELD_CHECK")[0]
                    data["events"].remove(row)
                    data["events"].insert(0, row)
                with self.assertRaises(RuntimeError): validate_t04(data["cases"], data["events"], 16, data["device"])

    def test_corrupt_partial_pci_rejected(self):
        original = self.fixture("t02_pci")
        for corruption in ("hit", "quota", "retry", "devices", "recovery"):
            with self.subTest(corruption=corruption):
                data = copy.deepcopy(original)
                if corruption == "hit": data["kernel"] = data["kernel"].replace("added=3", "added=0", 1)
                elif corruption == "quota":
                    next(r for r in data["events"] if r.get("event") == "T02_LEDGER" and r.get("reserve") == 8)["reserve"] = 0
                elif corruption == "recovery":
                    next(r for r in data["events"] if r.get("event") == "T02_GLOBAL_RECOVER")["after_session"] = 0
                else:
                    next(r for r in data["events"] if r.get("event") == "T02_PCI_CASE")["clean_retry" if corruption == "retry" else "pci_devices_absent"] = False
                with self.assertRaises(RuntimeError): self.pci(data)

    def test_real_provision(self):
        self.assertEqual(self.provision(self.fixture("t02_provision"))["status"], "PASS")

    def test_corrupt_provision_rejected(self):
        original = self.fixture("t02_provision")
        for event, field, replacement in (("T02_DRIVER_HIT", "fired", 0), ("T02_DRIVER_HIT", "name", "off"),
                ("T02_LEDGER", "reserve", 8), ("T02_PROVISION_CASE", "expected_errno", 0),
                ("T02_GLOBAL_RECOVER", "after_session", 0)):
            with self.subTest(event=event, field=field):
                data = copy.deepcopy(original)
                next(r for r in data["events"] if r.get("event") == event)[field] = replacement
                with self.assertRaises(RuntimeError): self.provision(data)

    def test_missing_driver_hit_rejected(self):
        data = self.fixture("t02_provision")
        data["events"].remove(next(r for r in data["events"] if r.get("event") == "T02_DRIVER_HIT"))
        with self.assertRaisesRegex(RuntimeError, "actual host"): self.provision(data)

    def test_raw_hit_and_failed_barrier_rejected(self):
        for corrupt in ("kernel", "barrier"):
            data = self.fixture("t02_provision")
            if corrupt == "kernel": data["kernel"] = data["kernel"].replace("TOY_FAILPOINT_HIT", "not-hit", 1)
            else: data["records"][0]["status"] = "FAIL"
            with self.assertRaises(RuntimeError): self.provision(data)

    def test_real_busy(self):
        self.assertEqual(self.busy(self.fixture("t10"))["status"], "PASS")

    def test_corrupt_busy_rejected(self):
        original = self.fixture("t10")
        for event, field, replacement in (("T10_BUSY", "errno", 0), ("T10_BUSY", "owner_pid", -1),
                ("T10_BUSY", "topology_unchanged", False), ("T10_SNAPSHOT", "lease", 0),
                ("T10_SNAPSHOT", "reserve", 0), ("OWNER_UNMAP", "returned", 0)):
            with self.subTest(event=event, field=field):
                data = copy.deepcopy(original)
                next(r for r in data["events"] if r.get("event") == event)[field] = replacement
                with self.assertRaises(RuntimeError): self.busy(data)

    def test_real_owner_faults(self):
        data = self.fixture("t02_owner")
        self.assertEqual(validate_t02_owner(data["cases"], data["events"], data["device"])["status"], "PASS")

    def test_corrupt_owner_faults_rejected(self):
        original = self.fixture("t02_owner")
        for event, field, replacement in (("OWNER_FAILURE_CLEANUP", "final_fds", 99),
                ("OWNER_FAILURE_CLEANUP", "fence_verified", False), ("T02_OWNER_REAP", "exit_code", 0),
                ("OWNER_UNMAP_QUARANTINE", "buffer_held", False), ("OWNER_FAILPOINT_HIT", "name", "missing")):
            with self.subTest(event=event, field=field):
                data = copy.deepcopy(original)
                next(r for r in data["events"] if r.get("event") == event)[field] = replacement
                with self.assertRaises(RuntimeError): validate_t02_owner(data["cases"], data["events"], data["device"])

    def test_fake_short_unmap_rejected(self):
        data = self.fixture("t02_owner")
        row = next(r for r in data["events"] if r.get("event") == "OWNER_UNMAP_QUARANTINE" and r.get("ioctl_executed"))
        row["returned"] = row["requested"]
        with self.assertRaises(RuntimeError): validate_t02_owner(data["cases"], data["events"], data["device"])


if __name__ == "__main__": unittest.main()
