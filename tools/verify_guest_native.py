#!/usr/bin/env python3
"""Exercise actual stream framing and descriptor rejection under ASan/UBSan."""
import json
import os
import subprocess
import sys
from lab_common import ROOT, artifact_dir, provenance, run_logged, sha256, write_json


def main():
    out = artifact_dir("guest-native")
    result = {**provenance(), "status": "FAIL", "scope": "wire_uapi_and_tenant_ring_cursors"}
    paths = [ROOT / p for p in ("tests/guest/test_wire.c", "guest/common/lab_wire.c",
                               "guest/common/lab_wire.h", "include/toy_uapi.h",
                               "guest/common/lab_ring.h", "tests/guest/ring_driver.c",
                               "tests/guest/ring_oracle.py", "tests/guest/uapi_layout.c")]
    paths += [ROOT / p for p in ("tests/guest/test_owner_workload.c", "guest/tenant/owner.c",
                                 "guest/tenant/owner.h", "guest/tenant/vfio.h", "include/toy_regs.h")]
    result["inputs"] = {str(p.relative_to(ROOT)): sha256(p) for p in paths}
    binary = out / "test-wire"
    try:
        for bits in (32, 64):
            run_logged(["gcc", f"-m{bits}", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                        "-I" + str(ROOT / "include"), "-I/usr/include/x86_64-linux-gnu", str(paths[7])],
                       out, f"uapi-{bits}", timeout=30)
        run_logged(["clang", "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                    "-I" + str(ROOT / "include"), "-I" + str(ROOT / "guest/common"),
                    str(paths[0]), str(paths[1]), "-o", str(binary)], out, "compile", timeout=60)
        run_logged([str(binary)], out, "wire", timeout=60,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
        ring = out / "ring-driver"
        run_logged(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "guest/common"), str(paths[5]), "-o", str(ring)], out, "ring-compile", timeout=60)
        run_logged([sys.executable, str(paths[6]), str(ring)], out, "ring-oracle", timeout=60,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
        owner = out / "owner-workload"
        run_logged(["clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-Wl,--wrap=pread", "-Wl,--wrap=pwrite",
                    "-I" + str(ROOT / "include"), "-I" + str(ROOT / "guest/common"),
                    "-I" + str(ROOT / "guest/tenant"), str(paths[8]), str(paths[9]), str(paths[1]),
                    "-o", str(owner)], out, "owner-compile", timeout=60)
        run_logged([str(owner)], out, "owner-workload", timeout=60,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
        if "OWNER_WORKLOAD_TEST_PASS aligned=5 stale_sq_gap=5 counter_wrap=5" not in (out / "owner-workload.log").read_text():
            raise RuntimeError("Missing SQ/CQ cursor regression marker")
        result.update(status="PASS", binary_sha256=sha256(binary), owner_binary_sha256=sha256(owner),
                      owner_cursor_regression="PASS",
                      driver_events="NOT_RUN", vfio_owner_fault_unwind="NOT_RUN")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
    write_json(out / "result.json", result)
    write_json(ROOT / "artifacts/guest-native-latest.json", {"path": str(out.relative_to(ROOT)), "status": result["status"]})
    print(json.dumps({"status": result["status"], "evidence": str(out.relative_to(ROOT))}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
