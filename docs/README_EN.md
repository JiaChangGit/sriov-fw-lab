# SR-IOV firmware and platform lab

Linux driver and virtualization interview project for a 16 GB laptop, WSL2 Ubuntu 26.04 and QEMU TCG.
Supported scope: **M0–M3 / T01–T10**, covering the portable core, QEMU device, PF driver, VFIO tenants and UEFI handoff.

The portable C core owns the device VF state machine, mailbox, quota and scheduler.
The PF driver coordinates transactions; tenants own independent VFIO containers, mappings and eventfds.
The IOMMU restricts addresses, the fence controls DMA timing, and generations invalidate stale work.
Guest Linux executes actual PCI/VFIO/IOMMU paths against an emulated FILL/CRC32 device.

After dependency installation: `make env-check`, `make fetch-sources`, `make verify-project JOBS=2`, `make build-handbook`.
See [tutorial](BUILD_TUTORIAL_ZH.md), [requirements](REQUIREMENTS.md), [validation](VALIDATION.md) and [offline handbook](handbook.html).
Normal guests use 1536 MiB; KASAN guests use 2048 MiB. After validation, use `make demo` and `make demo-reset`.

T05 strict WRITE-only rejection requires an explicit toy-only guest kernel patch, not stock Linux behavior.
No physical passthrough, real-device performance, KVM acceptance, firmware flashing or secure update is claimed.
