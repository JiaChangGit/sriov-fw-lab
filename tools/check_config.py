#!/usr/bin/env python3
"""Check required capabilities in the final kernel .config, not just fragments."""
import argparse
import json
from pathlib import Path
from lab_common import ROOT, sha256

BASE = """64BIT X86_64 SMP PCI PCI_MMCONFIG PCIEPORTBUS PCI_IOV PCI_MSI ACPI INTEL_IOMMU
IOMMU_SUPPORT VFIO VFIO_PCI VFIO_GROUP VFIO_CONTAINER VFIO_IOMMU_TYPE1
DEVTMPFS PROC_FS SYSFS DEBUG_FS BLK_DEV_INITRD SERIAL_8250_CONSOLE UNIX
EVENTFD EPOLL EFI EFI_STUB MODULES IRQ_REMAP BINFMT_ELF X86_IOPL_IOPERM""".split()


def check(path, profile):
    if profile not in ("base", "stress", "kasan"):
        raise ValueError("Supported guest profiles are stress and kasan")
    values = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key[7:]] = value
    required = BASE + ({"kasan": ["KASAN", "KASAN_GENERIC", "PROVE_LOCKING", "DEBUG_ATOMIC_SLEEP", "DEBUG_LIST", "DEBUG_KMEMLEAK"]}.get(profile, []))
    missing = [name for name in required if values.get(name) != "y"]
    forbidden = ["VFIO_NOIOMMU", "IOMMUFD", "KCSAN"]
    if profile != "kasan": forbidden.append("KASAN")
    enabled = [name for name in forbidden if values.get(name) in {"y", "m"}]
    return {"status": "FAIL" if missing or enabled else "PASS", "profile": profile,
            "config_sha256": sha256(path), "missing_built_in": missing,
            "forbidden_enabled": enabled, "checked_symbols": {key: values.get(key, "n") for key in required},
            "pidfd_evidence": "Linux v6.12 fs/Makefile builds pidfs.o unconditionally; no CONFIG_PIDFD exists."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("stress", "kasan"), default="stress")
    parser.add_argument("--config", type=Path)
    args = parser.parse_args()
    result = check(args.config or ROOT / f"build/kernel-{args.profile}/.config", args.profile)
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__": raise SystemExit(main())
