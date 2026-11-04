# SR-IOV Controller Firmware & Platform Lab

**16 GB RAM 筆電／WSL2 Ubuntu 26.04／QEMU TCG** 上可重現的 Linux driver、VFIO 與韌體教學專案。
用 FILL／CRC32 加速器實作模擬 PCIe SR-IOV、DMA 隔離、佇列、reset 與錯誤恢復。

**[離線完整教材](docs/handbook.html)** · [從零建置](docs/BUILD_TUTORIAL_ZH.md) ·
[系統指南](docs/SYSTEM_GUIDE_ZH.md) · [系統分析](docs/SYSTEM_ANALYSIS_ZH.md) ·
[Kernel 導讀](docs/KERNEL_WALKTHROUGH_ZH.md) · [程式追蹤](docs/TRACE_CODE_ZH.md) ·
[追蹤練習與圖解](docs/TRACE_PRACTICE_ZH.md) ·
[DEMO 步驟](docs/DEMO_ZH.md) · [BUG 紀錄](docs/BUG_LOG.md)

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
