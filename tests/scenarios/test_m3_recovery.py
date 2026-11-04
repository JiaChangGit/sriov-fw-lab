"""Validate preserved real reset traces and reject corrupted acceptance evidence."""
import gzip
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
from validate_recovery import validate_recovery


class Recovery(unittest.TestCase):
    def fixture(self,name):
        return json.loads(gzip.decompress((Path(__file__).parent/("fixtures/"+name.lower()+".json.gz")).read_bytes()))

    def check(self,data,name):
        return validate_recovery(data["cases"],data["events"],data["device"],name,
                                 SimpleNamespace(records=data["records"],armed={}))

    def test_real_recovery(self):
        for case in ("T07","T08"):
            result=self.check(self.fixture(case),case)
            self.assertEqual(result["status"],"PASS")
            self.assertEqual(json.loads(json.dumps(result["evidence"])),result["evidence"])

    def test_recovery_mutations_rejected(self):
        for event,field,value in (("RECOVERY_PENDING","descriptor_hex","00"*32),
                ("RECOVERY_CANCELLED","requests_marked_error",0),("RECOVERY_LOADED","outstanding",0),
                ("RECOVERY_STOPPED","submissions_stopped",False),("T08_SNAPSHOT","flags",28),
                ("RECOVERY_COUNTERS","stale_desc",1),("OWNER_RETIRED_CHECK","crc",-1),
                ("OWNER_HELD_CHECK","crc",-1),("RECOVERY_NO_IRQ","eventfd_errno",0),
                ("RECOVERY_HEALTHY_PROGRESS","epoch",999),("OWNER_UNMAP","returned",0),
                ("OWNER_EXIT","buffers",1),("RECOVERY_FINAL","reserve",8)):
            with self.subTest(event=event,field=field):
                data=self.fixture("T08")
                next(r for r in data["events"] if r.get("event")==event)[field]=value
                with self.assertRaises(RuntimeError): self.check(data,"T08")

    def test_fake_callback_and_clock_rejected(self):
        for mode in ("callback","pending","clock","ack","rid"):
            data=self.fixture("T08")
            if mode=="callback": data["device"]=data["device"].replace("TOY_STALE_CPL vf=0","REMOVED vf=0",1)
            elif mode=="pending": data["device"]=data["device"].replace("try-stale-cpl valid=1","try-stale-cpl valid=0")
            elif mode=="rid": data["device"]=data["device"].replace("rid=0102","rid=0101")
            elif mode=="ack": data["records"][0]["status"]="FAIL"
            else:
                ob=next(r["observation"] for r in data["records"] if r["request"]["action"]=="virtual-delay")
                ob["last_tick"]=ob["first_tick"]
            with self.subTest(mode=mode),self.assertRaises(RuntimeError): self.check(data,"T08")


if __name__=="__main__": unittest.main()
