# 專案範圍與驗收要求

目標環境為 Lenovo R7000P 2024、16 GB RAM、WSL2 Ubuntu 26.04，主題是 Linux kernel、
PCI driver 與 VFIO。功能範圍以能在這套環境重建、執行及核對結果為原則。
本文件定義 M0–M3／T01–T10 的驗收要求；實測結果見 [VALIDATION](VALIDATION.md)。

## 保留範圍

| 範圍 | 實作與驗收 |
|---|---|
| M0 | C11 FSM／mailbox／quota／scheduler／descriptor；unit、ASan/UBSan、獨立 oracle、coverage、fuzz |
| M1 | QEMU PF/VF、BAR／ARI／ACS／MSI-X／FLR；四個獨立 VFIO domains、實際 MAP/UNMAP、100 次 enable/disable |
| M2 | EDK2／OVMF probe、capability／aperture、同次開機交接 Linux、disabled-PCD 對照 |
| M3 | PF leases／labd／tenant、SQ/CQ、FILL／CRC32、DMA fault、reset/rearm、stale callback、KASAN T01–T10 |
| 建置 | pinned sources／patches、stress/KASAN kernels、TCG、JOBS=2、先 fetch 後離線 build |
| 教材 | 繁中離線 HTML、六張架構／資料／控制／時序／狀態圖、trace code、講稿問答、逐步建置與 BUG log |

## 必要門檻

1. `make verify-project JOBS=2` 成功，`artifacts/project-latest.json` 及其結果 PASS。
2. M0 四個 seeds 各至少 100000 oracle operations；兩個 fuzz targets 各 100000 runs 加 300 秒探索；coverage ≥90%。
   重用結果須綁定受測來源、build stamp、binary 與 evidence hashes。
3. M3 包含當前 M0、KASAN/stress M1、三次 M2 boot、T01–T10 與 validators；部分 checkpoint 不取代全 gate。
4. 同時檢查 VM 退出碼、cases／raw logs、timeout 與 kernel reports。原始 FAIL 保留，改動後重跑受影響範圍。
5. 建置能從空 build 開始，保留 pinned source cache；結果須記錄 `build_started_empty`。
   中斷後重驗要保留原紀錄並核對 inputs，不偽稱為單次不中斷的乾淨驗收，也不宣稱重新安裝過全新 Windows。
6. 教材連結／source anchors／hash 檢查成功，另以瀏覽器檢查圖表。

## 範圍外的功能

| 項目 | 範圍說明 |
|---|---|
| M4／T11–T21 | 缺完整驗收；M0/M3 使用的 fault hooks 與 native scheduler tests 保留 |
| KCSAN／KVM | 缺本版驗收；移除公開選項及 KCSAN config |
| M5／T22–T25 capsule／A/B／journal | 未實作，不提供更新／persistent demo；reserved ABI opcode 保持 UNSUP、feature bit 3 維持 0 |
| 實體 SR-IOV、硬體效能／刷寫／secure update | 本機沒有相關硬體驗證，不依機型推定能力 |
| hostile PF remove／tenant crash 全矩陣 | 不宣稱未執行的 M4 驗證，保留已測正常 cleanup 與 M3 錯誤路徑 |

保留 ABI rejection 是輸入驗證，不表示已提供 firmware update。
`tests/scenarios.json` 只列本版必要案例，其 NOT_RUN 是需求初始值，執行狀態需讀 artifacts。
有限測試不能保證所有輸入與未來環境皆無 BUG。當前問題見 [OPEN_ISSUES](OPEN_ISSUES.md)。
