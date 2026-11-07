# SR-IOV 韌體與平台實驗

16 GB RAM 筆電、WSL2 Ubuntu 26.04、Linux driver／虛擬化教學專案。
功能範圍為 **M0–M3／T01–T10**，涵蓋 portable core、QEMU、PF driver、VFIO tenant 與 UEFI handoff。

入口：[閱讀路線](LEARNING_PATH_ZH.md)、[從零建置](BUILD_TUTORIAL_ZH.md)、[工具與技術](TOOLS_DEEP_DIVE_ZH.md)、
[系統分析](SYSTEM_ANALYSIS_ZH.md)、[Kernel 導讀](KERNEL_WALKTHROUGH_ZH.md)、[程式追蹤](TRACE_CODE_ZH.md)、
[追蹤練習](TRACE_PRACTICE_ZH.md)、[除錯方法](DEBUGGING_ZH.md)、[BUG 紀錄](BUG_LOG.md)、
[DEMO 步驟](DEMO_ZH.md)、[驗收要求](REQUIREMENTS.md)、[實測紀錄](VALIDATION.md)。
各章 Markdown 可直接在 GitHub 閱讀；[離線教材](handbook.html)需下載後用瀏覽器開啟。
套件安裝後，在 WSL 依序執行 `make env-check`、`make fetch-sources`、`make verify-project JOBS=2`、`make build-handbook`。
一次一個大型 build／TCG guest，driver 只載入 guest。

Core 管理 VF 的 FSM／ledger；PF 協調交易；tenant 擁有 VFIO fds／mappings／buffers。
以位址、時間、世代三種隔離介紹 DMA 與 reset。Guest 真正使用 Linux VFIO/IOMMU，device 由 QEMU 模擬。
T05 strict WRITE-only 需 toy-only guest kernel patch，不宣稱 stock Linux 保證、實體硬體能力或零 BUG。
