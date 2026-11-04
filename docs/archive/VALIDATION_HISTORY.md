# 歷史驗證紀錄

以下記錄屬於各自列明的版本。現行驗收見 [VALIDATION](../VALIDATION.md)。

Only executed, structured results count as PASS. This snapshot records the
completed evidence listed below; a build still in progress has no accepted
result. The original HTML defined the historical M0–M5 plan, superseded by this release's REQUIREMENTS.md. M0, the strengthened
M1, M2 and M3 passed at the recorded source checkpoint. M4–M5 and the original
clean extended gate were incomplete at that time and are now outside the deliverable. Later affected-source changes require
rerunning the relevant gates; these historical results do not validate new code.

The accepted T05/T06 checkpoint is
`artifacts/m3-checkpoint/20261013T005556Z-56ebu4kg/`: both M1 profiles, three M2
boots, complete T01/T02/T03/T04/T05/T06/T09/T10 and 32 validator tests PASS.
T05 includes all six real DMA fault variants under the explicit strict-WO
guest-kernel patch; T06 includes real VFIO reset/close/open and healthy VF
progress. The repaired T01 lifecycle passed all 100 cycles again. Every linked
result hash was checked after the aggregate completed. Earlier failures and
the interrupted previous aggregate remain preserved.
These historical results are not full M3 acceptance. New individual T07/T08 VM runs passed:
`artifacts/m3-case/20261013T074221Z-79mqmb0v/` and
`artifacts/m3-case/20261013T074144Z-r54944se/`. Exact native deadline checks and
42 validator/controller tests also passed; see M3_PROGRESS.md. The complete
current-input M3 gate **PASS** is `artifacts/m3/20261013T074512Z-64uwugjd/`:
current M0, both M1 profiles, three M2 boots, T01–T10 and 42 validator tests.
Post-run input/result/log hashes passed at `artifacts/m3-evidence-audit/20261013T131412Z-el30rtk6/`.
That checkpoint did not accept M4/M5 or a clean rebuild. The current offline handbook has six SVG diagrams and linked source listings;
document generation/checks are separate from project gates.

The accepted pre-T05 M3 development checkpoint is
`artifacts/m3-checkpoint/20261012T111926Z-eajy2snh/`: affected builds,
KASAN/stress M1, all three M2 paths, full T01, T02, T03, T04, T09, T10 and 26 semantic/controller
tests passed. M0 source/input revalidation also passed at
`artifacts/m0/20261012T113113Z-jw5aclg8/` (unchanged full core: 98.1567% coverage).
T05–T08 were NOT_RUN at that historical checkpoint. Earlier
failures and the rejected preliminary T03 result remain preserved. See
[M3_PROGRESS.md](../M3_PROGRESS.md) for the exact artifacts and incomplete M3 scope.

| Item | Profile / accelerator | Status | Evidence / outstanding work |
|---|---|---|---|
| Windows and WSL environment | native | PASS | `artifacts/environment/windows.json`, `artifacts/environment/wsl.json` |
| Required dependencies | Ubuntu 26.04 | PASS | `artifacts/dependencies/20261004T171703Z/`; apt exit codes and exact versions |
| Source commit/submodule resolution | four pinned upstreams | PASS | `sources.lock.json`, `artifacts/sources/` |
| Pinned API/PCD/Kconfig evidence | source inspection | PASS | `docs/SOURCE_EVIDENCE.md`, `artifacts/sources/source-evidence.json` |
| M0 complete gate | native GCC / Clang | PASS | `artifacts/m0/20261009T180814Z-59mo6bfz/result.json`; environment, ABI schema, source evidence/pins and matching full core results |
| M0 core after D005 deadline correction | native / ASan+UBSan / coverage / libFuzzer | PASS | `artifacts/verify-core/20261007T005515Z-v5p454gl/`; details below |
| QEMU compile and device-help smoke | GCC 15.2 / x86_64-softmmu | PASS | `artifacts/build-qemu/20261009T180051Z-45a14hyq/`; PF ACS reset-overlap correction included |
| Kernel configuration only | stress | PASS | `artifacts/build-kernel/20261006T163106Z-q61osh75/`; no kernel binary implied |
| Linux first full build | stress / GCC 15.2 | FAIL | `artifacts/build-kernel/20261007T154311Z-w2gl3ipa/compile.log`; EFI stub C23 keyword conflicts, recorded in D007 |
| Linux replacement build acceptance | stress / pinned GCC 14.3 | PASS | `artifacts/build-kernel/20261008T005830Z-j07gwf5d/`; kernel/module compiled with PCI_MMCONFIG; earlier obsolete no_llseek failure retained |
| BusyBox / initramfs build | stress / GCC 15.2 | PASS | `artifacts/build-initramfs/20261010T023451Z-zxaz50p_/`; strengthened capability checks, exact ELF closure and archive manifest |
| Earlier M1 runtime | stress / TCG / seed 101 | PASS | `artifacts/m1/20261008T162925Z-te4a54s0/`; four domains and 100 cycles passed; PF ARI/ACS validation was missing, so this no longer suffices for the complete gate |
| M1 corrected capability gate | stress / TCG | PASS | `artifacts/m1/20261010T023628Z-alfh_i9y/`; PF and four VF ARI/ACS, four groups/TYPE1 containers, exact UNMAPs and 100 measured cycles |
| EDK2 probe and OVMF variants build | GCC 14 / GCC5 tag / NASM 3 backport | PASS | `artifacts/build-edk2/20261010T024041Z-l_r8tqjv/`; generated PCD evidence, stable hash seed and corrected protocol BAR indexing |
| M2 earlier diagnostic boots | OVMF / TCG | FAIL | UART ABI, ACS overlap, protocol BAR index and duplicate log-field failures retained; D010–D012 and UEFI.md explain corrections |
| M2 complete gate | OVMF / TCG | PASS | `artifacts/m2/20261010T024121Z-v9l_et0q/`; probe, same-boot Linux handoff, disabled-PCD observation; raw evidence revalidated at `artifacts/m2/20261010T165131Z-_12smsq0/` |
| Updated core M0 | native / ASan+UBSan / fuzz | PASS | `artifacts/m0/20261011T041249Z-y96enneq/`; full async deadline/rollback rerun, 98.1567% coverage |
| Updated adapter/driver M1 | KASAN / TCG | PASS | `artifacts/m1/20261011T042025Z-jpfokbhp/`; four domains and 100 measured cycles |
| Updated adapter/driver M1 | stress / TCG | PASS | `artifacts/m1/20261011T130752Z-7wnb25hd/` |
| Updated core/adapter M2 | OVMF / TCG | PASS | `artifacts/m2/20261011T131027Z-59i1ihdd/`; all three modes and fresh matching VARS |
| Native guest wire/UAPI/ring cursors | ASan+UBSan / 32- and 64-bit layout compilation | PASS | `artifacts/guest-native/20261011T130800Z-0b265gw4/`; 120000 independent FIFO comparisons across u32 wrap |
| T03 only | KASAN / TCG | PASS | `artifacts/t03/20261011T131621Z-31i4_aj4/`; N16/32/256 × 4096 requests, IRQ, guards and exact owner cleanup; does not accept M3 |
| T02 probe/provision subset | KASAN / TCG | PASS | `artifacts/m3-case/20261011T133323Z-buq1je7o/`; ten probe points, 24 rollback variants, six retained-quota recoveries |
| T02 owner fault subset | KASAN / TCG | PASS | `artifacts/m3-case/20261012T010016Z-jjt0qoku/`; 15 faults and 15 clean retries; three real short TYPE1 UNMAP returns |
| T02 partial PCI enumeration subset | KASAN / TCG | PASS | `artifacts/m3-case/20261012T011050Z-o3pskq3g/`; five actual `pci_enable_sriov` failures, including paired firmware unwind failure |
| Current complete T02 | KASAN / TCG | PASS | `artifacts/t02/20261012T012454Z-h980khaf/`; all three subsets on identical builds after current M1/M2 |
| Current T03 | KASAN / TCG | PASS | `artifacts/t03/20261012T012754Z-3occp8j7/`; all ring sizes and native checks |
| Current T10 | KASAN / TCG | PASS | `artifacts/m3-case/20261012T012840Z-y1qerjw8/` |
| Current affected-build regression | KASAN + stress / TCG + OVMF | PASS | `artifacts/m3-checkpoint/20261012T011508Z-cc46sekz/`; both M1 profiles, three M2 boots, T02/T03/T10, 18 validator tests |
| T04 complete isolation aggregate | KASAN / TCG | PASS | `artifacts/t04/20261012T063152Z-ivewr8wd/`; N16/32/256, two independent domains with same IOVAs, 8196 completions and six bytewise buffer checks per size |
| Regression including T04 | KASAN + stress / TCG + OVMF | PASS | `artifacts/m3-checkpoint/20261012T061844Z-30wnco7b/`; both M1 profiles, three M2 boots, T02/T03/T04/T10, 20 validator tests |
| T01 full lease-owned lifecycle | KASAN / TCG | PASS | `artifacts/t01/20261012T112848Z-nzlrkpjz/`; warmup + 100 cycles, four live owners, exact UNMAPs, process/driver resource baselines, QOM inventory |
| T09 stale/malformed descriptors | KASAN / TCG + ASan/UBSan | PASS | `artifacts/t09/20261012T113922Z-jue6qi12/`; six real guest vectors and bounded native replay of identical bytes |
| Regression including T01/T09 | KASAN + stress / TCG + OVMF | PASS | `artifacts/m3-checkpoint/20261012T111926Z-eajy2snh/`; both M1 profiles, M2, T01/T02/T03/T04/T09/T10, 26 validator tests |
| T10 busy lease | KASAN / TCG | PASS | `artifacts/m3-case/20261012T005543Z-yuc8qxtm/`; rejected disable/resize, unchanged topology/quota, successful owner close and final disable |
| M3 barrier and semantic validators | native Python | PASS | `artifacts/m3-controller-native/20261012T010134Z-0eo5j6gy/`; 13 tests, including mutated real-run evidence |
| M3 | KASAN / TCG | PASS | `artifacts/m3/20261013T074512Z-64uwugjd/`; complete T01–T10 and current M0/M1/M2 prerequisites |
| M4 | KCSAN / TCG | NOT_RUN | T11–T21, operation budgets, repeats and fault/remove checks pending |
| M5 | persistent model / TCG | NOT_RUN | Capsule/journal, T22–T25 and process-crash recovery pending |
| Clean `verify-extended` | TCG | NOT_RUN | All gates must first exist and pass; not a completed project |
| KVM initialization | optional | NOT_RUN | Device exists; no initialization test performed |

The accepted M0 core run executed all 20 legal FSM edges and 92 illegal edges
with unchanged context, four independent-oracle seeds (101, 202, 303, 404) with
100,000 operations each, native and ASan/UBSan unit tests, and both fuzz targets
with 100,000 runs plus 300 seconds of exploration per target. Core line coverage
is 368/376, or **97.87234042553192%**. The aggregate M0 run reused that complete
result after checking the recorded core input hashes. Earlier results remain in
`artifacts/`; they are not substituted for the affected-code rerun.

The successful QEMU binary SHA256 is
`46507ba8e7d7a170bfd777efd3efd7fb76b8dabc1e66c53d1989d762b7069640`.
The first QEMU configuration failure is retained in
`artifacts/build-qemu/20261007T005337Z-xhfparax/`; D006 records the missing offline
Meson overlay fix. Neither the device-help smoke nor static source inspection
establishes runtime compatibility.

The earlier M1 run took 119.66 seconds. VF BDFs `0000:01:00.1` through `.4`
occupied distinct groups 5–8, each with only that VF. Final ledger, VF functions,
VFIO descriptors/maps and device-owned pending work were zero. QEMU had 340
objects at cold start and 360 after first-requester initialization; both complete
inventories before and after the following 100 measured cycles had the same
SHA256. No unexpected kernel report occurred. The earlier cold-versus-warm
inventory failure remains in `artifacts/m1/20261008T162441Z-o7m628zi/`; D008
explains the pinned Intel IOMMU cache behavior and the strengthened repeat.

The corrected M1 repeats these checks and validates ARI/ACS on all five
functions. The successful M2 probe is
`artifacts/m2-probe/20261010T024121Z-b4cxsvep/`; positive handoff is
`artifacts/m2-handoff/20261010T024123Z-9h7cps1x/`. All ten Linux PF/VF BAR
resources remained inside the corresponding firmware apertures and bridge
window. The handoff ran 100 cycles, used four independent groups/containers,
and reported no unexpected kernel diagnostics. Three boots used distinct fresh
VARS copies; no direct-kernel boot or PCI reallocation rescue arguments were used.

The disabled-PCD boot at `artifacts/m2-negative/20261010T024326Z-8cb4c766/`
observed both VF apertures initially at zero. Linux allocated them inside the
existing bridge window and passed enable-four plus the full M1 checks. This is
the recorded negative-configuration outcome, not an assumed allocation failure.
Earlier failed boots remain FAIL even though the corrected runs passed.

The Windows `.wslconfig` differs from the recommendation; see DECISIONS D001.
The existing global Git CRLF conversion was corrected in project source
checkouts only. Toolchain changes are recorded in the lock and build manifests;
the GCC 15 kernel failure remains a failure even if a later compiler succeeds.
No unbuilt adapter, driver, or planned test is represented as validated.
