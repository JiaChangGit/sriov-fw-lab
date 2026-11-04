#!/usr/bin/env python3
"""Run a reset-under-load or actual stale-completion case after current M1/M2."""
import argparse
import json
import subprocess
import sys
from types import SimpleNamespace
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json
from run_uefi import parse_serial, require_guest_builds, require_m1
from run_vm import KERNEL_REPORT
from validate_recovery import validate_recovery
from verify_m2 import validate_saved_run
from verify_recovery_deadlines import verify as verify_deadlines


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case",required=True,choices=("T07","T08"))
    args=parser.parse_args()
    out=artifact_dir(args.case.lower())
    result={**provenance(),"id":args.case,"status":"FAIL","m3_status":"NOT_RUN"}
    try:
        binary=ROOT/"build/qemu/qemu-system-x86_64"
        result["prerequisite_m1"]=require_m1(binary,ROOT/"build/kernel-stress/arch/x86/boot/bzImage",ROOT/"build/initramfs-stress/initramfs.cpio.gz")
        result["prerequisite_m2"]=[validate_saved_run(mode,"stress") for mode in ("probe","handoff","negative")]
        result["builds"]=require_guest_builds(binary,ROOT/"build/kernel-kasan/arch/x86/boot/bzImage",ROOT/"build/initramfs-kasan/initramfs.cpio.gz","kasan")
        if args.case=="T07": result["logical_deadlines"]=verify_deadlines()
        run_logged([sys.executable,"tools/run_vm.py","--gate","m3-case","--profile","kasan","--case",args.case],out,"recovery",timeout=1000)
        pointer=json.loads((ROOT/"artifacts/m3-case-latest.json").read_text())
        directory=ROOT/pointer["path"]
        report=json.loads((directory/"result.json").read_text())
        if report["status"]!="PASS" or report["unexpected_kernel_reports"]: raise RuntimeError("Recovery VM did not pass")
        for name,digest in report["evidence_hashes"].items():
            if sha256(directory/name)!=digest: raise RuntimeError("Evidence changed: "+name)
        serial=(directory/"serial.log").read_text()
        if any(KERNEL_REPORT.search(line) for line in ((directory/"kernel.log").read_text()+serial).splitlines()):
            raise RuntimeError("Unexpected kernel diagnostic")
        cases,events=parse_serial(serial)
        barrier=json.loads((directory/"m3-barriers.json").read_text())
        controller=SimpleNamespace(records=barrier["records"],armed=barrier["pending_failpoints"])
        checked=validate_recovery(cases,events,(directory/"qemu.log").read_text(),args.case,controller)
        for name,rows in checked["evidence"].items():
            if json.loads((directory/name).read_text())!=rows: raise RuntimeError("Derived evidence mismatch: "+name)
        result.update(status="PASS",path=pointer["path"],result_sha256=sha256(directory/"result.json"),validation=checked,artifact_hashes=report["artifact_hashes"])
    except (OSError,ValueError,KeyError,RuntimeError,subprocess.SubprocessError) as error:
        result["error"]=str(error); print(error,flush=True)
    write_json(out/"result.json",result)
    write_json(ROOT/("artifacts/"+args.case.lower()+"-latest.json"),{"path":str(out.relative_to(ROOT)),"status":result["status"]})
    print(json.dumps({"status":result["status"],"scope":args.case+"_only","m3_status":"NOT_RUN","evidence":str(out.relative_to(ROOT))}))
    return 0 if result["status"]=="PASS" else 1


if __name__=="__main__": raise SystemExit(main())
