#!/usr/bin/env python3
"""Controller-only synthetic fixtures. They never count as an executed M2 boot."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_uefi import (parse_serial, validate_checkpoint_events, validate_handoff,
                      validate_handoff_transport, validate_negative, validate_probe)
from test_m1_validator import fixture as m1_fixture


def firmware(mode="handoff", unassigned=False):
    lines = ["SRIOV_PROBE_START ABI=1.0", "PROBE_MODE=" + mode, "CORE_SELFTEST=PASS",
             "PF_LOCATION segment=0 bus=1 device=0 function=0", "PF_MSIX_VECTORS=2",
             "PCI_CAP offset=0x50 id=0x11", "PCI_CAP offset=0x80 id=0x10",
             "PCI_EXT_CAP offset=0x100 id=0xe version=1", "PCI_EXT_CAP offset=0x140 id=0x10 version=1",
             "PCI_EXT_CAP offset=0x200 id=0xd version=1",
             "CAP_TOTAL_VFS=8 CAP_INITIAL_VFS=8 VF_DEVICE_ID=0xe002 VF_OFFSET=1 VF_STRIDE=1",
             f"SRIOV_SUPPORTED_PAGE_SIZE=0x1 SRIOV_SYSTEM_PAGE_SIZE=0x{0 if unassigned else 1:x} SRIOV_CONTROL=0x0",
             "PF_ASSIGNED_BAR0_BASE=0xfe800000 PF_ASSIGNED_BAR0_LENGTH=0x10000 PF_ASSIGNED_BAR0_END=0xfe80ffff",
             "PF_ASSIGNED_BAR2_BASE=0xfe810000 PF_ASSIGNED_BAR2_LENGTH=0x4000 PF_ASSIGNED_BAR2_END=0xfe813fff"]
    for bar, start in ((0, 0xfe820000), (2, 0xfe840000)):
        start = 0 if unassigned else start
        lines.append(f"VF_BAR{bar}_REQUIRED=0x20000 VF_BAR{bar}_ASSIGNED_BASE=0x{start:x} VF_BAR{bar}_ASSIGNED_END=0x{start + 0x1ffff:x}")
    lines += ["BRIDGE_WINDOW segment=0 bdf=00:02.0 secondary=1 subordinate=1 base=0xfe800000 limit=0xfe8fffff",
              "BRIDGE_MATCH_COUNT=1 VF_WINDOW_ASSERTION=" + ("OBSERVE_ONLY" if mode.startswith("negative") else "CHECKED"),
              "MAILBOX_VERSION=1.0", "MAILBOX_UNKNOWN_MAJOR=REJECTED status=7"]
    lines += [json.dumps({"id": case, "status": "PASS"}) for case in ("M2_CORE", "M2_CAPS", "M2_WINDOWS", "M2_MAILBOX")]
    return "\n".join(lines + ["PROBE_PASS"]) + "\n"


def transport():
    return ("HANDOFF_LOAD_OPTIONS=console=ttyS0 rdinit=/init intel_iommu=on iommu.strict=1 panic=-1 lab_gate=m1\n"
            "HANDOFF_START=LINUX_EFI_STUB\nINITRD_LOADFILE2_BYTES=12345\nLinux version 6.12.0\n")


def checkpoints():
    names = ("M1_COLD_CHECKPOINT", "M1_INITIAL_CHECKPOINT", "M1_FINAL_CHECKPOINT")
    events = []
    for name in names:
        events += [{"event": name}, {"event": "M1_HOST_CHECKPOINT_ACK", "checkpoint": name, "status": "PASS"}]
    return events[:2] + [dict(event="warmup_enable_snapshot", cycle=0, enabled=4, ledger_credit=32),
                        dict(event="warmup_disable_snapshot", cycle=0, enabled=0, ledger_credit=0, outstanding=0, vf_links=0)] + events[2:]


def successful_linux():
    cases, events = m1_fixture()
    measured = [item for item in events if item["event"] in ("enable_snapshot", "disable_snapshot")]
    events = [item for item in events if item not in measured] + checkpoints()[:-2] + measured + checkpoints()[-2:]
    for bar, start, size in ((0, 0xfe800000, 65536), (2, 0xfe810000, 16384)):
        events.append(dict(event="pci_resource", bdf=cases["M1_GROUPS"]["pf"], bar=bar,
                           start=hex(start), end=hex(start + size - 1), size=size, flags="0x200"))
    for number, group in enumerate(cases["M1_GROUPS"]["groups"]):
        for bar, base in ((0, 0xfe820000), (2, 0xfe840000)):
            start = base + number * 16384
            events.append(dict(event="pci_resource", bdf=group["vf"], bar=bar, start=hex(start),
                               end=hex(start + 16383), size=16384, flags="0x200"))
    return cases, events


class ProbeTests(unittest.TestCase):
    def test_positive_and_unassigned_negative(self):
        self.assertEqual(validate_probe(firmware(), "handoff")["pf"], "0000:01:00.0")
        result = validate_probe(firmware("negative-handoff", True), "negative-handoff")
        self.assertEqual(result["bars"]["VF_BAR0"]["start"], 0)

    def test_corrupted_firmware_evidence(self):
        text = firmware()
        alterations = [
            lambda s: s.replace("CAP_TOTAL_VFS=8", "CAP_TOTAL_VFS=4"),
            lambda s: s.replace("VF_OFFSET=1", "VF_OFFSET=2"),
            lambda s: s.replace("SRIOV_CONTROL=0x0", "SRIOV_CONTROL=0x1"),
            lambda s: s.replace("SRIOV_SYSTEM_PAGE_SIZE=0x1", "SRIOV_SYSTEM_PAGE_SIZE=0x0"),
            lambda s: s.replace("limit=0xfe8fffff", "limit=0xfe82ffff"),
            lambda s: s.replace("id=0xd version=1", "id=0xc version=1"),
            lambda s: s.replace("PCI_CAP offset=0x80", "PCI_CAP offset=0x50"),
            lambda s: s.replace("CORE_SELFTEST=PASS", "CORE_SELFTEST=FAIL"),
            lambda s: s.replace("MAILBOX_UNKNOWN_MAJOR=REJECTED status=7", "MAILBOX_UNKNOWN_MAJOR=REJECTED status=0"),
            lambda s: s + "PROBE_PASS\n",
            lambda s: s.replace("PF_MSIX_VECTORS=2", "PF_MSIX_VECTORS=2\nPF_MSIX_VECTORS=2"),
        ]
        for index, alter in enumerate(alterations):
            with self.subTest(index=index), self.assertRaises(RuntimeError):
                validate_probe(alter(text), "handoff")

    def test_duplicate_case_rejected(self):
        with self.assertRaises(RuntimeError):
            parse_serial('{"id":"M2_CORE","status":"PASS"}\n' * 2)


class HandoffTests(unittest.TestCase):
    def test_positive_window_preservation(self):
        text = firmware() + transport()
        cases, events = successful_linux()
        result = validate_handoff(text, cases, events, validate_probe(text, "handoff"), 12345, {"status": "PASS"})
        self.assertEqual(len(result["linux_resource_relations"]), 10)

    def test_resource_and_transport_corruption(self):
        text = firmware() + transport()
        for changed in (text.replace("BYTES=12345", "BYTES=1"),
                        text.replace("panic=-1", "pci=realloc panic=-1"),
                        text.replace("panic=-1", "pci=assign-busses panic=-1"),
                        text + "Linux version second\n", text + "WARNING: real kernel issue\n"):
            with self.subTest(text=changed[-100:]), self.assertRaises(RuntimeError):
                validate_handoff_transport(changed, 12345)
        cases, events = successful_linux()
        changed = copy.deepcopy(events)
        item = next(e for e in changed if e["event"] == "pci_resource")
        item.update(start="0xfe900000", end="0xfe90ffff")
        with self.assertRaises(RuntimeError):
            validate_handoff(text, cases, changed, validate_probe(text, "handoff"), 12345, {"status": "PASS"})

    def test_missing_warmup_or_checkpoint_rejected(self):
        for event in ("M1_COLD_CHECKPOINT", "M1_HOST_CHECKPOINT_ACK", "warmup_enable_snapshot", "warmup_disable_snapshot"):
            with self.subTest(event=event), self.assertRaises(RuntimeError):
                validate_checkpoint_events([item for item in checkpoints() if item["event"] != event])


class NegativeObservationTests(unittest.TestCase):
    def test_linux_can_recover_unassigned_firmware_apertures(self):
        text = firmware("negative-handoff", True) + transport() + "LAB_RESULT=PASS\n"
        cases, events = successful_linux()
        result = validate_negative(text, cases, events, validate_probe(text, "negative-handoff"), 12345,
                                   {"exit_code": 1, "host_checkpoints": {"status": "PASS"}})
        self.assertEqual(result["linux_outcome"], "ENABLE4_AND_M1_PASS")

    def test_only_specific_same_pf_allocation_failure_accepted(self):
        text = (firmware("negative-handoff", True) + transport() +
                '{"case":"GUEST_BOOT","status":"PASS"}\n' +
                "toy_pf 0000:01:00.0: not enough MMIO resources for SR-IOV\n" +
                "M1_ERROR stage=enable_four_vfs errno=12 message=Cannot allocate memory\nLAB_RESULT=FAIL\n")
        cases = {"M1_FAILURE": {"status": "FAIL", "errno": 12}}
        probe = validate_probe(text, "negative-handoff")
        result = validate_negative(text, cases, [], probe, 12345, {"exit_code": 3})
        self.assertEqual(result["linux_outcome"], "ENABLE4_REJECTED_MMIO_RESOURCES")
        alterations = [lambda s: s.replace("toy_pf 0000:01:00.0", "toy_pf 0000:02:00.0"),
                       lambda s: s.replace("errno=12", "errno=5"),
                       lambda s: s.replace("enable_four_vfs", "vfio_map"),
                       lambda s: s.replace('"GUEST_BOOT","status":"PASS"', '"GUEST_BOOT","status":"FAIL"'),
                       lambda s: s + "Kernel panic: unrelated error\n"]
        for index, alter in enumerate(alterations):
            with self.subTest(index=index), self.assertRaises(RuntimeError):
                validate_negative(alter(text), cases, [], probe, 12345, {"exit_code": 3})
        with self.assertRaises(RuntimeError):
            validate_negative(text, cases, [{"event": "warmup_enable_snapshot"}], probe, 12345, {"exit_code": 3})


if __name__ == "__main__":
    unittest.main()
