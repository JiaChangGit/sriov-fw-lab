# 專案內容與教材目標

本專案用於學習與說明 Linux kernel、PCI driver、VFIO 和裝置生命週期。
執行環境是 Lenovo R7000P 2024、16 GB RAM、WSL2 Ubuntu 26.04，功能範圍為 M0–M3／T01–T10。
完整驗收條件見 [REQUIREMENTS](REQUIREMENTS.md)。

## 實作驗收

提供從空 build 執行 `make verify-project JOBS=2` 的入口，保留來源／build／結果 hashes 與 raw logs。
實際驗收須區分空 build、增量重驗與中斷紀錄，不將多次執行拼稱為單次不中斷的結果。
檔案存在、mock 單元測試或單一 PASS 字串不能取代 VM 驗收。
不提供 capsule／A/B／persistent update、不要求實體 SR-IOV 卡、不宣稱 KVM／KCSAN 已驗證。

## 教材內容

1. 90 秒、5 分鐘、深入追問的面試講稿與英文摘要。
2. 需求、責任邊界、各層狀態的管理者、資源所有權、不變條件及限制。
3. L0 Windows／L1 WSL-QEMU／L2 guest、控制／資料流、正常 I/O／reset 時序、FSM。
4. 重要功能實作原理、guard、錯誤與 cleanup 路徑、trace code。
5. 從取得專案、套件、pinned sources、建置到驗收的步驟、工作層、命令、輸出與成功判斷。
6. 真實 BUG 的症狀／原因／修正／回歸／原始 log，一般診斷建議另外標明。
7. 離線 HTML、Markdown、SVG 圖表與可點擊的源碼。

## 教材驗收

`make build-handbook` 檢查 local links、anchors 與 hash；另外用瀏覽器檢查閱讀與圖表。
文件生成成功不代表專案 gate PASS。最終執行結果見 [VALIDATION](VALIDATION.md)。
