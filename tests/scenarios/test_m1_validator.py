#!/usr/bin/env python3
"""Synthetic parser unit fixtures only; these are never guest PASS evidence."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from run_vm import validate_m1_payload


def fixture():
    pf = '0000:01:00.0'
    vfs = [f'0000:01:01.{i}' for i in range(4)]
    groups = [dict(vf=vf, group_id=i + 10, members=[vf]) for i, vf in enumerate(vfs)]
    cases = {
        'M1_CAPS': dict(status='PASS', pf_vectors=2, vf_vectors=2, vfs=4, unique_requesters=True, flr_declared=True),
        'M1_GROUPS': dict(status='PASS', pf=pf, groups=groups),
        'M1_TYPE1': dict(status='PASS', viable_groups=4, simultaneous_containers=4, maps=4, exact_unmaps=4,
                        remaining_fds=0, remaining_maps=0, vfio_reset_epoch_checked=True),
        'T01': dict(status='PASS', cycles=100, vfs_per_cycle=4, final_ledger=0,
                    remaining_vf_functions=0, remaining_vfio_fds=0, remaining_vfio_maps=0, fd_growth=0),
    }
    events = [dict(event='function_caps', bdf=pf, kind='PF', msix_offset=0x50, pcie_offset=0x80,
                   ari_offset=0x100, acs_offset=0x200, sriov_offset=0x140, vectors=2, flr=True,
                   raw_vendor=0x1234, raw_device=0xe001, enumerated_vendor=0x1234, enumerated_device=0xe001),
              dict(event='sriov_capability', pf=pf, offset=0x140, total_vfs=8, initial_vfs=8,
                   first_vf_offset=8, stride=1, supported_page_sizes=1, system_page_size=1)]
    for i, vf in enumerate(vfs):
        events += [dict(event='function_caps', bdf=vf, kind='VF', msix_offset=0x50, pcie_offset=0x80,
                        ari_offset=0x100, acs_offset=0x120, vectors=2, flr=True,
                        raw_vendor=0xffff, raw_device=0xffff, enumerated_vendor=0x1234, enumerated_device=0xe002),
                   dict(event='group_member', group=i + 10, bdf=vf, driver='none', **{'class': 0x058000}),
                   dict(event='vf_link', vf=vf, physfn=pf, rid=0x108 + i, group=i + 10),
                   dict(event='vfio_open', vf=vf, group=i + 10, container_fd=i * 3 + 5, vectors=2, reset_supported=True),
                   dict(event='vfio_map', vf=vf, size=4096, permissions='READ_WRITE'),
                   dict(event='vfio_unmap', vf=vf, size=4096),
                   dict(event='vfio_reset_observed', vf=vf, target=i == 0, before_epoch=2, after_epoch=3 if i == 0 else 2, hw_flags=1)]
    for cycle in range(1, 101):
        events += [dict(event='enable_snapshot', cycle=cycle, enabled=4, ledger_credit=32),
                   dict(event='disable_snapshot', cycle=cycle, enabled=0, ledger_credit=0, outstanding=0, vf_links=0)]
    return cases, events


class ValidationTests(unittest.TestCase):
    def test_complete_fixture(self):
        cases, events = fixture()
        self.assertEqual(validate_m1_payload(cases, events)['enable_disable_cycles_checked'], 100)

    def test_case_labels_alone_rejected(self):
        cases = {name: dict(status='PASS') for name in ('M1_CAPS', 'M1_GROUPS', 'M1_TYPE1', 'T01')}
        with self.assertRaises(RuntimeError):
            validate_m1_payload(cases, [])

    def test_missing_or_corrupted_initial_function_caps_rejected(self):
        cases, events = fixture()
        cap_indices = [index for index, event in enumerate(events) if event['event'] == 'function_caps']
        self.assertEqual(len(cap_indices), 5)
        for index in cap_indices:
            with self.subTest(index=index, damage='missing'), self.assertRaises(RuntimeError):
                validate_m1_payload(cases, [event for number, event in enumerate(events) if number != index])
            for field in ('ari_offset', 'acs_offset'):
                for invalid in (None, 0, 0xff, 0x101, 0xffc, True):
                    changed = copy.deepcopy(events)
                    changed[index][field] = invalid
                    with self.subTest(index=index, field=field, invalid=invalid), self.assertRaises(RuntimeError):
                        validate_m1_payload(cases, changed)

    def test_extended_capability_identity_and_layout_rejected(self):
        cases, events = fixture()
        alterations = [
            lambda e: e[0].update(sriov_offset=0x100),
            lambda e: e[0].update(acs_offset=0x144),
            lambda e: e[0].update(ari_offset=0x200),
            lambda e: e[0].pop('sriov_offset'),
            lambda e: e[1].update(offset=0x180),
            lambda e: e[1].update(total_vfs=4),
            lambda e: e[1].update(system_page_size=0),
            lambda e: e[2].update(sriov_offset=0x140),
            lambda e: e[2].update(flr=False),
            lambda e: e[2].update(enumerated_device=0xe001),
            lambda e: e.append(copy.deepcopy(e[0])),
        ]
        for number, alter in enumerate(alterations):
            changed = copy.deepcopy(events)
            alter(changed)
            with self.subTest(number=number), self.assertRaises(RuntimeError):
                validate_m1_payload(cases, changed)

    def test_semantic_corruptions(self):
        cases, events = fixture()
        alterations = [
            lambda c, e: c['T01'].update(cycles=99),
            lambda c, e: c['T01'].update(final_ledger=8),
            lambda c, e: c['M1_GROUPS']['groups'][1].update(group_id=10),
            lambda c, e: c['M1_GROUPS']['groups'][0]['members'].append(c['M1_GROUPS']['pf']),
            lambda c, e: c['M1_GROUPS']['groups'][0]['members'].append(c['M1_GROUPS']['groups'][1]['vf']),
            lambda c, e: c['M1_TYPE1'].update(remaining_maps=1),
            lambda c, e: next(x for x in e if x['event'] == 'vfio_unmap').update(size=0),
            lambda c, e: next(x for x in e if x['event'] == 'vfio_reset_observed').update(after_epoch=2),
            lambda c, e: next(x for x in e if x['event'] == 'disable_snapshot').update(ledger_credit=1),
            lambda c, e: e.pop(),
        ]
        for number, alter in enumerate(alterations):
            with self.subTest(number=number):
                changed_cases, changed_events = copy.deepcopy(cases), copy.deepcopy(events)
                alter(changed_cases, changed_events)
                with self.assertRaises(RuntimeError):
                    validate_m1_payload(changed_cases, changed_events)


if __name__ == '__main__':
    unittest.main()
