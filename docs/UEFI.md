# UEFI / OVMF 建置與驗證契約

M2 原始碼在 `uefi/SriovFwPkg`。`tools/build_edk2.py` 從 `sources.lock.json` 指定的
`edk2-stable202411` / `0f3867fa6ef0553e26c42f7d71ff6bdb98429742` 建置；建置成功與
M2 開機驗證分開記錄。目前完整驗收見 [VALIDATION](VALIDATION.md)。以下是較早版本的三條開機結果，紀錄在
`artifacts/m2/20261010T024121Z-v9l_et0q/`，重讀原始證據驗證在
`artifacts/m2/20261010T165131Z-_12smsq0/`；後續來源變更仍須重建與重測。

```sh
python3 tools/build_edk2.py --jobs 2
```

建置與 VM 共用 `artifacts/.resource.lock`，預設二個工作，不與其他大型 build 或 VM 並行。
`--prepare-only` 僅建立離線工作來源與工具設定，回報 `status=NOT_RUN`，不可當作建置通過。

工具建立 `build/edk2-src` detached worktree，驗證所有遞迴 submodule commit，從本機 Git
objects 匯出 submodule 到該工作目錄。BaseTools 會在 Brotli source 旁建立 `.o`，因此不可
把其目錄直接 symlink 到 `downloads/edk2`。整個流程不下載網路來源、不修改來源 cache
的檔案。`fwcore` 與 `include/toy_regs.h` 直接複製進工作 package，保留所有實際 source hash。

`BaseTools/Conf/tools_def.template` 的 X64 `GCC5` tag 使用 `GCC5_BIN` 前綴，並非
`GCC5_PREFIX`。工具將 `GCC5_BIN` 指向隔離 symlink 目錄，明確選 `gcc-14`、`gcc-ar-14`，
並記錄 host C++、binutils、NASM、IASL、Python 的實際版本及 hash。`GCC5` 是 EDK2
設定名稱，不宣稱使用 GCC 5。

Ubuntu 的 NASM 3.01 無法組譯 pinned EDK2 X64 exception stub 的兩處
`push strict dword`。本專案明確 backport 上游
[`9ccf8751a74f26142e584c7b7c7572a182b67997`](https://github.com/tianocore/edk2/commit/9ccf8751a74f26142e584c7b7c7572a182b67997)
的 `strict qword` 修正，patch、原檔與改後檔 SHA-256 均在來源鎖定檔驗證。
patch 僅套用至 build worktree，保留原檔的 CRLF，失敗紀錄與決策見 D009。

每次實際建置產生 raw logs、command argv、return codes、EDK2 build reports，並讀取
所建 `PciBusDxe/DEBUG/AutoGen.h` 確認實際 `PcdSrIovSupport`、`PcdAriSupport` 與
`PcdSrIovSystemPageSize`。probe 與 portable core 明確使用 `-std=c11`。
positive 與 negative 各有
獨立 output directory，使用同一 pinned `OvmfPkgX64.dsc`；只改 output directory，以下
PCD 透過上游支援的 `--pcd` 傳入：

| 設定 | positive | negative | pinned 定義 |
| --- | --- | --- | --- |
| PcdSrIovSupport | TRUE | FALSE | MdeModulePkg.dec，BOOLEAN，default TRUE |
| PcdAriSupport | TRUE | TRUE | MdeModulePkg.dec，BOOLEAN，default TRUE |
| PcdSrIovSystemPageSize | 0x1 | 0x1 | MdeModulePkg.dec，UINT32，4 KiB support bit |

PCD 在 pinned DEC 可用於 FixedAtBuild，PciBusDxe 以 PcdGet 使用；pinned OvmfPkgX64.dsc
未覆寫上述三項。CLI 宣告見 `BaseTools/Source/Python/build/buildoptions.py`。建置期
`AutoGen.h` 證據補足「source 中存在」與「實際編入該設定」的差別。

固定輸出為：

- `build/edk2/SriovProbe.efi`
- `build/edk2/positive/{OVMF_CODE.fd,OVMF_VARS.fd,stamp.json}`
- `build/edk2/negative/{OVMF_CODE.fd,OVMF_VARS.fd,stamp.json}`
- `build/edk2/stamp.json` 與 `artifacts/build-edk2/<run>/result.json`

CODE 與 VARS 來自同一次 variant build，合計要求 4 MiB。每次 VM 必須將 CODE 設 readonly，
並從對應 VARS template 複製新檔，禁止沿用上一 boot 的 variable store。

## ESP 與序列輸出

ESP fallback application 為 `EFI/BOOT/BOOTX64.EFI`（複製 SriovProbe.efi）。ESP 根目錄
`lab-mode.txt` 指定 `probe`、`negative`、`handoff` 或 `negative-handoff`；僅接受完整
名稱，可帶結尾 CR/LF。`negative-handoff` 使用停用 SR-IOV allocator 的觀測條件，並在
同一次開機交給 Linux，記錄 OS 是否補配置資源。兩種 handoff 另需 `linux.efi`、
`initramfs.cpio.gz`、ASCII `cmdline.txt`（工具不得加入 `pci=realloc` 或 `assign-busses`）。

probe 使用 SerialIoLib UART 輸出，且每個必要案例只在真的成功之後印出 JSON：
`M2_CORE`、`M2_CAPS`、`M2_WINDOWS`、`M2_MAILBOX`。probe/negative 結束有
`PROBE_PASS`、`LAB_RESULT=PASS`，再呼叫 ResetSystem shutdown。失敗印實際 EFI status
與 `LAB_RESULT=FAIL`；正常 shutdown 本身不構成 PASS。

2026-10-16 起，兩種 Linux handoff 的 UART0 保存為 `kernel.log`，內容包含 firmware probe、
EFI stub handoff 與 Linux console；UART1 的 `serial.log` 專門保存 guest JSON 和 checkpoint ACK。
Probe-only 沒有 Linux，仍由 UART0 socket 保存到 `serial.log`。`read_uefi_output()` 在驗證時讀取
兩份原始紀錄，不覆寫或猜測修補 JSON；firmware→kernel 的先後仍在同一 channel 驗證。
跨 channel 的串接只方便語意檢查，不宣稱它是兩條 UART 的全域時間排序。背景見 BUG_LOG B020。

X64 的可變參數 logger 必須在宣告與定義都標註 `EFIAPI`。pinned `MdePkg/Include/Base.h`
使用 `__builtin_ms_va_start`，GCC5 tag 則將 `EFIAPI` 定義為 `ms_abi`；沒有該標註會使
logger 以 SysV 呼叫慣例接收參數，卻以 Microsoft ABI 讀取，產生錯誤的零值與空字串。
首次 runtime probe 的失敗原始紀錄保留於
`artifacts/m2-probe/20261009T125442Z-o1u956uy/serial.log`；不能把其中的零值當作真實
PCI 觀測。修正後仍須重建並重新執行 probe，才可判定 PCI 與 handoff 結果。

修正後的 probe、positive handoff 及 disabled-PCD handoff 均已實際通過。
釘選的 PciBus 以一個 protocol BAR index 表示 64-bit BAR；實體 BAR2 因此對應
index 1。probe 依 config BAR 型態換算，並驗證回傳位址與 config 一致，詳見 D012。
disabled-PCD 的 VF BAR 起始位址為零；Linux 後續在原 bridge window 內配置成功，
並通過四 VF 與 100 次循環。這是實測觀察，不宣稱關閉該 PCD 必然造成 Linux 失敗。

portable core selftest 用 dummy HAL 真正跑 VERSION_NEG、CRC、provision/FSM、fence、
reset/destroy 與 ledger 歸零，沒有另一份狀態機。PCI capability walkers 有範圍、對齊、
visited set、hop limit；要求 PCIe/MSI-X/ARI/ACS/SR-IOV。extension config 若
EFI_PCI_IO 回 EFI_UNSUPPORTED，依 `GetLocation` 的 segment 找到 matching
EFI_PCI_ROOT_BRIDGE_IO，使用該 protocol 的 ExtendedRegister address encoding。

probe 不開 VF、不寫 BAR size probe；使用 GetBarAttributes 和 config snapshot。
positive 要求 PF BAR 與兩個各 128 KiB 的完整 VF aperture 均落在實際 upstream
non-prefetchable MMIO window。negative 保留原始 VF base/page size/window 資料，允許
未選的 System Page Size 與未保留的 VF BAR，只檢查 PF window；不能預設 OS 一定失敗。

handoff 在同一 firmware boot 印出 `PROBE_PASS` 後 LoadImage/StartImage Linux EFI stub。
initrd 由 `LINUX_EFI_INITRD_MEDIA_GUID` vendor device path 加 EFI_LOAD_FILE2_PROTOCOL
提供；Linux stub 的 buffer-size negotiation 與實際 copy 均有界限，印
`INITRD_LOADFILE2_BYTES`。LoadOptions 明確轉為 UTF-16。正常 Linux 接手不返回；即便
StartImage 回 EFI_SUCCESS 也回報失敗。controller 必須確認 `PROBE_PASS` 先於 Linux
案例，並保存 Linux resource 與 UEFI window 關係及實際 enable4 結果。
