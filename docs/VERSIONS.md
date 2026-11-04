# Pinned versions and compatibility evidence

Current scope is defined by [REQUIREMENTS.md](REQUIREMENTS.md). Exact source commits, recursive submodule commits,
patch hashes, Meson wrap inputs and wheel hashes are recorded in
[sources.lock.json](../sources.lock.json). A fetched commit or source inspection
PASS is distinct from a successful build and from an accepted guest runtime.
The current complete acceptance is recorded in [VALIDATION.md](VALIDATION.md).
Historical build failures below explain the selected toolchains.

| Component | Pinned ref | Exact source commit | Source inspection / build evidence | Runtime evidence |
|---|---|---|---|---|
| QEMU | `v9.2.0` | `ae35f033b874c627d81d51070187fbf55f0bf1a7` | SR-IOV, VF DMA, FLR, MSI-X, ACS/ARI source anchors PASS; native GCC 15.2 compile and device-help smoke PASS | M1 TCG PASS: four independent groups/TYPE1 containers, exact MAP/UNMAP and 100 cycles |
| Linux | `v6.12` | `adc218676eef25575469234709c2d87185ca223a` | Classic VFIO/PCI/EFI/Kconfig source anchors PASS; GCC 15.2 full build FAIL; GCC 14.3 kernel/module build PASS | Stress and KASAN M1, complete M3 TCG PASS; KCSAN outside scope |
| EDK2 | `edk2-stable202411` | `0f3867fa6ef0553e26c42f7d71ff6bdb98429742` | GCC 14/GCC5 tag, NASM 3 upstream backport, generated positive/negative PCD checks PASS | M2 TCG PASS: probe/selftest, continuous Linux handoff and disabled-PCD observation |
| BusyBox | `1_36_1` | `1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4` | GCC 15.2 with explicit gnu99 build PASS; minimal configuration checked | Initramfs/M1 TCG PASS |

All four source checkouts and their declared recursive submodules were fetched.
The QEMU annotated tag object is `55162f76c8d6698c22d9e805f29b1faf15d9724a`
and the Linux annotated tag object is
`06090c9b622a7e1f797e775db4c035e0d779b76e`; these are not the peeled source
commits listed above. Linux v6.12 is this offline teaching baseline, not a claim
of current security maintenance.

| Tool / environment | Recorded version | Use and actual result |
|---|---|---|
| Host | Windows 11 + WSL2 Ubuntu 26.04.1 | Environment inspection PASS; native ext4 build directory |
| GCC | Ubuntu `15.2.0-16ubuntu1`, GCC 15.2.0, `x86_64-linux-gnu` | Native C11 core M0 PASS; QEMU build PASS; Linux EFI-stub build FAIL |
| GCC 14 | Ubuntu `14.3.0-14ubuntu1`, GCC 14.3.0, `x86_64-linux-gnu` | Explicitly pinned kernel `CC`/`HOSTCC`; stress build and M1 guest PASS (D007) |
| Clang | Ubuntu 21.1.8 (`6ubuntu1`), `x86_64-pc-linux-gnu` | M0 ASan/UBSan and libFuzzer PASS |
| Python | 3.14.4 | Source/build/controller tools; actual QEMU configure completed |
| Meson | 1.10.1 | Actual system Meson accepted by the pinned QEMU configure (`>=1.5.0`) |
| Ninja | 1.13.2 | Actual QEMU build completed with two jobs |
| pycotap | 1.3.1 | QEMU configure log records installation in `build/qemu/pyvenv` |
| EDK2 toolchain tag | `GCC5` with GCC 14.3, NASM 3.01 and IASL 20251212 | Positive/negative OVMF and probe compile PASS; PYTHONHASHSEED=0 recorded (D011) |

QEMU also bundles Meson 1.5.0 and pycotap 1.3.1 wheels. Their exact SHA256 values
are in the lock. Bundled availability does not mean both wheels were installed:
the successful configure used system Meson 1.10.1 and installed pycotap 1.3.1 in
the project build venv. The actual configure log is the installation evidence;
wheel status fields in the lock must be kept consistent with that evidence.
No system Python packages were installed with pip.

The QEMU integration patch is
`patches/qemu/0001-toy-sriov-build.patch`, SHA256
`679fac52a9bdad6220ba6eeb43fa41f3411cca61bad0186df60461a89146fe75`.
The historical 2026-10-09 build's binary SHA256 is
`46507ba8e7d7a170bfd777efd3efd7fb76b8dabc1e66c53d1989d762b7069640`.
Build outputs are associated with their recorded input hashes; these values do
not authorize reusing a binary after an affected source change.

Historical evidence locations (current affected-source acceptance is tracked in VALIDATION.md):

- Static API/PCD/Kconfig evidence: [SOURCE_EVIDENCE.md](SOURCE_EVIDENCE.md) and
  `artifacts/sources/source-evidence.json`, with exact source anchors and hashes.
- Earlier complete M0 gate: `artifacts/m0/20261009T180814Z-59mo6bfz/result.json`.
  Full affected-core rerun: `artifacts/verify-core/20261007T005515Z-v5p454gl/`.
- QEMU successful build/configure/device-help:
  `artifacts/build-qemu/20261009T180051Z-45a14hyq/`. The earlier configuration
  failure is retained at `artifacts/build-qemu/20261007T005337Z-xhfparax/` (D006).
- Linux configuration-only run:
  `artifacts/build-kernel/20261006T163106Z-q61osh75/`. Failed GCC 15 full build:
  `artifacts/build-kernel/20261007T154311Z-w2gl3ipa/` (D007). GCC 14 kernel/module
  with PCI_MMCONFIG passed at `artifacts/build-kernel/20261008T005830Z-j07gwf5d/`.
- BusyBox/initramfs: `artifacts/build-initramfs/20261010T023451Z-zxaz50p_/`.
  Strengthened M1 TCG: `artifacts/m1/20261010T023628Z-alfh_i9y/`.
- EDK2 build: `artifacts/build-edk2/20261010T024041Z-l_r8tqjv/`.
  Full M2 TCG: `artifacts/m2/20261010T024121Z-v9l_et0q/`; raw evidence revalidation:
  `artifacts/m2/20261010T165131Z-_12smsq0/`. Disabled-PCD Linux resource allocation
  succeeded inside the existing bridge window; all three boot paths passed.
- Environment and installed dependency manifests: `artifacts/environment/` and
  `artifacts/dependencies/20261004T171703Z/`; the additional GCC 14 selection is
  recorded under `kernel_toolchain` in the lock.

Any source/toolchain change must update the lock, relevant patches/configuration,
this matrix, and the affected gate results. A failed or pending combination is
not converted to PASS because another combination succeeds. This release supports TCG only.

The complete 2026-10-18 project run is
`artifacts/project/20261017T173924Z-zxmxej2p/`, with all ten top-level steps PASS.
It includes the complete M3 aggregate at `artifacts/m3/20261017T175140Z-q1s8vqzd/`
and the full core run at `artifacts/verify-core/20261017T173926Z-m79vspke/`
(98.1567% core line coverage). Input and evidence hashes were checked after completion.
The project run used incremental builds; see VALIDATION.md for the exact build conditions.
