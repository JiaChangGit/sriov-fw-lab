# SR-IOV firmware and platform lab

Linux driver and virtualization lab for a 16 GB laptop, WSL2 Ubuntu 26.04 and QEMU TCG.
Supported scope: **M0–M3 / T01–T10**, covering the portable core, QEMU device, PF driver, VFIO tenants and UEFI handoff.

The portable C core owns the device VF state machine, mailbox, quota and scheduler.
The PF driver coordinates transactions; tenants own independent VFIO containers, mappings and eventfds.
The IOMMU restricts addresses, the fence controls DMA timing, and generations invalidate stale work.
Guest Linux executes actual PCI/VFIO/IOMMU paths against an emulated FILL/CRC32 device.

After dependency installation: `make env-check`, `make fetch-sources`, `make verify-project JOBS=2`, `make build-handbook`.
Start with the [reading path](LEARNING_PATH_ZH.md), then the [build tutorial](BUILD_TUTORIAL_ZH.md),
[tools](TOOLS_DEEP_DIVE_ZH.md), [code trace](TRACE_CODE_ZH.md), [GDB exercises](TRACE_PRACTICE_ZH.md)
and [debugging guide](DEBUGGING_ZH.md). The detailed teaching chapters are in Traditional Chinese.
Markdown chapters render directly on GitHub. Download the [offline handbook](handbook.html) and open it in a browser.
See also [requirements](REQUIREMENTS.md) and [validation](VALIDATION.md).
Normal guests use 1536 MiB; KASAN guests use 2048 MiB. After validation, use `make demo` and `make demo-reset`.

T05 strict WRITE-only rejection requires an explicit toy-only guest kernel patch, not stock Linux behavior.
No physical passthrough, real-device performance, KVM acceptance, firmware flashing or secure update is claimed.
