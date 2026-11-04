#!/usr/bin/env python3
"""Record pinned-source API anchors and their exact file hashes."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHORS = [
    ('qemu', 'include/hw/pci/pcie_sriov.h', 'void pcie_sriov_pf_init(', 4),
    ('qemu', 'include/hw/pci/pcie_sriov.h', 'void pcie_sriov_pf_init_vf_bar(', 2),
    ('qemu', 'include/hw/pci/pcie_sriov.h', 'void pcie_sriov_vf_register_bar(', 2),
    ('qemu', 'hw/pci/pcie_sriov.c', 'static void register_vfs(', 40),
    ('qemu', 'hw/pci/pcie_sriov.c', 'static void unregister_vfs(PCIDevice *dev)\n{', 22),
    ('qemu', 'include/hw/pci/pci_device.h', 'static inline AddressSpace *pci_get_address_space(', 26),
    ('qemu', 'include/hw/pci/pci_device.h', 'static inline MemTxResult pci_dma_write(', 7),
    ('qemu', 'hw/pci/pcie.c', 'void pcie_cap_flr_init(', 28),
    ('qemu', 'hw/pci/pcie.c', 'void pcie_acs_init(', 34),
    ('qemu', 'hw/pci/pcie.c', 'void pcie_cap_arifwd_init(', 9),
    ('qemu', 'hw/net/igbvf.c', 'static void igbvf_qdev_reset_hold(', 8),
    ('qemu', 'hw/pci/msix.c', 'void msix_notify(', 36),
    ('qemu', 'pythondeps.toml', '[meson]', 5),
    ('linux', 'drivers/vfio/Kconfig', 'menuconfig VFIO', 54),
    ('linux', 'drivers/vfio/pci/Kconfig', 'config VFIO_PCI\n', 9),
    ('linux', 'lib/Kconfig.kcsan', 'menuconfig KCSAN', 7),
    ('linux', 'lib/Kconfig.kasan', 'config KASAN_GENERIC', 8),
    ('linux', 'include/linux/pci.h', 'void (*remove)(struct pci_dev *dev)', 6),
    ('linux', 'drivers/pci/iov.c', 'int pci_enable_sriov(', 26),
    ('linux', 'drivers/pci/iov.c', 'static int sriov_add_vfs(', 25),
    ('linux', 'drivers/pci/iov.c', 'int pci_iov_add_virtfn(', 78),
    ('linux', 'drivers/pci/iov.c', 'int pci_vfs_assigned(', 34),
    ('linux', 'drivers/vfio/pci/vfio_pci_core.c', 'int vfio_pci_core_enable(', 35),
    ('linux', 'drivers/vfio/pci/vfio_pci_core.c', 'void vfio_pci_core_close_device(', 28),
    ('linux', 'drivers/vfio/pci/vfio_pci_core.c', 'case VFIO_DEVICE_RESET:', 3),
    ('linux', 'drivers/vfio/vfio_iommu_type1.c', 'static int vfio_dma_do_map(', 24),
    ('linux', 'include/uapi/linux/vfio.h', '#define VFIO_GROUP_FLAGS_VIABLE', 2),
    ('linux', 'fs/Makefile', 'kernel_read_file.o mnt_idmapping.o remap_range.o pidfs.o', 1),
    ('linux', 'drivers/firmware/efi/libstub/efi-stub-helper.c', 'efi_status_t efi_load_initrd_dev_path(', 37),
    ('linux', 'drivers/firmware/efi/libstub/efi-stub-helper.c', 'efi_status_t efi_load_initrd(efi_loaded_image_t', 34),
    ('edk2', 'MdeModulePkg/MdeModulePkg.dec', '[PcdsFixedAtBuild, PcdsPatchableInModule, PcdsDynamic, PcdsDynamicEx]', 1),
    ('edk2', 'MdeModulePkg/MdeModulePkg.dec', 'gEfiMdeModulePkgTokenSpaceGuid.PcdSrIovSupport|TRUE|BOOLEAN', 20),
    ('edk2', 'MdeModulePkg/Bus/Pci/PciBusDxe/PciEnumeratorSupport.c', 'if (PcdGetBool (PcdSrIovSupport))', 54),
    ('edk2', 'MdeModulePkg/Bus/Pci/PciBusDxe/PciIo.c', 'if (PciIoDevice->IsPciExp)', 17),
    ('edk2', 'OvmfPkg/OvmfPkgX64.dsc', 'PciSegmentLib|MdePkg/Library/BasePciSegmentLibPci/', 8),
    ('edk2', 'BaseTools/Conf/tools_def.template', '*_GCC5_X64_CC_PATH', 1),
    ('edk2', 'BaseTools/Conf/tools_def.template', '*_GCC_X64_CC_PATH', 1),
    ('edk2', 'BaseTools/Conf/tools_def.template', 'DEBUG_GCC_X64_CC_FLAGS', 1),
]


def main():
    lock = json.loads((ROOT / 'sources.lock.json').read_text())
    evidence = []
    for component, relative, needle, count in ANCHORS:
        entry = lock['sources'][component]
        path = ROOT / entry['path'] / relative
        text = path.read_text()
        offset = text.find(needle)
        if offset < 0:
            raise RuntimeError(f'Pinned source anchor missing: {component}/{relative}: {needle}')
        start = text[:offset].count('\n') + 1
        lines = text.splitlines()
        evidence.append({'component': component, 'commit': entry['commit'], 'path': relative,
                         'line': start, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                         'excerpt': '\n'.join(lines[start - 1:start - 1 + count])})
    ovmf = ROOT / lock['sources']['edk2']['path'] / 'OvmfPkg/OvmfPkgX64.dsc'
    overrides = [line for line in ovmf.read_text().splitlines()
                 if any(pcd in line for pcd in ('PcdSrIovSupport', 'PcdAriSupport', 'PcdSrIovSystemPageSize'))]
    report = {'status': 'PASS', 'scope': 'STATIC_PINNED_SOURCE_INSPECTION_ONLY',
              'build_compatibility': 'NOT_RUN', 'anchors': evidence,
              'ovmf_x64_pcd_overrides': overrides,
              'sources_lock_sha256': hashlib.sha256((ROOT / 'sources.lock.json').read_bytes()).hexdigest()}
    output = ROOT / 'artifacts/sources/source-evidence.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    markdown = ['# Pinned source evidence', '',
                'Quoted upstream source retains its original license and copyright notices; see [third-party notices](../THIRD_PARTY_NOTICES.md).', '',
                'Static inspection is PASS. Compiler compatibility, device isolation, and boot behavior require their milestone run evidence; this file does not mark them VALIDATED.', '',
                'Generated by `python3 tools/source_evidence.py`. Full machine-readable excerpts and SHA256 values: `artifacts/sources/source-evidence.json`.', '',
                '| Component | Commit | Source anchor | SHA256 |', '|---|---|---|---|']
    for item in evidence:
        markdown.append(f'| {item["component"]} | `{item["commit"]}` | `{item["path"]}:{item["line"]}` | `{item["sha256"]}` |')
    markdown.extend(['', '## Adapter decisions supported by these anchors', '',
        '- QEMU SR-IOV PF initialization returns `void` and takes capability offset, VF type name/device ID, initial/total VFs, VF offset, and stride. VF BAR registration takes `(PCIDevice *, region_num, MemoryRegion *)`; size/type are provided on the PF. VF RIDs are PF devfn + offset + i × stride on the same PCI bus. The model must keep all eight RIDs within that bus and verify actual Linux enumeration.',
        '- `pci_dma_read/write` return `MemTxResult` and route through the supplied device address space. Every toy VF DMA must pass that VF device and check the result. PF/global-memory DMA is not equivalent.',
        '- FLR initialization only advertises capability. The config-write path must call `pcie_cap_flr_write_config`, which invokes `pci_device_reset`. The toy reset callback must fence and invalidate its own queues. IGB VF uses `ResettableClass.phases.hold`; its example cannot be assumed to provide the toy lifecycle.',
        '- QEMU endpoint ACS helper advertises zero optional endpoint ACS bits when there is no peer-to-peer support; downstream ports advertise redirection and related capabilities. This does not prove distinct guest IOMMU groups: M1 must observe groups and establish separate TYPE1 containers.',
        '- MSI-X notify honors masks and sets PBA. `msix_reset` clears vectors, table, PBA, and enable state; using it for every software fence would also destroy tenant MSI-X setup. The adapter must explicitly prevent stale pending interrupts without pretending a masked notification vanished.',
        '- Linux 6.12 classic VFIO has `VFIO_GROUP`, `VFIO_CONTAINER`, and `VFIO_IOMMU_TYPE1`. Selecting `IOMMUFD=n` with `VFIO=y` selects the classic interfaces. `VFIO_NOIOMMU` is prohibited in the lab config. Group viability alone is insufficient without actual container/TYPE1/MAP/UNMAP.',
        '- KCSAN explicitly depends on `!KASAN`; separate final `.config` checks are required. `pidfs.o` is unconditional in `fs/Makefile`, with pidfd implementations in the kernel; there is no invented `CONFIG_PIDFD` switch.',
        '- PCI driver remove is `void`; `sriov_configure` is `int`. VFIO core attempts reset on enable, performs reset/cleanup on disable, and routes `VFIO_DEVICE_RESET` to its reset handler. A fresh VFIO open must re-latch epoch/session.',
        '- EDK2 `PcdSrIovSupport` is BOOLEAN TRUE; `PcdAriSupport` is BOOLEAN TRUE; `PcdSrIovSystemPageSize` is UINT32 0x1. Their DEC section supports FixedAtBuild/PatchableInModule/Dynamic/DynamicEx. PciBusDxe consumes these PCDs. OvmfPkgX64.dsc has no direct overrides of these three names. Build variants must confirm effective values in generated reports.',
        '- PciIoVerifyConfigAccess has an express-specific extended config path and enforces limits. Probe must test EFI status; PciSegmentLib is available as the recorded OVMF binding when a fallback is needed.',
        '- Both GCC and GCC5 toolchain tags exist in the pinned BaseTools template and point X64 CC at gcc; GCC is selected for the first native build. This is tag availability, not GCC 15 compatibility validation.',
        '- Linux EFI stub first attempts the Linux initrd vendor device path plus LoadFile2 protocol, then falls back to `initrd=` when no protocol is found. A handoff loader can explicitly install LoadFile2, or supply a valid same-ESP path and verify the fallback message at boot. BLK_DEV_INITRD and EFI_STUB must be enabled.',
        '- QEMU accepts system Meson >=1.5.0. Its pinned source bundles Meson 1.5.0 and pycotap 1.3.1 wheels; hashes and any actual venv installation belong in lock/build evidence. Meson Git wraps are separate from Git submodules and are pre-fetched into `downloads/qemu-subprojects`.',
        '', '## Exact API excerpts', ''])
    for item in evidence:
        markdown.extend([f'### {item["component"]}: {item["path"]}:{item["line"]}', '', '```text', item['excerpt'], '```', ''])
    (ROOT / 'docs/SOURCE_EVIDENCE.md').write_text('\n'.join(markdown))
    print(json.dumps({'status': 'PASS', 'anchors': len(evidence), 'output': str(output)}))


if __name__ == '__main__':
    main()
