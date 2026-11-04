"""Mutate authentic runtime records to verify that acceptance rejects missing proofs."""
import gzip
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from validate_t06 import validate_t06


class Reset(unittest.TestCase):
    def fixture(self):
        return json.loads(gzip.decompress((Path(__file__).parent / "fixtures/t06.json.gz").read_bytes()))

    def check(self, data):
        return validate_t06(data["cases"],data["events"],data["device"])

    def test_real_vfio_reset(self):
        self.assertEqual(self.check(self.fixture())["status"], "PASS")

    def test_reset_corruption_rejected(self):
        for event,field,value in (("T06_CLEAR","zero_registers",0),("T06_IOCTL_RESET","new_epoch",0),
                ("T06_REARM_REJECT","errno",0),("T06_SNAPSHOT","ready_gen",99),
                ("T06_HEALTHY_PROGRESS","epoch",999),("T06_HEALTHY_CHECK","state",5),
                ("OWNER_HELD_CHECK","crc",-1),("OWNER_WORKLOAD","payload_crc",-1),
                ("OWNER_UNMAP","returned",0),("OWNER_EXIT","final_fds",99),
                ("T06_FINAL","lease",1)):
            with self.subTest(event=event,field=field):
                data = self.fixture()
                next(r for r in data["events"] if r.get("event") == event)[field] = value
                with self.assertRaises(RuntimeError): self.check(data)

    def test_missing_flr_and_healthy_progress_rejected(self):
        for mode in ("flr","rid","progress","ordering"):
            data = self.fixture()
            if mode == "flr": data["device"] = data["device"].replace("TOY_FLR vf=0", "REMOVED vf=0",1)
            elif mode == "rid": data["device"] = data["device"].replace("rid=0102", "rid=0101")
            elif mode == "progress": data["events"].remove(next(r for r in data["events"] if r.get("event") == "T06_HEALTHY_PROGRESS"))
            else:
                row = next(r for r in data["events"] if r.get("event") == "T06_REARM_REJECT")
                row["ready_gen"] += 1
            with self.subTest(mode=mode),self.assertRaises(RuntimeError): self.check(data)


if __name__ == "__main__": unittest.main()
