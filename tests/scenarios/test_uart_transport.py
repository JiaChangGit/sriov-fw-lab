"""Regression for printk splitting M1 JSON; synthetic checks, not VM evidence."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from run_uefi import parse_serial, read_uefi_output, validate_handoff, validate_probe
from run_vm import validate_m1_payload
from test_m1_validator import fixture
from test_m2_controller import firmware, transport, successful_linux


class UartTransportTests(unittest.TestCase):
    def test_interleaved_clocksource_cannot_be_accepted_as_complete_evidence(self):
        cases, events = fixture()
        lines = []
        damaged = False
        for event in events:
            line = json.dumps(event)
            if event.get("event") == "function_caps" and event.get("kind") == "VF" and not damaged:
                # The real failure inserted printk immediately after the bdf
                # opening quote. Do not teach the parser to repair such bytes.
                position = line.index('"bdf": "') + len('"bdf": "')
                line = line[:position] + "clocksource: tsc: mask: 0xffffffffffffffff\n" + line[position:]
                damaged = True
            lines.append(line)
        self.assertTrue(damaged)
        _, parsed = parse_serial("\n".join(lines))
        with self.assertRaisesRegex(RuntimeError, "capability observations"):
            validate_m1_payload(cases, parsed)

    def test_split_uart_preserves_firmware_handoff_and_linux_records(self):
        cases, events = successful_linux()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            boot = firmware() + transport() + "clocksource: Switched to clocksource tsc\n"
            serial = "\n".join(json.dumps(row) for row in events) + "\n"
            serial += "\n".join(json.dumps(dict(value, id=name)) for name, value in cases.items())
            (directory / "kernel.log").write_text(boot)
            (directory / "serial.log").write_text(serial)
            text = read_uefi_output(directory, "handoff")
            parsed_cases, parsed_events = parse_serial(text)
            checked = validate_handoff(text, parsed_cases, parsed_events, validate_probe(text, "handoff"),
                                       12345, {"status": "PASS"})
            self.assertEqual(len(checked["linux_resource_relations"]), 10)
            self.assertEqual((directory / "kernel.log").read_text(), boot)
            self.assertEqual((directory / "serial.log").read_text(), serial)
            # Separation must not hide kernel diagnostics from validation.
            (directory / "kernel.log").write_text(boot + "WARNING: injected kernel diagnostic\n")
            text = read_uefi_output(directory, "handoff")
            with self.assertRaisesRegex(RuntimeError, "kernel diagnostics"):
                validate_handoff(text, parsed_cases, parsed_events, validate_probe(text, "handoff"),
                                 12345, {"status": "PASS"})

    def test_missing_or_empty_kernel_uart_is_rejected_for_linux_boots(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "serial.log").write_text("LAB_RESULT=PASS\n")
            for mode in ("handoff", "negative"):
                with self.subTest(mode=mode), self.assertRaises(OSError):
                    read_uefi_output(directory, mode)
            (directory / "kernel.log").write_text("")
            for mode in ("handoff", "negative"):
                with self.subTest(mode=mode), self.assertRaisesRegex(RuntimeError, "UART evidence missing"):
                    read_uefi_output(directory, mode)

    def test_probe_only_does_not_require_a_linux_uart(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            text = firmware("probe")
            (directory / "serial.log").write_text(text)
            self.assertEqual(read_uefi_output(directory, "probe"), text)


if __name__ == "__main__":
    unittest.main()
