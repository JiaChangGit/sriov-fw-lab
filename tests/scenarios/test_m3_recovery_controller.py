import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
from m3_controller import M3Controller


class Stream:
    def __init__(self): self.sent=[]
    def sendall(self,b): self.sent.append(b)


class Device:
    toy_path="/machine/peripheral/toy"
    def __init__(self,ticks=(100,),pending=1): self.ticks=iter(ticks); self.pending=pending
    def execute(self,command,args):
        assert command=="qom-get" and args["path"]==self.toy_path
        return next(self.ticks) if args["property"]=="virtual-tick" else self.pending


class RecoveryController(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.c=M3Controller(Path(self.tmp.name),"T08")
        self.s=Stream()
        self.item={"event":"M3_BARRIER","scenario":"T08","phase":"request","barrier":"test",
                   "action":"virtual-delay","ticks":200}
    def tearDown(self): self.tmp.cleanup()

    def test_virtual_clock_determines_ack(self):
        with patch("m3_controller.time.sleep"),patch("m3_controller.time.monotonic",return_value=0):
            self.c(self.item,Device((100,101,299,300)),self.s)
        self.assertEqual(self.c.records[0]["observation"],{"first_tick":100,"last_tick":300,"requested_ticks":200})
        self.assertEqual(self.s.sent,[b"M3_HOST_ACK test\n"])

    def test_regressing_or_stalled_clock_rejected(self):
        with patch("m3_controller.time.sleep"),patch("m3_controller.time.monotonic",return_value=0):
            with self.assertRaisesRegex(RuntimeError,"regressed"):
                self.c(self.item,Device((100,190,180)),self.s)
        self.assertEqual(self.s.sent,[])
        c=M3Controller(Path(self.tmp.name),"T08")
        with patch("m3_controller.time.sleep"),patch("m3_controller.time.monotonic",side_effect=(0,20)):
            with self.assertRaisesRegex(RuntimeError,"bounded progress"):
                c(self.item,Device((100,)),self.s)
        self.assertEqual(c.records[0]["status"],"FAIL")

    def test_actual_pending_required(self):
        for value in (0,True):
            c=M3Controller(Path(self.tmp.name),"T08")
            with self.assertRaises(RuntimeError):
                c({**self.item,"action":"pending-check","expected":1},Device(pending=value),self.s)
        self.assertEqual(self.s.sent,[])
        self.c({**self.item,"action":"pending-check","expected":1},Device(pending=1),self.s)
        self.assertEqual(self.c.records[0]["observation"]["pending"],1)


if __name__=="__main__": unittest.main()
