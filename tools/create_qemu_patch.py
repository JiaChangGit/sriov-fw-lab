#!/usr/bin/env python3
"""Reproduce the small integration patch from the immutable pinned source."""
import difflib
import json
from lab_common import ROOT, sha256, write_json

edits = {
    "hw/misc/meson.build": "\nsubdir('sriov-fw-lab')\n",
    "hw/misc/Kconfig": "\nconfig TOY_SRIOV\n    bool\n    default y\n    depends on PCI_EXPRESS && MSI_NONBROKEN\n",
}
patch = ROOT / "patches/qemu/0001-toy-sriov-build.patch"
patch.parent.mkdir(parents=True, exist_ok=True)
lines = []
for path, addition in edits.items():
    before = (ROOT / "downloads/qemu" / path).read_text()
    after = before + addition
    lines.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True), "a/" + path, "b/" + path))
patch.write_text("".join(lines))
lock = json.loads((ROOT / "sources.lock.json").read_text())
lock["sources"]["qemu"]["patches"] = [{"path": str(patch.relative_to(ROOT)), "sha256": sha256(patch)}]
write_json(ROOT / "sources.lock.json", lock)
print(patch)
