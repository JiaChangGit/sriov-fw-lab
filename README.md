# SR-IOV Controller Firmware & Platform Lab

**16 GB RAM 筆電／WSL2 Ubuntu 26.04／QEMU TCG** 上可重現的 Linux driver、VFIO 與韌體教學專案。
用 FILL／CRC32 加速器實作模擬 PCIe SR-IOV、DMA 隔離、佇列、reset 與錯誤恢復。

**[從這裡開始：閱讀路線與實作練習](docs/LEARNING_PATH_ZH.md)**

SR-IOV 是單根輸入／輸出虛擬化：一個 PCIe 裝置可以提供管理用的 PF 和多個資料用的 VF。
本專案用模擬裝置研究 VF 的資源配置、DMA 隔離與 reset；常用名詞與縮寫見
[閱讀路線中的速查表](docs/LEARNING_PATH_ZH.md#1-前置知識與補讀位置)。

教材可直接在 GitHub 閱讀；各章提供操作步驟、技術原理、關鍵程式碼、流程與時序圖，
並以預測、追蹤和自問自答核對理解。建置與歷史驗證紀錄分別標明適用環境及證據範圍。

| 閱讀目的 | 章節 |
|---|---|
| 取得來源、安裝套件、逐階段建置與判斷結果 | [從零建置](docs/BUILD_TUTORIAL_ZH.md) |
| 理解 compiler/linker、Kbuild、initramfs、TCG 與 VFIO | [工具與技術深入](docs/TOOLS_DEEP_DIVE_ZH.md) |
| 分析所有權、資料流、控制流與狀態轉移 | [系統分析](docs/SYSTEM_ANALYSIS_ZH.md) · [系統指南](docs/SYSTEM_GUIDE_ZH.md) |
| 閱讀 probe、UAPI、IRQ、鎖及資源回收 | [Kernel 導讀](docs/KERNEL_WALKTHROUGH_ZH.md) |
| 沿一筆請求逐函式追蹤、檢查錯誤分支與 reset | [程式追蹤](docs/TRACE_CODE_ZH.md) |
| 手算 ring、執行 GDB 練習並核對狀態 | [追蹤練習與圖解](docs/TRACE_PRACTICE_ZH.md) |
| 依症狀定位、建立可反證假說、修正與回歸 | [除錯方法](docs/DEBUGGING_ZH.md) · [真實 BUG 紀錄](docs/BUG_LOG.md) |
| 執行展示、查驗要求與實測限制 | [DEMO](docs/DEMO_ZH.md) · [驗證紀錄](docs/VALIDATION.md) |

[離線完整教材](docs/handbook.html)請下載後用瀏覽器開啟；GitHub 檔案頁面會顯示 HTML 原始碼。
離線版本包含預先產生的 SVG，不需連線載入圖表套件。圖表維護見 [DIAGRAMS](docs/DIAGRAMS.md)。

專案範圍是 **M0–M3／T01–T10**：portable C core、QEMU 裝置、Linux PF driver、VFIO tenant、
UEFI handoff，以及隔離與 reset 測試。使用 TCG，不需要實體 SR-IOV 卡。
2026-11-05 曾從空 build 完成一輪驗證；受測輸入與限制見驗證紀錄。驗收要求見 [REQUIREMENTS](docs/REQUIREMENTS.md)，
受測版本、建置方式與本機紀錄索引見 [VALIDATION](docs/VALIDATION.md)。

## 建置

先在 PowerShell 執行 `wsl -d Ubuntu-26.04`；首次套件安裝見 [教學](docs/BUILD_TUTORIAL_ZH.md)。
之後在 WSL：

```sh
cd /path/to/sriov-fw-lab  # 含 Makefile 的專案根目錄
make env-check
make fetch-sources
make verify-project JOBS=2
make build-handbook
```

完整入口會串行建立 core、QEMU、stress/KASAN kernels、initramfs、EDK2，再執行 M0–M3 與 validators。
固定使用 TCG，檢查 `artifacts/project-latest.json` 及其指向結果，不能只 grep PASS。
已有完整 build 後，`make verify-m3` 可重跑整合回歸。

## 執行範例

完成完整驗收後：

```sh
make demo         # M1：四 VF、獨立 VFIO domains、100 次 enable/disable
make demo-reset   # T07：outstanding work reset、健康 VF 進度
```

Demo 不取代完整驗收。從「誰擁有資源、誰可以 DMA、舊工作如何失效」介紹，
再沿 tenant → SQ → VF requester → IOMMU → payload → CQ → eventfd trace。

Portable C core 管理 VF 的 FSM、ledger 與 mailbox 協定；PF driver 另管理 host 交易及 lease 狀態。
Guest 真正使用 Linux PCI、VFIO TYPE1、IOMMU、MSI-X/eventfd；PCIe device 由 QEMU 模擬。
T05 strict WRITE-only 需要明列的 toy-only guest kernel patch，不是 stock Linux 保證。
不包含實體 passthrough、硬體效能、刷寫或 secure update；測試通過不等於證明零 BUG。

`downloads/` 保存 pinned sources；`build/` 保存產物；`artifacts/` 保存原始結果。
PF driver 只載入 L2 guest；JOBS=2，一次一個大型 build／guest。
`make clean-build` 保留 sources 與測試紀錄。

[繁體中文摘要](docs/README_ZH.md) · [English summary](docs/README_EN.md)

## 公開版本與授權

公開原始碼包附有 [驗證摘要](docs/verification-summary.json)，包含受測來源的 SHA256。
原始 logs、主機環境資訊與編譯產物留在本機，不在公開包內；文件中的 artifacts 路徑用於重跑後查找結果。
摘要保留當次結果與增量建置限制，不取代自行執行驗收。
PUBLICATION_MANIFEST.json 是 2026-10-18 公開快照的清單；後續修改後，不能用它驗證目前工作目錄。

本專案自有內容採 **GPL-2.0-only**；[授權範圍](LICENSING.md) 與
[第三方聲明](THIRD_PARTY_NOTICES.md) 說明上游補丁及引用片段的例外。
