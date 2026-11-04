# 記憶體順序與 DMA 所有權

本章說明目前的 x86_64 guest、classic VFIO TYPE1 與 QEMU 裝置模型。
主要程式在 `guest/tenant/owner.c`、`device/toy_hw.c`、`device/toy_vf.c` 與 `guest/pf/`。
實測範圍是 M0–M3／T01–T10，結果見 [驗證紀錄](VALIDATION.md)。

## SQ 如何交給裝置

1. Owner 配置 page-aligned buffer，檢查 VFIO 回報的 IOVA ranges 與 page-size bitmap。
   SQ 映射為 device READ，CQ 為 device WRITE，payload 為 READ|WRITE。
   READ/WRITE 指裝置方向；CPU 能否讀寫 buffer 是另一件事。
2. SQ/CQ base 必須 64-byte 對齊，entries 相同，且為 16–256 之間的 2 次方。
   Owner 負責建立完整映射；CONFIG_COMMIT 檢查欄位、範圍與世代，
   不會預先走訪 IOMMU 驗證每個 page 都可存取。映射失效仍可能在 DMA 時才被發現。
3. `owner_workload()` 以 little-endian helper 填入完整 32-byte descriptor，
   執行 C11 release fence，再發布 SQ_TAIL 與 DOORBELL。
   `owner_write32()` 另執行 sequentially consistent fence，經 VFIO region 的 `pwrite()` 寫 MMIO。
4. `service_one()` 先檢查 gate、pending work 與 CQ 空間，再以該 VF 的 PCI requester
   讀取 32 bytes 到本地 buffer。DMA helper 失敗時立即處理錯誤，不解碼未完成的讀取。
   後續工作使用這份 descriptor 副本，不重新讀取同一個 SQ 欄位。

以上順序已在本專案的 x86_64 QEMU 路徑測試。C11 fence 本身不是通用 DMA API，
程式也沒有替 non-coherent 實體平台執行 cache maintenance。
`volatile` 不能取代發布／取得順序；移植時須依目標平台重新設計與驗證。

SQ/CQ 使用 u32 累計 counter，按模 2^32 回繞。`tail - head` 以 unsigned subtraction
計算，結果必須不超過 N；array index 才取 `counter & (N-1)`。
裝置每 VF 最多保留一筆 delayed operation；pending 存在時不取下一筆 SQ，
因此不會把它預留的 CQ slot 再分配給別筆工作。

## Payload、completion 與 IRQ 的順序

`toy_complete()` 先檢查保存的 session、epoch、cancel token，接著檢查 gate。
Gate 還要求 VF 存在、ACTIVE、config valid、BME 開啟且沒有 fence/kill。

| 階段 | 實作順序 |
|---|---|
| Payload | FILL 寫入或 CRC32 讀取；每個 bounded chunk 都經 VF requester DMA，重新檢查 gate 與世代，並檢查 helper 回傳值 |
| CQ body | 寫 bytes 0–23，再寫清零的 reserved bytes 25–31 |
| CQ phase | 最後單獨寫 byte 24；phase 為 `1 ^ ((counter / N) & 1)` |
| CQ_TAIL | phase 寫入成功後才遞增 |
| VF MSI-X | `toy_irq()` 再檢查 gate／世代，設定 cause 並通知 vector 0 |
| Tenant 讀取 | `owner_workload()` 等待 eventfd，再以 acquire load 檢查 phase，讀取 CQ body 與 payload |
| CQ_HEAD | 核對 id、epoch、status、length、result、reserved bytes 與資料後，才發布新的 head |

Owner 在 CONFIG_COMMIT 前將 CQ 清零，裝置第一圈發布 phase 1。
Eventfd 是通知，讀到的數值不能當作 completion 數；IRQ 被合併或遮罩時尤其要分清楚。
CQ_TAIL 是裝置進度資訊，這裡的 owner 仍以 phase 判斷 entry 是否可讀。

SQ fetch 或 CQ write 失敗會使該 VF fence／FAILED。若 CQ body 寫入成功、phase 寫入失敗，
CQ_TAIL 不前進，也不發完成 IRQ。Payload fault 則會嘗試寫 DMA_ERROR completion，
之後 fence；若 CQ 也無法寫入，仍走失敗處理。跨頁 payload 可能已完成部分寫入，
本實作不回滾已寫的 bytes。

## Fence、世代與 reset

`toy_fence()` 在同一個裝置 callback 中依序執行：

1. 設定 fenced，阻止後續 gate 通過。
2. 取消 pending operation，保留副本供 stale-callback 故障測試使用。
3. 清除 VF MSI-X pending bits 與 IRQ cause。
4. 檢查 epoch／cancel token 是否會溢位；溢位則保持 fenced 並 KILL。
5. 增加 epoch 與 cancel token。

PF 的非零 FENCE_TOKEN 提供重送去重：同 token 維持 fence、取消 pending，
但不再次增加世代；較舊 token 被拒絕。`toy_vf_flr()` 不使用此去重分支，
每次實際 reset callback 都嘗試換代並清除 queue，不能假設 VFIO open/close 只觸發一次 reset。

不同 token 的型別與用途如下，不能混用：

| 欄位 | 型別 | 用途 |
|---|---|---|
| session | u64 | 區分同一 QEMU instance 內的全域 recovery；不提供跨 QEMU 重啟的持久唯一值 |
| epoch | u64 | 區分單一 VF 的世代 |
| cancel token | u64 | 裝置內部取消工作時更新，保存在 operation 副本中 |
| FENCE_TOKEN | u32 | PF register 命令重送去重 |
| mailbox seq | u32 | 同 session 的 mailbox 請求順序與重送判斷 |
| lease token／operation | u64 | PF per-open 管理所有權／控制交易 |

T08 透過 `stale_set()` 把保存的 operation 副本交回真正的 `toy_complete()`。
世代不符時在 gate 之前增加 STALE_CPL 並返回，不存取 payload/CQ，也不發 tenant IRQ。
測試分別在 fresh CONFIG 尚未 REARM、以及 REARM 之後執行，並比較新舊 buffer 與 eventfd。

正常 recovery 路徑是：
fence/readback → QUIESCE／RESET → RECOVERING → 將舊 request 標為錯誤 →
確認 cancel／drain 後 exact UNMAP → 配置新 buffer／CONFIG → READY／REARM → RUNNING。
UNMAP 成功且回傳 size 符合要求後才可釋放舊 allocation。
T08 刻意保留已 UNMAP 的 CPU allocation 作 guard，再用不同 allocation 重用相同 IOVA。
UNMAP 失敗時則保留 allocation，等 VFIO fd 全部關閉後再清理，並保留原始錯誤結果。

## PF driver 的鎖與生命週期

PF 管理鎖順序為 `admin_mutex → mbx_mutex`。
IRQ handler 只讀取／清除 PF event、喚醒 mailbox completion，並把 health/FLR bits
累積到 atomic pending flags 後排入 workqueue；它不取得 admin mutex。

`toy_notify_work()` 在 process context 取得管理鎖並讀一致快照，再發布 per-open event。
Event ring 的 spinlock 只保護短小 queue 操作。讀取時先移入 stash，放開 spinlock，
才呼叫可能觸發 page fault 的 `copy_to_user()`；複製失敗保留 stash。
Read mutex 序列化同一 open context 的讀者。

`toy_remove()` 先關管理入口、標記 dying 並 fence，再移除入口與取消 work；
PCI disable 在管理鎖外執行，之後回收 slots、IRQ 與 BAR。
Per-open kref 延長 PF 物件記憶體的生命週期，dying 則阻止 fd 再碰已移除的 BAR。
這是程式中的防護；本版沒有把所有 forced remove 或 race 時序列為已通過測試。

## Snapshot、mailbox 與所有權邊界

PF SNAPSHOT 一次保存 PF session 及 VF view；driver 序列化讀者，避免兩個交易互相覆蓋快照。
VF EPOCH_LATCH 保存自己的 epoch、session 與 status。64-bit 值從同一份快照拆成 low/high 讀取。
Queue 設定則先寫 staging，通過 CONFIG_COMMIT 才更新有效配置。

Mailbox 同時只有一筆 outstanding。Core 以 session／seq 辨識請求，逐欄比較 opcode、
target 與 args，沒有另算雜湊。Cache 保存最近已接受的 request 與 reply，
包括尚未完成的 QUIESCE／RESET；poll 完成時更新 cached status。
清除 MMIO DONE 不會清掉 core 的重送 cache。

裝置 HAL 的 `owners_closed()` 只能觀察 VF PCI function 是否已移除，不能查看 tenant fd。
Lease、owner process 與 VFIO 關閉順序由可信任的 PF driver／labd 協調；
管理端已被入侵或 root 繞過流程的情境不在本教學模型內。

## 如何對照測試

T03 檢查 ring、CRC 與 CQ 消費；T04/T05 檢查 domain、requester 與 DMA fault；
T06 檢查實際 VFIO reset；T07/T08 檢查 recovery 與舊 callback；
T09 檢查 stale／格式錯誤 descriptor。U32 wrap 另由 native ring oracle 驗證。
這些結果不能替代未執行的 IRQ／CQ-full 壓力矩陣或實體平台 coherency 測試。
