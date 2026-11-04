# 系統分析與圖解

本章描述本版 M0–M3 的責任分工與設計契約。每張圖都要搭配 guard 與測試證據閱讀，不能把示意圖當成無條件保證。
離線教材由 `tools/build_handbook.py` 產生，SVG 不需要網路、JavaScript 或外部字型。

## 架構與所有權

新手先用一個情境理解需求：兩個程式各自使用一個 VF，都把 payload IOVA 設成 `0x400000`。
A reset 時，B 必須繼續工作；A 的舊 callback 不能寫進重新配置後的新 buffer。
因此系統同時需要「位址歸誰」「何時能存取」「工作是否過期」三種判斷。

| 需求 | 可檢查的不變條件 | 主要證據 |
|---|---|---|
| A 不能寫到 B | 每次 DMA 用 A 的 VF requester／domain；B 的整個 buffer 不變或只發生自己的預期變化 | T04 同 IOVA、T05 fault 與 healthy VF |
| reset 不留下舊工作 | fence 生效後 epoch/token 改變，callback 執行前的世代檢查拒絕舊 tuple | T07/T08、retired/new buffers、eventfd |
| owner 失敗不能漏資源 | mapping、fd、child、lease、PCI function、ledger 各自回收；失敗時保留 quarantine | T01 100 輪、T02 unwind |
| queue 可重用 | u32 差值有界、phase 區分世代、CQ_HEAD 在讀完後更新 | T03、native ring oracle |
| 管理重送不重複扣 quota | session/sequence 加完整 request 比較、cached reply | M0 mailbox oracle/unit |

## 六張圖的閱讀順序

架構圖回答「在哪裡執行」，資料流回答「資料去哪裡」，控制流回答「誰決定下一步」，
正常 I/O 與 reset 時序回答「何時發生」，FSM 回答「這個事件現在是否合法」。
面試時先選一條正常路徑，再指出一個失敗分支，避免一次展示所有細節。

![系統分層與所有權](diagrams/architecture.svg)

QEMU process 在 WSL 執行；guest 的 PCI config/MMIO 操作會進入 QEMU 的裝置 callback。
圖中的「firmware」是連進 QEMU process 的 portable core。L2 driver 在 guest CPU 上執行，
不能直接呼叫 host QEMU process 裡的 fwcore 函式，必須透過 register/mailbox 介面控制裝置。

| 元件 | 負責內容 | 不應擅自代做的事 |
|---|---|---|
| `fwcore` | authoritative FSM、quota/queue/vector ledger、mailbox protocol、logical scheduler | 不配置 Linux fd，不直接呼叫 VFIO |
| QEMU toy device | PCI presentation、registers、模擬硬體 gate、VF requester DMA、virtual clock | 不因 Linux 枚舉了 VF 就私自修改 ledger |
| PF driver | host 管理交易、mailbox adapter、lease、event、driver lifetime | 不由 PF 替 tenant UNMAP，不另建矛盾 FSM |
| labd | owner 啟停、SO_PEERCRED/pidfd、tuple 驗證、reset handshake | 不把文字 READY 當作硬體已 RUNNING |
| tenant | 自身 VFIO container/group/device、eventfd、SQ/CQ/payload allocation/mapping | 不操作 PF BAR 或另一 tenant 的 fd |
| L1 test controller | 啟動一台 VM、ACK fault、保存原始輸出、獨立檢查 | 不直接改 guest buffer 偽造裝置結果 |

## 資料流：隔離在每次 DMA 都必須成立

![DMA 與 completion 資料流](diagrams/data-flow.svg)

相同 IOVA 並不表示同一塊記憶體。IOMMU translation 還取決於 requester 及其 domain。
SQ 是裝置讀，CQ 是裝置寫，payload 依 opcode 決定方向。每次 helper 的錯誤回傳都要處理；
成功 fetch descriptor 不保證稍後的 payload 或 CQ 也能存取。

CQ phase 是最後發布的 byte。Tenant 先檢查 phase/acquire，再讀 CQ 其他欄位及 payload，
最後更新 CQ_HEAD。IRQ 只通知「可能有新進度」，不代替 CQ 的有效性檢查。

## 控制時序：正常配置

![Provision 與失敗回收控制流程](diagrams/control-flow.svg)

1. labd 透過 PF driver PROVISION，再由 Linux enable SR-IOV。
2. PF lease 成立後啟動 owner；VFIO open 可能引發 FLR，owner 重新讀 session/epoch。
3. owner MAP 自己的 buffers、建立 MSI-X eventfd、寫 staging registers 與 CONFIG_COMMIT。
4. owner 發 READY，labd 核對 peer、lease、operation、session/epoch/ready_gen。
5. PF REARM 核對 live hardware state，確認 RUNNING，再讓 owner 開始提交。

若步驟 3 失敗，不能直接把 lease 當成沒發生；仍要確認 fence、mapping、fd 和 child 的清理。
若步驟 5 拒絕，owner 保持停止，不能重送 descriptor 試運氣。

## 正常 I/O 的時間與可見性

![正常 I/O 發布時序](diagrams/io-sequence.svg)

圖中的 fetch 是 SQ 與 CRC payload 的讀取路徑；FILL 的 payload 操作是以相同 VF requester 寫入，
兩種 opcode 都經 IOMMU 並檢查每次 DMA 回傳值。
Doorbell 表示「SQ 有新工作」，不表示工作已完成。DMA payload 成功，也不表示 CQ phase 已發布。
IRQ 發生後，tenant 仍以 phase/acquire 讀取 completion；eventfd counter 不能拿來當 CQ entry 數。
本版限定 x86_64 QEMU/VFIO MMIO 路徑；換到 non-coherent 實體平台時，cache maintenance 與 DMA barriers
需要平台專屬實作和重新驗證，不能只複製 C11 fence。
即使 coherent DMA memory 也仍需要正確 ordering；參見
[Linux 6.12 DMA mapping guide](https://docs.kernel.org/6.12/core-api/dma-api-howto.html)。
該文件說明 kernel DMA API；本專案 userspace VFIO 的具體存取順序仍應追 `owner_workload()`。

## 控制時序：有未完成請求的 reset

![有未完成請求的 reset 時序](diagrams/reset-sequence.svg)

T07/T08 已驗證圖中的 retained work 與 stale callback，並通過完整 M3 回歸。
T06 另驗證 VFIO_DEVICE_RESET、queue readback、重新配置以及 healthy VF。
兩組測試觸發 reset 的路徑不同，不能互相替代。

Reset 時的兩個重點是「確認安全的順序」和「誰仍擁有記憶體」。PF 先關 gate/readback，
core 完成 quiesce/reset 到 RECOVERING；owner 將舊 request 標失敗，完成 exact UNMAP 後，
才建立新 queue。T08 刻意把已 UNMAP 的舊 CPU allocation 保留下來做 guard，
新 allocation 使用相同 IOVA，用來觀察 callback 是否錯碰新舊 memory。

不能只看 callback counter。必須實際執行帶有舊 session/epoch/token 的 callback，
檢查 STALE_CPL 而非 STALE_DESC，並確認 payload/CQ 未改及 eventfd 沒有新通知。

## FSM 與 queue gate 是兩張不同的圖

![Firmware 生命週期主要轉移](diagrams/lifecycle.svg)

完整轉移表以 `fwcore/fsm.c:edges` 的 20 條為準；離線 HTML 會直接從當前來源抽取表格。
圖只展示主要路徑與失敗匯流，不能拿示意箭頭當作所有合法 state/event 的枚舉。

從 FREE 到 ACTIVE 的 PROVISION commit，表示 firmware 已分配資源。它仍要求 hardware fenced，
且沒有 live config。這時的 ACTIVE 不代表 tenant 可 DMA。真正的執行條件位於
`device/toy_hw.c:gate()`：要求 ACTIVE、valid config、BME、沒有 fence/kill，並有正確 generation。

QUIESCING 與 RESETTING 的 logical deadline 各 100 ticks，RECOVERING 等 READY 為 1000 ticks。
裝置一 tick 是一毫秒 QEMU virtual time。PF driver 的 mailbox 上限為 5 秒，以 guest jiffies 計時；
labd／tenant 的 wire 上限為 120 秒，以 guest CLOCK_MONOTONIC 計時。它們不延長 core deadline，
也不是 WSL controller 的程序 timeout。因此可能出現「guest 管理程式仍在等待，裝置已 KILL」的狀態。

期限的精確邊界另以 native fake HAL 控制 logical clock，從正常 CREATE/COMMIT/REARM/FENCE
路徑進入 QUIESCING、RESETTING、RECOVERING，沒有直接改寫待測 deadline。
`artifacts/recovery-deadlines/20261013T074437Z-7f_3jtp_/` 保存九個實際讀值及 ASan/UBSan 執行：
quiesce/reset 在 99 不失敗，100/101 進 FAILED 且 KILL；READY 相應為 999/1000/1001。
這三種 timeout 情境都保留 8 credits。VM 的慢 READY 測試負責真實跨層握手；native 測試負責精確邊界，
不宣稱 QMP polling 可以準確停在每一個 VM tick。

FAILED 會保留 reserve；已驗證 owners closed、PCI/config/outstanding 清除後，DESTROY 可以釋放 ledger，
但 KILL 仍需 global recovery 才能清除。故 FREE、FENCED 和 KILLED 可同時出現，
並不是只看 FREE 就能接受為乾淨測試基線。

## 測試結果能支持哪些結論

| 證據層 | 檢查內容 | 不能替代 |
|---|---|---|
| Native core unit/oracle/fuzz | 轉移、ledger、bounded parser、scheduler 算法 | 真實 PCI/VFIO、driver lifetime |
| Guest KASAN + VFIO | kernel memory safety 診斷、實際 map/reset/owner 路徑 | 未觸發路徑、KCSAN race、硬體效能 |
| QEMU requester/gate trace | 該次 DMA/RID、fence、generation、callback 路徑 | 整個 buffer 是否正確、host process leak |
| Whole-buffer + exact UNMAP + pidfd | 資料變化及 ownership cleanup | 所有無限可能時序都沒有 BUG |
| 保存 raw logs 並重新解析 | PASS 可追溯到真正執行與預期語意 | 沒執行的 case、原始碼存在本身 |

面試時要說明結論來自哪一層測試，以及該測試尚未涵蓋的情境。

## 錯誤分類決定恢復動作

| 錯誤類型 | 即時動作 | 為何不能一律 retry |
|---|---|---|
| 格式錯誤 descriptor | BAD_DESC completion，核對無額外 payload 改動 | 同一筆錯誤輸入重送不會變正確 |
| old epoch descriptor | consume SQ、增加 STALE_DESC、不產生 CQ | 若回寫舊 CQ，可能碰到新 owner 的資料 |
| DMA fault | 能安全寫 CQ 時回 DMA_ERROR，再 fence/fail | mapping 可能已失效，重試可能破壞隔離 |
| old callback | 增加 STALE_CPL、禁止 payload/CQ/IRQ | 即使新 queue 已 RUNNING，舊工作仍無權存取 |
| cleanup 不完整 | 保留 quota 或 buffer 到已確認安全的階段 | 歸零計數會掩蓋實際 ownership |
| logical timeout | KILL／FAILED；保留資源待明確 recovery | guest 管理程式仍在等待，不代表可以延長裝置 deadline |

測試範圍為 M0–M3／T01–T10，不包含 hostile remove／crash 全矩陣或量產可靠度驗證。
