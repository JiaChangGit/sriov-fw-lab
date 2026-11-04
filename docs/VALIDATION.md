# 驗證紀錄

本頁記錄 2026-10-18 的本機驗收。公開原始碼包附有 [驗證摘要](verification-summary.json)，
保留 146 個受測輸入雜湊、各步驟結果與原始 result 的 SHA256；原始 logs、binary 及主機環境紀錄不隨包提供。
下文 artifacts 路徑是本機紀錄索引，新的執行會產生不同時間戳。摘要不能獨立驗證完整 runtime 證據。

公開整理後，146 個受測輸入仍逐一符合原雜湊。規格 HTML 已改為現行範圍索引，
並更新授權、路徑與文件呈現；規格的新舊雜湊分別列在摘要中。這些整理不另算一次完整 runtime 驗收。

## 完整驗收：PASS

2026-10-18（臺灣時間），在 Lenovo R7000P 2024、16 GB RAM、WSL2 Ubuntu 26.04 上執行
`make verify-project JOBS=2`，以 QEMU TCG 完成 **M0–M3／T01–T10**。
十個頂層步驟全部通過，總耗時約 35 分 28 秒。這是本機當次測量，不是首次建置的時間預估。

- 完整結果（本機紀錄：`artifacts/project/20261017T173924Z-zxmxej2p/result.json`）：各步驟命令、退出碼、耗時及子結果。
- M3 結果（本機紀錄：`artifacts/m3/20261017T175140Z-q1s8vqzd/result.json`）：兩種 M1 profile、M2、T01–T10 與 validators。
- 驗收後核對（本機紀錄：`artifacts/project-evidence-audit/20261018T045239Z-8lc0viw2/result.json`）：44 份結果、570 個檔案的來源、產物及紀錄雜湊符合保存值。

受測工作目錄的基底 commit 是 `7b0cec68a191b31db99c6848421e94ca01d5ae23`，
並包含尚未提交的修改；確切版本以結果中的 146 個 `input_hashes` 為準。
只 checkout 這個 commit，不等於取得本次受測內容。

## 驗收摘要

| 項目 | 結果與證據 |
|---|---|
| 環境與建置 | env-check、QEMU、stress/KASAN kernel、兩份 initramfs 全部成功；EDK2 在 M2 中成功建立 |
| M0 core | unit、ASan/UBSan、獨立 oracle、coverage、兩個 fuzz targets 全部通過 |
| Core coverage | **98.16% line coverage**；只計 core，不是整個專案或 Linux kernel 的覆蓋率 |
| Oracle | 四個 seeds 各 100,000 operations；另有 QoS 比對 |
| Fuzz | 每個 target 先跑 100,000 次，再各探索 300 秒；當次探索分別執行 5,570,699 與 21,869,143 次 |
| M1 | stress 與 KASAN 各驗四 VF、獨立 VFIO domains、MAP/UNMAP、100 次 enable/disable 與資源回收 |
| M2 | UEFI probe-only、同次開機 handoff、disabled-PCD 對照三條路徑通過 |
| M3 | T01–T10 全部通過；逐項內容及結果見 [M3 測試索引](M3_PROGRESS.md) |
| Python tests | 全部 **73 項**通過；其中 M3 controller/validator 的 **42 項**另在 M3 gate 執行，兩者不相加 |
| Native guest | wire/UAPI、120,000 次 ring oracle 比對、15 組真實 owner_workload cursor 回歸通過 |

Core 詳細結果在 verify-core（本機紀錄：`artifacts/verify-core/20261017T173926Z-m79vspke/result.json`）；
native guest 結果在 guest-native（本機紀錄：`artifacts/guest-native/20261017T181450Z-01n6vtkz/result.json`）。
Native 的 32-bit UAPI 檢查是編譯期 layout 檢查，並未執行 32-bit guest。
15 組 owner workload 以 native harness 呼叫實際函式，和 VM 整合測試分別記錄。

## DEMO 與教材檢查

2026-10-18 依 [DEMO 手冊](DEMO_ZH.md) 另行執行三個展示入口，結果如下。
耗時是本機已有 build 的單次觀察，並非固定執行時間。

| 指令 | 結果 | 耗時 | 實際報告 |
|---|---|---:|---|
| `make demo` | PASS | 120.59 秒 | 四 VF／100 次啟停（本機紀錄：`artifacts/m1/20261018T045250Z-qt3f6qwo/result.json`） |
| `make demo-reset` | PASS | 24.15 秒 | T07 reset（本機紀錄：`artifacts/t07/20261018T045451Z-1vmphj0k/result.json`） |
| `make verify-t04` | PASS | 60.48 秒 | T04 三種 ring size（本機紀錄：`artifacts/t04/20261018T045515Z-attm8kit/result.json`） |

指令、退出碼與 log hashes 保存在 DEMO 命令紀錄（本機紀錄：`artifacts/demo-commands/20261018T045250Z-6zkmqh9f/result.json`）。
手冊內四段結果讀取程式也已 逐段實跑通過（本機紀錄：`artifacts/demo-examples/20261018T045621Z-ctmb2bay/result.json`）。

原驗收版本教材的 30 段命令範例通過 語法檢查（本機紀錄：`artifacts/doc-examples/20261018T045323Z-1883cr3n/result.json`）：
27 段 shell 範例包含 7 段內嵌 Python，另有 3 段 PowerShell。
語法檢查不執行其中的安裝、清理或建置命令。

離線教材包含主頁及 22 份程式碼頁；`make build-handbook` 檢查本地連結、
source anchors、輸入／輸出 hashes 與六張內嵌 SVG。
當次畫面檢查紀錄（本機紀錄：`artifacts/handbook-visual-review/20261017T175223Z-rhny9fbk/result.json`）
保存六張圖、章節版面與來源跳轉的檢查結果，並綁定最後產生的檔案雜湊。

## 已修正問題

- **B017**：舊 core PASS 沒有綁定受測 binary；補齊來源、build stamp、binary 及 evidence hash 核對，七項反例回歸通過。
- **B018**：SQ/CQ cursor 不同步時，payload 核對取錯 slot；改用原始 SQ cursor，五個 ring sizes 各驗正常、差一與 u32 wrap。
- **B019**：補齊教材 renderer 安裝與 GCC 14 環境檢查。
- **B020**：kernel printk 與 JSON 共用 UART，偶發破壞紀錄；改為雙 UART，並修正 checkpoint ACK 的輸入端。

症狀、根因、修正前失敗與程式位置見 [BUG_LOG](BUG_LOG.md)。
本輪完整驗收涵蓋上述修正後版本，未出現尚待修復的測試失敗。

## 建置方式與中斷紀錄

本次完整 PASS 的 `build_started_empty=false`，使用已存在的 source cache 與增量建置。
建置程式核對來源及產物後，實際執行全部驗收；不將它描述為單次從空 build 跑到底。

先前從空 build 開始的 `artifacts/project/20261014T005523Z-i612et2_/` 已完成 M0、QEMU、
stress/KASAN kernel 與兩份 initramfs，之後在 KASAN M1 中斷。
`artifacts/project/20261014T143257Z-nrtvyinf/` 則在 M2 handoff 實際失敗，根因為 B020；
原始 UART 交錯紀錄留在 `artifacts/m2-handoff/20261014T145421Z-4j_x1q0l/`，仍維持 FAIL。

其他中斷及較早版本的結果見 [歷史驗證紀錄](archive/VALIDATION_HISTORY.md)。
從頭建立的命令及清理方式見 [建置教學](BUILD_TUTORIAL_ZH.md)。

## 適用範圍

裝置與 IOMMU 由 QEMU 模擬，Linux PCI/VFIO/IRQ 路徑在 x86_64 guest 實際執行。
T05 strict WRITE-only 使用明列的 toy-only guest kernel patch。
本版不包含 M4/M5、KCSAN、KVM、實體 passthrough、firmware update 或硬體效能驗證；
也未涵蓋所有 race、hot-remove 或 non-coherent ARM 情境。
