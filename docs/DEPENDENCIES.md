# 相依套件與來源版本

專案以 `sources.lock.json` 記錄 upstream commit、submodules、patch 和工具鏈。
來源取得成功與建置／開機成功分開判斷；FETCHED 只表示來源已取得。
實際建置結果見 [版本表](VERSIONS.md) 與 [驗證紀錄](VALIDATION.md)。

## 釘選來源

| 元件 | Ref | Commit | 本版用途 |
|---|---|---|---|
| QEMU | v9.2.0 | ae35f033b874c627d81d51070187fbf55f0bf1a7 | x86_64 TCG、toy PF/VF 與 Intel vIOMMU |
| Linux | v6.12 | adc218676eef25575469234709c2d87185ca223a | stress／KASAN guest kernel、PF driver、VFIO TYPE1 |
| EDK2 | edk2-stable202411 | 0f3867fa6ef0553e26c42f7d71ff6bdb98429742 | OVMF 與 SriovProbe EFI application |
| BusyBox | 1_36_1 | 1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4 | 最小 initramfs userspace |

Lock 另保存來源 URL、tag object 與 peeled commit。Annotated tag 的 object 不等於 source commit。
Linux v6.12 是固定的教學基線，不代表目前的安全維護版本。
更新來源時，要重新核對 API、patch、設定與受影響測試，不能只修改版本字串。

## 安裝與建置的分工

1. `tools/install_deps.py` 透過 apt 安裝系統套件，保存套件候選版本、安裝及 dpkg-query 紀錄。
2. `make env-check` 檢查 WSL、檔案系統、記憶體、磁碟與必要工具；缺項先處理。
3. `make fetch-sources` 取得 upstream、recursive submodules 與所需 wheel，記錄 commit／hash。
4. 各 builder 使用本機來源，在 `build/` 建立可套 patch 的工作目錄；建置不自動下載缺少的來源。
5. `make verify-project JOBS=2` 串行建置及驗證 M0–M3，保存每一步命令、退出碼與結果。

`downloads/` 是來源快取，不是直接修改專案的地方。
需要相容性修正時，以有 hash 的 patch 套用到 build 工作樹。
Git checkout 使用 LF；不修改使用者的全域 Git 設定。

## 系統套件用途

下表是用途索引，確切安裝清單以 `tools/install_deps.py` 為準。
套件目前是否存在、版本是否符合，仍由本機 manifest 與 env-check 判斷。

| 套件／工具 | 用途 |
|---|---|
| build-essential、git、python3、pkg-config | C 建置、來源管理與測試 controller |
| gcc-14、gcc-ar-14 | pinned Linux／EDK2 工具鏈；builder 明確選用，不切換全機 alternatives |
| python3-venv、meson、ninja-build | QEMU configure 與建置 |
| libglib2.0-dev、libpixman-1-dev、zlib1g-dev | QEMU host 依賴 |
| libelf-dev、libssl-dev、bison、flex、bc | Linux host tools 與設定 |
| cpio、xz-utils、gzip、rsync | initramfs、來源與產物處理 |
| nasm、acpica-tools、uuid-dev | EDK2 BaseTools／OVMF |
| pciutils | guest lspci 與 PCI 診斷 |
| clang | libFuzzer 與 sanitizer 測試 |
| lcov／gcov | core coverage 資料 |
| python3-markdown-it | 離線教材 renderer |

Linux v6.12 曾在 GCC 15 的 EFI stub 編譯失敗，因此 kernel 明確使用 GCC 14。
EDK2 的 GCC5 是工具鏈設定名稱，本專案實際選用 GCC 14。
QEMU 與 guest userspace 的已測版本使用 GCC 15；實際版本及歷史失敗見 [VERSIONS](VERSIONS.md)。

libslirp 是 QEMU 的選配網路依賴，本專案 guest 測試不需要外部網路。
Sparse、cppcheck、shellcheck 等補充分析工具不等於已執行的驗收項目；
是否執行要看當次紀錄，不能只因已安裝就列為 PASS。

讀碼與除錯練習另使用 `ripgrep`（`rg`）及 `gdb`，不在基本 installer 清單內；
額外安裝命令見[建置手冊](BUILD_TUTORIAL_ZH.md)。
一般離線手冊建置只需已提交的圖檔；修改 Mermaid 來源後重新產 SVG，才需要 Python 3、
本機 Chromium 瀏覽器與固定版本 Mermaid。來源 hash 與維護命令見 [DIAGRAMS](DIAGRAMS.md)。

## 16 GB 筆電的資源安排

預設 JOBS=2，一次只執行一項大型建置或一台 guest。
一般 guest 為 1536 MiB、兩個 vCPU；KASAN guest 為 2048 MiB。
建置前至少預留 60 GiB，也要確認 Windows 上存放 WSL VHD 的實體磁碟仍有空間。
Swap 不算可用實體 RAM；不要直接用 `-j$(nproc)` 放大平行工作量。

QEMU、kernel、initramfs、EDK2 的建置工具與 VM runner 共用 resource lock；clean-build 也使用同一把鎖。
Native core 工具沒有這層鎖，不要同時啟動多輪驗收，也不要在測試期間手動清理 build。
本版只接受 TCG，Linux direct boot 與 OVMF handoff 是不同的測試路徑。

## Guest 封裝

Guest binaries 的 ELF interpreter、shared libraries 與 symlink 目標都必須放入 initramfs。
L1 的 `ldd` 能找到 library，只表示 host 有檔案，不表示 guest 已封裝完成。
Guest init 使用 POSIX sh；L1 Python controller 不放進 guest。
套件安裝使用 apt，不使用 sudo pip，也不在 build 中臨時修改系統 Python。

## API 與來源查證

| 元件 | 主要來源位置 | 檢查重點 |
|---|---|---|
| QEMU | include/hw/pci/pcie_sriov.h、hw/pci/pcie_sriov.c | PF/VF 初始化、BAR 與 requester RID |
| QEMU | PCI DMA／MSI-X／PCIe reset helpers | VF address space、MemTxResult、pending IRQ 與 FLR |
| Linux | drivers/pci/iov.c | enable/disable、部分枚舉 unwind、assigned VF |
| Linux | drivers/vfio/、include/uapi/linux/vfio.h | group、TYPE1 MAP/UNMAP、reset、capability layout |
| Linux | Kconfig 與實際 .config | stress／KASAN、PCI_IOV、VFIO、Intel IOMMU |
| EDK2 | MdeModulePkg.dec、PciBusDxe、OvmfPkgX64.dsc | SR-IOV／ARI／page-size PCD 與實際 build 值 |
| EDK2／Linux | PCI protocols、EFI stub initrd loader | BAR index、extended config、LoadFile2 handoff |

精確 commit、來源片段與 hash 見 [SOURCE_EVIDENCE](SOURCE_EVIDENCE.md)。
來源中有某個 API，不代表專案已正確使用；仍須搭配建置、guest runtime 與錯誤路徑結果。

Build stamp 與 result 保存來源、patch、設定、工具及產物雜湊。
修改來源後要重新建置與驗證，不手改 stamp 讓舊 binary 通過檢查。
PASS／FAIL／BLOCKED／NOT_RUN 描述執行狀態，RESOLVED／FETCHED 則描述來源取得進度。
