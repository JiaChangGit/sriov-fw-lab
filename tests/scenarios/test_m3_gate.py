"""An aggregate must not claim M3 on a partial, failed or changed-input run."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools"))
import verify_m3_checkpoint as gate


class FullGate(unittest.TestCase):
    def exercise(self,failure=None,mutate=False,missing=False):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);out=root/"artifacts/m3/run";out.mkdir(parents=True)
            (root/"tests").mkdir()
            for name in ("sources.lock.json","tests/scenarios.json","Makefile"):
                (root/name).write_text("{}")
            def run(argv,directory,name,**kwargs):
                log=out/(name+".log");log.write_text("mocked aggregate-control test only\n")
                if name==failure: raise RuntimeError("injected stage failure")
                pointer_name="m1" if name.startswith("m1-") else "m3-case" if name=="t10" else name
                result=root/("artifacts/"+pointer_name+"/"+name+"/result.json")
                result.parent.mkdir(parents=True,exist_ok=True)
                result.write_text(json.dumps({"status":"FAIL" if missing and name=="t08" else "PASS"}))
                (root/("artifacts/"+pointer_name+"-latest.json")).write_text(json.dumps(
                    {"status":"PASS","path":str(result.parent.relative_to(root))}))
                if mutate and name=="validators": (root/"Makefile").write_text("changed")
                return {"log":str(log.relative_to(root)),"exit_code":0}
            with patch.object(gate,"ROOT",root),patch.object(gate,"artifact_dir",return_value=out),\
                 patch.object(gate,"provenance",return_value={}),patch.object(gate,"run_logged",side_effect=run):
                rc=gate.main(complete=True)
            return rc,json.loads((out/"result.json").read_text())

    def test_full_gate_requires_all_ten(self):
        rc,result=self.exercise()
        self.assertEqual(rc,0)
        self.assertEqual(result["m3_status"],"PASS")
        self.assertEqual([c["id"] for c in result["cases"]],[f"T{i:02}" for i in range(1,11)])
        self.assertTrue(all(c["status"]=="PASS" for c in result["cases"]))
        self.assertEqual(result["steps"][0]["id"],"m0")

    def test_stage_failure_leaves_later_cases_not_run(self):
        rc,result=self.exercise(failure="t07")
        self.assertEqual(rc,1)
        self.assertEqual(result["m3_status"],"NOT_RUN")
        self.assertEqual(result["cases"][-1]["status"],"NOT_RUN")
        self.assertEqual(result["cases"][6]["status"],"FAIL")
        self.assertEqual(next(s for s in result["steps"] if s["id"]=="t07")["status"],"FAIL")

    def test_inputs_cannot_change_during_gate(self):
        rc,result=self.exercise(mutate=True)
        self.assertEqual(rc,1)
        self.assertEqual(result["status"],"FAIL")
        self.assertEqual(result["m3_status"],"NOT_RUN")
        self.assertIn("inputs changed",result["error"])

    def test_exit_zero_does_not_override_failed_result(self):
        rc,result=self.exercise(missing=True)
        self.assertEqual(rc,1)
        self.assertEqual(result["m3_status"],"NOT_RUN")
        self.assertEqual(next(s for s in result["steps"] if s["id"]=="t08")["status"],"FAIL")


if __name__=="__main__":unittest.main()
