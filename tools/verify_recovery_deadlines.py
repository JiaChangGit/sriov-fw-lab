#!/usr/bin/env python3
"""T07 supplemental exact logical boundaries; native fake HAL, not VM timings."""
import json
import os
import subprocess
from build_core import FLAGS
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json


def verify():
    out=artifact_dir("recovery-deadlines")
    result={**provenance(),"status":"FAIL","scope":"native_fake_HAL_exact_deadlines",
            "vm_timing_claim":False}
    try:
        sources=sorted((ROOT/"fwcore").glob("*.c"))+[ROOT/"tests/unit/fake_hal.c",ROOT/"tests/scenarios/deadline_vectors.c"]
        headers=sorted((ROOT/"fwcore").glob("*.h"))+[ROOT/"tests/unit/fake_hal.h"]
        inputs=sources+headers+[ROOT/"tools/build_core.py",ROOT/"tools/verify_recovery_deadlines.py"]
        result["inputs"]={str(p.relative_to(ROOT)):sha256(p) for p in inputs}
        binary=out/"deadline-vectors"
        run_logged(["clang",*FLAGS,"-O1","-g","-fno-omit-frame-pointer","-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",*map(str,sources),"-o",str(binary)],out,"build")
        env={**os.environ,"ASAN_OPTIONS":"detect_leaks=1:abort_on_error=1","UBSAN_OPTIONS":"halt_on_error=1:print_stacktrace=1"}
        run_logged([str(binary)],out,"vectors",env=env)
        rows=[json.loads(line) for line in (out/"vectors.log").read_text().splitlines() if line.startswith("{")]
        expected=[]
        # 規格期望值獨立寫出；不是從 fwcore 的轉移表或輸出回推。
        for phase,deadline,state in (("quiesce",100,3),("reset",100,4),("ready",1000,5)):
            for elapsed in (deadline-1,deadline,deadline+1):
                expired=elapsed>=deadline
                expected.append(dict(phase=phase,elapsed=elapsed,start=500,deadline=500+deadline,status_code=0,
                                     state=6 if expired else state,fenced=1,killed=int(expired),reserve=8,used_credit=8))
        if rows!=expected: raise RuntimeError("Exact deadline, failure containment or retained quota mismatch")
        write_json(out/"boundaries.json",rows)
        result.update(status="PASS",boundaries=rows,evidence_hashes={name:sha256(out/name) for name in
                      ("build.log","vectors.log","boundaries.json","deadline-vectors")})
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as error:
        result["error"]=str(error)
    write_json(out/"result.json",result)
    write_json(ROOT/"artifacts/recovery-deadlines-latest.json",dict(path=str(out.relative_to(ROOT)),status=result["status"]))
    if result["status"]!="PASS": raise RuntimeError("Recovery deadline checks failed: "+str(out))
    return dict(path=str(out.relative_to(ROOT)),status="PASS",sha256=sha256(out/"result.json"))


if __name__=="__main__": print(json.dumps(verify()))
