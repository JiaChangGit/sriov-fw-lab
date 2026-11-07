# Design decisions / 設計決策

The original HTML v3.0 was the historical design input. The project release is scoped by REQUIREMENTS.md; see D016 below. Decisions below resolve
implementation details; they do not remove required gates. Status statements here
are design decisions, not test results.

## D001 — Environment and resource limits

- Build host: the existing `Ubuntu-26.04` WSL2 distribution, native ext4 repository.
- Heavy upstream builds are serialized, default `JOBS=2`; one QEMU guest at a time.
- Existing Windows `.wslconfig` is preserved. It currently limits memory to
  9,319,743,488 bytes and enables `debugConsole`; no processor override is present.
  The v3.0 recommendation is 8 GiB / 6 processors / 4 GiB swap. This difference is
  recorded in the environment report, not silently represented as the recommendation.
- No WSL shutdown, host driver installation, BIOS, boot-security or system-Python
  change is needed for the project.
- PCI class selected for the emulated controller: `0x058000` (other memory
  controller), revision `0x01`; IDs remain the spec's unregistered teaching IDs.

## D002 — Mailbox protocol rejection versus completed transaction

Section 09 requires reply caching, while T13 requires an unsupported protocol
request to preserve context. Protocol-rejected requests (`INVAL`, `UNSUP`,
`NO_NEG`, `SEQ_CONFLICT`, `STALE_SEQ`) leave the complete core context unchanged,
including the last-completed request cache. Accepted transactions cache their
completion, including execution state/device failures, for identical retries.
Illegal direct FSM events preserve the full context byte for byte. Tests must
verify both properties. Hardware fencing is a separate adapter operation and is
not undone by a firmware protocol rejection.

## D003 — Provenance and completion

Source resolution is distinct from build validation: a resolved commit is not a
validated compiler/runtime combination. Failed, blocked and unexecuted tests are
never treated as success. Logs retain the actual command, exit code and hashes.

## D004 — Source line endings

The existing WSL user's Git configuration has `core.autocrlf=true`. The first
QEMU checkout consequently contained CRLF shell scripts, including `configure`.
Source acquisition must force `core.autocrlf=false` locally and for recursive
submodule commands, and normalize pristine checkouts before any build. The
user's global Git configuration is preserved. The upstream commit remains the
same; this is a checkout setting, not a source patch.

## D005 — READY deadline and removed PCI functions

Pinned Linux v6.12 `drivers/pci/iov.c:sriov_disable()` removes the VF functions and
then waits one second before returning to the PF driver. A VFIO close can first
perform FLR and leave the slot in RECOVERING. Applying the 1000-tick tenant READY
deadline after the PCI function has disappeared would incorrectly KILL that slot
during normal disable, before the driver can issue DESTROY.

The READY deadline therefore applies only while hardware reports `pci_present`.
An absent RECOVERING slot retains its authoritative state, quota and fence until
DESTROY. Present-VF 999/1000/1001-tick boundaries remain enforced. QUIESCING and
RESETTING deadlines are unchanged. Native tests and the independent oracle cover
this distinction; the complete affected M0 gate is rerun.

## D006 — Offline QEMU Meson wrap overlays

The pinned softfloat/testfloat wrap files specify `patch_directory`. Offline
source copies must also copy those pinned `subprojects/packagefiles` overlays;
the upstream Git trees alone do not contain the required Meson build files.
The build tool applies these source-provided overlays without network access.

## D007 — Pin GCC 14 for the Linux v6.12 guest builds

The first stress kernel build with Ubuntu GCC 15.2 failed in the x86 EFI stub.
The pinned `drivers/firmware/efi/libstub/Makefile` replaces the normal x86
`KBUILD_CFLAGS` and does not preserve an explicit C dialect option. The compiler
therefore used its C23 default, which conflicts with this kernel's `false`
enumeration constant and `bool` typedef. The actual errors and failed exit are
retained in `artifacts/build-kernel/20261007T154311Z-w2gl3ipa/`.

The kernel builder explicitly selects `CC=gcc-14` and `HOSTCC=gcc-14` for the
guest kernel, its tools and external PF module. The available Ubuntu package is
`gcc-14` version `14.3.0-14ubuntu1`, compiler version 14.3.0 and target
`x86_64-linux-gnu`; `sources.lock.json.kernel_toolchain` records the selection.
This retains the pinned Linux source and the required EFI stub configuration.
It does not change the system default compiler or establish that GCC 14 succeeds:
the replacement build, guest boot and affected gates must supply their own
results. QEMU and native core compiler evidence remains separately recorded.

## D008 — Steady-state object inventory after explicit requester warm-up

The first real M1 run completed 100 enable/disable cycles but failed the host
object inventory assertion: 340 cold objects became 360. Pinned QEMU
`hw/i386/intel_iommu.c:vtd_find_add_as` caches five memory-region objects per
(bus, devfn, pasid); four newly used VF requesters explain these 20 objects.
The failed run remains `artifacts/m1/20261008T162441Z-o7m628zi/`.

The test now records the cold inventory, performs one explicit uncounted warm-up
cycle with all capability/VFIO checks, records a disabled baseline, then runs
100 complete measured cycles. Initial/final full machine child-object inventories
and PCI requesters must match exactly; all four device-owned work counters must
be zero. No ongoing growth is ignored. Guest PF has no deferred work or timer
allocation at M1; its synchronous transactions and descriptor counts are checked.

## D009 — Backport the upstream NASM 3 X64 PUSH fix

The first full positive OVMF build failed with the installed NASM 3.01:
`UefiCpuPkg/Library/CpuExceptionHandlerLib/X64/ExceptionHandlerAsm.nasm`
used `push strict dword` in 64-bit code. The preserved failure is
`artifacts/build-edk2/20261009T022324Z-lat9fnjj/positive.log`.

Upstream EDK2 commit
[`9ccf8751a74f26142e584c7b7c7572a182b67997`](https://github.com/tianocore/edk2/commit/9ccf8751a74f26142e584c7b7c7572a182b67997)
changes these two operands to `strict qword`. The project backports those exact
instruction changes in `uefi/patches/0001-nasm3-x64-push.patch`, applied only to
the isolated build worktree. The source commit remains edk2-stable202411;
`downloads/edk2` is unchanged. The source lock records upstream attribution,
patch hash, and exact original/patched file hashes. The builder rejects any
different patch or target content and records application evidence.

The pinned upstream assembly file contains CRLF bytes in Git itself. This
patch preserves those context and output bytes and uses a narrow `-text`
Git attribute for `uefi/patches/*.patch`; it does not change global checkout
settings or normalize upstream source. The initial LF-context patch attempt
failed cleanly and is retained in `artifacts/build-edk2/20261009T022547Z-ein4_cny/`.

## D010 — Keep ACS outside the pinned SR-IOV reset write range

The corrected UEFI probe reached ARI at 0x100 and SR-IOV at 0x140, then rejected
the zero header at the advertised next capability. Pinned QEMU v9.2.0
`hw/pci/pcie_sriov.c:pcie_sriov_pf_reset()` loops over `PCI_NUM_REGIONS` (7)
and writes eight bytes for each four-byte BAR offset. With SR-IOV at 0x140,
the final write at 0x17c extends through 0x183, overwriting ACS at 0x180.

The device now places PF ACS at 0x200. Its header, controls and linked-list
pointer remain handled by QEMU's standard helper; no PCI or MMIO ABI register
changes. M1 is strengthened to check ARI and ACS on the PF and every VF, and
the affected device build and M1 gate must pass again before M2 continues.
The diagnostic failure is retained in
`artifacts/m2-probe/20261009T130032Z-zktgskjg/`.

## D011 — Stable Python hash seed for incremental EDK2 builds

The pinned EDK2 `BaseTools/Source/Python/AutoGen/GenC.py` selects each module's
`STACK_COOKIE_VALUE` with `hash(Info.Guid) % len(cookie_values)`. Python's default
per-process hash randomization therefore changes generated `AutoGen.h` files
between otherwise identical builds. Every compiled source includes that header,
so an unchanged OVMF build recompiled almost all modules, including OpenSSL.
The repeated build is retained in
`artifacts/build-edk2/20261009T180208Z-llo33trc/`. Read-only diagnosis confirmed
that Conf templates, DSC files and persisted cookie-value tables retained their
old timestamps while the generated headers changed.

The builder now explicitly sets `PYTHONHASHSEED=0` in its subprocess environment
and records it in the build report. Existing cookie values remain unchanged;
the fixed seed makes selection from those values stable. Unchanged Conf files,
generated DSC files and toolchain symlinks also retain their timestamps. Source
or configuration changes still enter the normal EDK2 dependency checks. One
transition build is expected before the next unchanged build can reuse objects.

The transition build passed at `artifacts/build-edk2/20261009T180755Z-zejy7bxp/`.
The unchanged rebuild passed at `artifacts/build-edk2/20261010T023420Z-uk63gdw9/`:
positive/negative OVMF steps took 11.75/11.25 seconds instead of 115.75/109.34
seconds. Probe, CODE and VARS hashes were identical between the two builds.
The comparison and input hashes are saved in
`artifacts/edk2-incremental/20261010T023823Z-u953zmyh/result.json`.

## D012 — Resolve EDK2 protocol BAR indices from physical BAR layout

The probe at `artifacts/m2-probe/20261010T023836Z-_uoy5wg0/` passed the complete
PF capability chain and BAR0 validation, then received EFI_UNSUPPORTED for
`GetBarAttributes(2)`. In pinned EDK2, `GatherDeviceInfo()` in
`MdeModulePkg/Bus/Pci/PciBusDxe/PciEnumeratorSupport.c` advances its array index
once per `PciParseBar()` call, while a 64-bit BAR advances the config offset by
eight bytes. `PciIoGetBarAttributes()` uses that array index. Consequently this
device's physical BAR0 and BAR2 correspond to protocol indices 0 and 1.

The probe now walks the physical config BAR types to obtain the protocol index,
retains physical BAR numbers in its evidence, and verifies returned length,
type and translated base against the actual config BAR. It never writes all-ones
to size a live BAR or assumes a successful return from another index is enough.
The original failed boot remains recorded; the corrected runtime must pass.

## D013 — Keep kernel diagnostics separate from M3 structured records

`artifacts/m3-case/20261011T132643Z-n6tck8bh/` contained all nine driver failpoint
hits, but printk interrupted three JSON lines on the shared UART. The semantic
validator rejected the result. M3 direct boots now send kernel console output
to UART0 (`kernel.log`) and init/labd/tenant communication to UART1 (`serial.log`).
Both are preserved and hashed, the kernel boot signature is required, and both
are scanned for unexpected kernel diagnostics. This changes log transport, not
fault-hit requirements. M1 and UEFI handoff retain their original console path.

## D014 — Scoped guest-kernel hook for partial PCI enumeration failure

Returning an error before `pci_enable_sriov()` cannot exercise Linux's partial
enumeration cleanup. The pinned Linux `sriov_add_vfs()` loops over real
`pci_iov_add_virtfn()` calls and removes earlier VFs through its `failed:` path.
The SHA256-pinned patch in `patches/linux/` injects ENOMEM at that loop boundary,
only for vendor/device 1234:e001, once at the requested VF number. A root-only
debugfs control accepts 0–8 and is consumed on a hit. The unchanged upstream
cleanup path executes; the PF driver must independently unwind its firmware
quota. A paired firmware destroy failure tests retained quota and recovery.

The patch is applied to a separate `build/linux-src` Git worktree. The upstream
download remains pristine. Build reports record source commit, patch hashes,
patched-file hashes and the actual source diff; runtime prerequisite checks
reject changed patch/build inputs. This is an explicit test hook in the offline
guest kernel; it is neither installed into WSL nor a systemwide random allocator
failure. Runtime acceptance requires actual kernel hit logs and all PCI, quota
and object cleanup evidence; compilation alone does not approve T02.

## D015 — Explicit toy-only strict WRITE-only guest IOMMU mode

The stock pinned Linux v6.12 Intel driver accepts `VFIO_DMA_MAP_FLAG_WRITE` but
adds `DMA_FL_PTE_PRESENT` unconditionally in `__domain_mapping()`
(`drivers/iommu/intel/iommu.c:1803`). That bit aliases `DMA_PTE_READ`
(`drivers/iommu/intel/iommu.h:43,48`), so the resulting second-level mapping is
read/write. This is an upstream policy, not a missing check in the toy DMA
helper. The original authors describe the first-/second-level consistency
decision in this [upstream discussion](https://lkml.rescloud.iu.edu/2103.1/01108.html).

The real T05 WRITE-only run at
`artifacts/m3-case/20261012T163725Z-at60ieyh/` remains **FAIL**: actual VF RID 0101
successfully read eight bytes through the requested WRITE-only mapping and
published an OK completion. Requested VFIO flags alone do not prove effective
read isolation. T05 now rejects an observed OK completion immediately instead
of waiting for a fault timeout.

`patches/linux/0002-toy-vfio-strict-write-only.patch` adds the default-off boot
option `toy_iommu_strict_wo=1`. It preserves separate R/W bits only for an
unmanaged, non-nested, second-level domain containing exactly one PCI VF with
vendor/device 1234:e002. Device membership is read under the existing domain
lock. First-level translation, other devices/domains and boots without the
option retain upstream behavior. The patch changes real IOMMU page-table
permissions; it does not inject a toy DMA error or increment a counter.

The patch is SHA256-pinned and applied only to `build/linux-src`. Direct and
UEFI lab boots explicitly enable the option. T05 requires its kernel startup
marker in addition to actual failed DMA, correct requester/counters, guard
checks and healthy VF work. Results in this mode must be described as using
the custom lab guest kernel, never as stock Linux WRITE-only enforcement.
The upstream download and Windows/WSL kernels are unchanged. All six strict-mode
T05 development runs passed. The affected-build regression then passed at
`artifacts/m3-checkpoint/20261013T005556Z-56ebu4kg/`, including both M1 profiles,
M2, T01/T02/T03/T04/T05/T06/T09/T10 and 32 validator tests.
The initial aggregate exposed a T01 serial-close deadline issue; its failure
and subsequent 100-cycle validations are recorded in BUG_LOG B001.

## D016 — Scope the laptop project release to M0–M3

The supported configuration is M0–M3/T01–T10 on a 16 GB RAM system with WSL2 Ubuntu 26.04 and QEMU TCG.
This keeps the project reproducible without a physical SR-IOV adapter. KCSAN/KVM, persistent firmware updates and the M4/M5 acceptance matrix are outside the supported scope.
Reserved wire values return UNSUP and retain their ABI numbers.

REQUIREMENTS.md defines acceptance. The full gate checks M0/M3, controller tests and native owner regressions.
Build evidence records whether execution started empty or reused existing products; interrupted attempts remain separate.
The original specifications are archived for reference, while current documentation describes the implemented configuration.

## D017 — Separate kernel and protocol UARTs for every Linux boot

The 2026-10-14 handoff run `artifacts/m2-handoff/20261014T145421Z-4j_x1q0l/`
failed semantic validation despite the guest final PASS. A delayed clocksource printk
split VF4's function_caps JSON. D013 had separated only M3 output, leaving M1 and
UEFI handoff exposed to the same shared-channel defect.

All Linux boots now keep console output on UART0 and the test protocol on UART1.
For UEFI handoff, UART0's kernel.log preserves firmware, EFI stub and Linux ordering;
UART1's serial.log retains JSON and checkpoint ACKs. Probe-only retains its UART0
socket because it never enters Linux. Semantic parsing can read both logs, but the
raw files remain separate and hashed, and their concatenation is not a global timeline.
Kernel reports still reject acceptance. Saved-run validation requires both channels.

M1 host_checkpoint must also use the stdin device inherited from init, rather than
reopening /dev/console. The first migration attempt timed out waiting on the old
channel; its failure is preserved at `artifacts/m1/20261015T162947Z-_q0ku6op/`.
Use a separate open file description so nonblocking flags do not alter inherited
stdin, and restore the UART termios on both success and failure. See BUG_LOG B020
and VALIDATION.md for actual regression results.
