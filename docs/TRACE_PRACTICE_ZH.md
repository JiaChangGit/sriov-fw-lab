# 動手追蹤：一筆請求、兩個游標與一次重置

本章從一筆 FILL 請求開始，練習辨認資料放在哪裡、何時跨越邊界、由誰更新狀態。
先讀範例，再遮住答案追一次 CRC32 或 stale descriptor。圖中的箭頭分成兩種：

- `→`：同一執行環境內，圖中明示的呼叫或資料流；圖只列出與問題相關的步驟。
- `==>`：透過 MMIO、DMA、中斷或程序通訊跨越邊界；不能把它當成一般 C 函式呼叫。

以下依目前來源靜態整理，圖中的數字是教學用例子，並非本次 VM 的實測輸出。
實際執行與測試證據請看 [驗證紀錄](VALIDATION.md)。

## 1. 先畫出執行位置

```text
 L2 guest Linux                         L1 WSL 中的 QEMU process
 ┌───────────────────────────────┐     ┌──────────────────────────────┐
 │ tenant process                │     │ toy VF / PF 裝置模型         │
 │ owner_workload：準備 SQ       │     │ write_vf：接收 register 寫入 │
 │ eventfd：等待通知             │     │ toy_worker：安排待處理工作   │
 │ CPU 讀取 CQ、核對 payload     │     │ service_one：讀取 descriptor │
 │             │                 │     │ toy_complete：資料與完成記錄 │
 │ VFIO / guest IOMMU            │     │ fwcore：狀態與資源帳本       │
 └─────────────┼─────────────────┘     └──────────────┼───────────────┘
               └──── MMIO / DMA / MSI-X 跨越邊界 ────┘
```

QEMU 裝置模型與 guest 程式不在同一個 C call stack。追到 VF register 寫入時，
要換到 QEMU 的 register handler；等 IRQ 時則要回到 guest 的通知與 CQ 讀取路徑。

**練習：** 如果 guest 的 payload 檢查失敗，應先記下哪兩個位址？
答案：記下 CPU 使用的 buffer 位置和 descriptor 使用的 IOVA，並確認兩者所屬的 VF 與 mapping；
數字相同與否不能單獨證明 DMA 有權存取。

## 2. 一筆 FILL 的資料流

假設 entries=16，這批只有一筆請求，開始時 SQ tail=6、CQ head=5。
此例刻意讓兩個 cursor 不同；不要假設它們永遠同步。

```text
tenant                                      QEMU toy VF
① 填 SQ[6]：payload IOVA、長度、id、epoch
    │
    ├─ release fence → 寫 SQ_TAIL=7、DOORBELL ==> write_vf
    │                                               │
    │                                   排程 ==> toy_worker
    │                                               │
    │                                  service_one 讀 SQ[6]
    │                                  保留 session / epoch / token
    │                                               │
    │                                  toy_complete 檢查世代與 gate
    │                                               │
    │                        VF requester DMA 寫入 payload ==> IOMMU
    │                                               │
    │                                  寫 CQ[5] 內容，再寫 phase
    │                                               │
    │                               更新 CQ tail → MSI-X ==>
    │
② eventfd 喚醒 → 檢查 CQ[5] phase
    → 檢查 id、狀態、長度、epoch、資料與 guard
    → 寫 CQ_HEAD=6，讓裝置可重用該 completion slot
```

這是成功路徑的簡圖：`service_one` 有 CQ 已滿、DMA 失敗、舊 epoch 等分支；
`toy_complete` 也可能拒絕舊世代或處理 DMA fault。
畫線時請同時查 `guest/tenant/owner.c:owner_workload`、
`device/toy_vf.c:write_vf`、`device/toy_hw.c:toy_worker`、
`device/toy_hw.c:service_one` 與 `device/toy_hw.c:toy_complete`。

| 要追的資料 | 建立或寫入 | 誰讀取 | 要確認的事 |
|---|---|---|---|
| SQ descriptor | tenant 填入 SQ memory | VF 以 DMA 讀取 | id、長度、IOVA、epoch 是否屬於此請求 |
| payload | FILL 時由 VF 寫入 | tenant 在 completion 後核對 | 每個 byte 與 guard 是否符合 |
| CQ record | VF DMA 寫入 | tenant 讀 phase 後核對 | id、status、length、epoch；不能只看 IRQ |
| eventfd | guest VFIO 傳遞通知 | tenant 等待並讀取 | 通知次數不必等於完成數 |

**沿線自查：** 找 `device/toy_hw.c:dma`，觀察傳給 `pci_dma_read/write` 的 PCI device。
這決定 vIOMMU 看見的 requester；若只看到 payload 結果正確，仍不足以證明 requester 正確。

## 3. 函式呼叫與非同步事件要分開

以下縮排中的 `→` 表示來源中的直接呼叫；`==>` 表示事件接續，
沒有宣稱跨邊界的函式位於同一個 call stack。

```text
guest tenant
 owner_workload
   → owner_write32（SQ_TAIL 與 DOORBELL）
       ==> VF register 寫入
QEMU
 write_vf
   → toy_schedule
       ==> timer 到期
 toy_worker
   → fw_schedule_round
   → service_one
       → dma（讀 SQ）
       → toy_complete（無延遲分支；延遲分支先保存工作）
           → dma（payload）
           → dma（CQ body / phase）
           → toy_irq
               ==> MSI-X / VFIO / eventfd
guest tenant
 owner_workload 繼續執行 → 讀 CQ、核對 payload → 更新 CQ_HEAD
```

找到呼叫後，接著看分支與回傳值。例如 `dma` 先檢查 gate，再以 VF requester
執行 PCI DMA；回傳失敗時，呼叫者的處理方式要回到對應分支確認。
`toy_complete` 的 payload fault 分支仍會嘗試寫入帶錯誤狀態的 CQ；
CQ DMA 本身失敗時則進入 `toy_fail`。不能把所有失敗簡化為同一種結果。

## 4. 為什麼 SQ slot 6 對應 CQ slot 5？

`guest/common/lab_ring.h:lab_ring_index` 對 entries=16 使用 `counter & 15`。
在這個例子裡，送出時 `start=6`、收到第一筆時 `received=0`：

```text
提交資料位置： (start + received) & 15 = 6  → payload slot 6
完成記錄位置： cq_head & 15           = 5  → CQ slot 5
```

兩者用途不同。舊 epoch 的 descriptor 可以只推進 SQ、不產生 CQ；
因此用 CQ index 找 payload 可能拿到另一筆資料。現在的
`guest/tenant/owner.c:owner_workload` 使用 `start + received` 找原始 payload，
使用 `cq_head` 找 completion。B018 的原因與回歸見 [BUG 紀錄](BUG_LOG.md)。

下一步試算：若 entries=16、start=15、received=1，payload slot 是多少？
答案是 0。這是 ring index 的回繞；u32 counter 本身也會回繞，
`lab_ring_used` 以 unsigned 減法計算差值，但容量限制仍由呼叫處檢查。

## 5. 從 ACTIVE 到重置後重新開放

下圖只畫一條正常重置路徑，完整合法邊以 `fwcore/fsm.c` 的 `edges` 為準：

```text
 ACTIVE
   │ FENCE：已 fenced，硬體 epoch 必須較新，取消工作成功
   ▼
 QUIESCING
   │ DRAIN_OK：outstanding 必須為 0
   ▼
 RESETTING
   │ RESET_DONE：outstanding 為 0，清除 queue 成功
   ▼
 RECOVERING
   │ READY_REARM：新 config、epoch、ready_gen、BME、
   │              deadline 等條件符合；set_gate 成功
   ▼
 ACTIVE

 QUIESCING / RESETTING -- timeout 到期 --> FAILED
 RECOVERING -- VERIFY_FAIL -------------> FAILED
```

箭頭不是只寫 state。一般事件由 `fwcore/fsm.c:fw_transition` 先查合法邊、
讀硬體 snapshot 並檢查條件，再更新 core 狀態；不合法的邊在 HAL 呼叫前拒絕。
`READY_REARM` 是特別路徑：`fwcore/fsm.c:fw_vf_event` 轉交
`fwcore/admission.c:fw_rearm`，由後者檢查條件並開 gate。
FLR 也有直接進入 RECOVERING 的合法邊，不能把上圖視為所有 reset 的唯一路徑。

**練習：** 已經 CONFIG 成功但 gate 還關閉時，可以從 ACTIVE 單獨推論 DMA 正在執行嗎？
不可以。還要檢查 fence、killed、BME、config_valid、世代與 queue 等條件；
`device/toy_hw.c:gate` 是實際 DMA/IRQ 前的檢查位置。

## 6. 追一個失敗，避免只看最後的 PASS

```text
症狀：payload 不符
   ↓
確認測試時間、來源與 build hash 是否同一輪
   ↓
讀 tenant 結果：request id、SQ tail、CQ head、epoch
   ↓
讀 qemu.log：對應 VF / RID 的 TOY_DMA_OK 或 TOY_DMA_ERROR
   ↓
分辨 SQ fetch、payload、CQ、IRQ 發生到哪一步
   ↓
查程式分支，提出可被反例推翻的原因
   ↓
保留原始 FAIL → 修改 → 新一輪重跑受影響測試
```

從 `artifacts/*-latest.json` 找本輪結果時，先確認指標時間與 command；
測試執行中可能仍指向前一次。沒有建立該輪結果時，狀態是未執行或未完成，
不能使用另一輪 PASS 補上。guest JSON、kernel log 和 QEMU log 各自回答不同問題。

靜態追蹤顯示程式可能走的路徑，並不證明該分支在本次測試中執行。
Linux 的 [ftrace 文件](https://docs.kernel.org/trace/ftrace.html) 說明
function graph tracer 可以記錄函式進出；是否可用仍取決於受測 kernel 的設定，
不可將一般來源閱讀或本專案的 QEMU log 稱為已執行 ftrace。

## 參考閱讀

- [Organizing Instruction and Study to Improve Student Learning](https://ies.ed.gov/ncee/wwc/PracticeGuide/1)：
  使用完整示例、練習、圖與文字搭配；本章用示例後接小練習的方式編排。
- [MDN technical writing](https://developer.mozilla.org/en-US/blog/technical-writing/)：
  先交代背景、使用具體例子，並讓每個段落回答明確問題。
- [Linux ftrace](https://docs.kernel.org/trace/ftrace.html)：了解執行期函式追蹤的能力與限制。
