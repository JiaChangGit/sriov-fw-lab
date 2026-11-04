# M3 控制介面與所有權

本章整理目前 M3 的控制介面。操作教學見 [建置教學](BUILD_TUTORIAL_ZH.md) 與
[DEMO 手冊](DEMO_ZH.md)，程式導讀見 [Kernel 導讀](KERNEL_WALKTHROUGH_ZH.md)。
實際測試結果以 [驗證紀錄](VALIDATION.md) 及 [案例索引](M3_PROGRESS.md) 為準。

## 各層負責什麼

- `fwcore/` 管理裝置 VF 的生命週期、quota ledger 與 mailbox 協定。
- QEMU adapter 管理模擬硬體的 fence、kill、epoch、queue 與 DMA。
- PF driver 管理控制交易、per-open lease、事件與自身生命週期。
- labd 持有 PF control fd，啟停 tenant、檢查 peer credentials、保存 pidfd 並協調 reset。
- 每個 tenant 持有自己的 VFIO container/group/device fd、eventfds、buffer 與 DMA mappings。

Tenant 不取得 PF fd。HAL 的 `owners_closed()` 只能檢查裝置可觀察的條件，
QEMU adapter 在這裡檢查 VF PCI function 已不存在；它不能查核 guest process 的 fd。
Owner 是否完成 UNMAP、關閉及退出，由可信任的 labd／PF driver 流程處理。
Root 繞過這個流程的情境不在測試模型內。

## UAPI 版本與欄位

`include/toy_uapi.h` 定義 host UAPI 1.0。
`TOY_GET_INFO` 為 56 bytes，`TOY_GET_VF` 為 72 bytes；
新增控制操作共用 80-byte `toy_vf_ctl`，64-bit 欄位使用 `__aligned_u64`。
Header 和 native layout tests 檢查大小與 offset；這不是 32-bit guest runtime 測試。

Device MMIO/mailbox ABI 也是 1.0，但與 host UAPI 分開定義。
`toy_probe()` 使用從 `TOY_ABI_VERSION` 推導的 `TOY_PROTOCOL_MAJOR/MINOR` 協商，
不使用 UAPI 版本代替裝置協定版本。

| Offset | 欄位 | 說明 |
|---:|---|---|
| 0 | hdr | 8-byte size／major／minor |
| 8 / 12 | vf / flags | u32；global 使用 UINT32_MAX；release 使用 OWNER_CLOSED |
| 16 / 24 | session / epoch | u64；輸入預期值，輸出觀察值 |
| 32 | lease_token | u64；綁定 open file context |
| 40 | operation | u64；同一 open context 的控制操作編號 |
| 48 | ready_gen | u32；REARM 核對 CONFIG_COMMIT 世代 |
| 52 / 56 / 60 | state / hw_flags / outstanding | u32 輸出欄位，輸入須為零 |
| 64 | fw_status | u32；保留裝置結果，syscall 仍回負 errno |
| 68 | reserved[3] | 三個 u32，必須為零 |

Ioctl 由 `_IOWR(TOY_IOCTL_MAGIC, number, struct toy_vf_ctl)` 建立，
不要在應用程式硬寫某個架構的 ioctl 整數值。

| 編號／操作 | 實際責任 |
|---|---|
| 3 LEASE_ACQUIRE | 核對 live tuple 與 VF 狀態，將新 lease 綁到呼叫者的 open context；在 VFIO open 之前完成 |
| 4 LEASE_RELEASE | 核對 context、token、OWNER_CLOSED、fence 與 outstanding；接受可信 labd 的關閉確認，只清 host lease |
| 5 FENCE_VF | 執行 PF-only FENCE_BEGIN，讀回結果與新 epoch；不等待 firmware mailbox |
| 6 RESET_VF | 使用同一 context 已完成的 fence 結果，執行 QUIESCE／RESET；不在入口再 fence 一次 |
| 7 REARM_VF | 核對 session、epoch、ready_gen、READY_PENDING、BME 等條件；mailbox REARM 成功後再次讀回狀態 |
| 8 GLOBAL_RECOVER | 要求無 lease 且 VFs 已停用；經 PF_CONTROL 做全域復原，再協商 device ABI |

`GET_VF.lease_token` 只回報目前 context 自己的 token。
兩次獨立 `open()` 會建立不同 context；`dup()` 則共用同一個 open file description，
因此也共用 lease、replay cache 與 read stash。

## 重送與錯誤

`toy_control_ioctl()` 先驗證輸入形狀，再比對 per-open replay cache。
相同 operation、cmd 和完整輸入回傳保存的結果，不重做硬體副作用；
同 operation 改內容回 EPROTO，舊 operation 回 ESTALE。
Replay 在 live tuple 檢查之前，因此 RELEASE 或 GLOBAL_RECOVER 已改變狀態後，
仍能重取原操作結果。

已執行操作會保存回覆及 errno；`copy_to_user()` 失敗不撤銷硬體操作。
新操作須重新讀取狀態，不能把 timeout 當作「什麼都沒發生」而任意重送。

Device mailbox 的重送是另一層：session／seq 加逐欄 request 比較，
只保存最近已接受的請求。兩個 cache 分別保護 ioctl 交易與裝置協定，不能互相替代。

## 事件、鎖與 remove

`toy_event` 固定 64 bytes，包含 type、vf、event_seq、session、epoch、lease_token、
ready_gen、state、hw_flags 和 detail。事件類型為 RESET、FLR、FAULT、REMOVE、LOST_EVENTS。

每個 open context 有容量 64 的 ring。溢位時記錄遺失數，讀者先收到 LOST_EVENTS，
再以 GET_INFO／GET_VF 重新同步；舊通知不能直接授權 REARM。
Read 一次回一個完整 record；空的 nonblocking read 回 EAGAIN。
資料先移入 stash，離開 event spinlock 後才複製到 userspace，copy 失敗保留 stash。

鎖順序為 `admin_mutex → mbx_mutex`。IRQ handler 完成 mailbox 等待通知並排入 work，
通知 worker 才取得管理鎖、讀 snapshot、發布事件。
Remove 的資源順序與 kref／dying 分工見 [Kernel 導讀](KERNEL_WALKTHROUGH_ZH.md)。
本版未驗收所有 forced remove、tenant crash 或 KCSAN 時序。

## labd 與 tenant 的 wire 格式

`guest/common/lab_wire.c` 使用 AF_UNIX/SOCK_STREAM。
每個 frame 先放 4-byte little-endian body length，再放 64–4096 bytes 的 body。
Transfer 會處理 partial I/O 與 EINTR，沿用絕對 deadline；不假設一次 recv 就收到整筆訊息。
收到 ancillary data 時拒絕訊息，並關閉已收到的 SCM_RIGHTS fds。

| Body offset | 欄位 |
|---:|---|
| 0 / 4 / 6 | magic TLAB／u16 version=1／u16 type |
| 8 / 16 / 24 | u64 request_id／session／epoch |
| 32 / 40 | u64 lease_token／operation |
| 48 / 52 / 56 / 60 | u32 vf／ready_gen／flags=0／payload_len |

Decoder 檢查非零 request_id、欄位格式與 `64 + payload_len == body_length`。
各握手步驟再核對預期的 request_id、type 及 tuple；
wire helper 本身沒有通用的重送 cache。

訊息編號為 HELLO=1、HELLO_REPLY=2、READY=3、STOP_REQUEST=4、STOPPED=5、
RESET_NOTICE=6、RUN=7、EXIT_ACK=8、ERROR=9、STOP=10、INSPECT=11、INSPECT_REPLY=12。
停止／reset 訊息帶有 16-byte reason、hw_flags、outstanding、reserved；
EXIT_ACK 帶四個剩餘資源計數，正常結束時皆須為零。
ERROR 帶 8-byte errno magnitude 與 stage。INSPECT 用於測試時的同步觀察。

labd 以 SO_PEERCRED 核對 uid 與實際啟動的 child PID，再以 pidfd／waitpid 追蹤退出。
訊息內自行聲稱的 PID 或 token 不足以驗證身分。

## 正常啟動

1. labd 取得 lease、綁定 VF 並啟動 owner。
2. Owner 建立獨立 VFIO container/group/device，檢查 capabilities。
3. VFIO open 可能觸發 FLR，因此取得 device fd 後才重新 latch epoch/session 並送 HELLO。
4. labd 核對 PF snapshot，回覆預期的下一個 ready_gen。
5. Owner MAP SQ/CQ/payload、設 IRQ、啟用 BME、CONFIG_COMMIT，確認 READY_PENDING。
6. labd 核對 READY 與新的 PF snapshot，執行 REARM，再送 RUN。
7. Owner 讀回 QUEUE_STATUS=RUNNING，才開始提交 descriptor。

## Reset 與關閉

Reset handshake 先 STOP_REQUEST 與 PF fence，再執行 QUIESCE／RESET。
STOPPED 只表示停止新增提交，不表示 owner 可以立即 free buffer。
RESET_NOTICE 帶入已確認的 recovery tuple；owner 將舊 request 標為錯誤，
在 fence/cancel 已確認後執行 exact UNMAP，再配置新 queue。
T08 保留已 UNMAP 的舊 CPU allocation 作 guard，檢查舊 callback 沒有寫入新舊 buffer。

正常關閉先確認 fence 與 outstanding=0，再讓 owner 停用 IRQ、UNMAP、關閉 VFIO fds
並回報 EXIT_ACK。labd 等待 child 退出，解除綁定、更新 close FLR 後的 snapshot，
最後釋放 lease 與 disable VFs。PF 不代替 tenant 呼叫 VFIO UNMAP。

Short／failed UNMAP 的原始錯誤會保留；allocation 等 VFIO fds 關閉後再釋放。
若 PF control fd 意外關閉而仍有 lease，driver 會 fence 並保留 orphaned lease，
不推定另一個 process 的 VFIO fd 已關閉。這種管理端遺失需要重啟 guest 才能重新開始。

PF driver 的 mailbox 等待上限由 `TOY_MBX_TIMEOUT_MS=5000` 定義，為 5 秒，
以 guest kernel 的 jiffies 計時；本版沒有可調整此值的 module parameter。
Wire 的 `LAB_TIMEOUT_MS=120000` 為 120 秒，使用 guest 的 CLOCK_MONOTONIC。
這兩者與 WSL controller 等待 QEMU 程序的 timeout 不同。

Core 的 QUIESCING／RESETTING／RECOVERING deadline 分別為 100／100／1000 virtual ticks，
由裝置的 QEMU virtual clock 計算；driver 或 labd 仍在等待，不會延長 core deadline。

## 測試入口

`make verify-m3` 串行執行 prerequisite gates、T01–T10 與 controller/validator tests。
T02 注入 probe、provision、真實 PCI 部分枚舉及 owner 清理失敗；
T03–T05 檢查 ring、資料與 domain；T06–T09 檢查 reset、stale callback 與 descriptor；
T10 檢查 lease 對 disable/resize 的限制。完整列表及原始結果見 [案例索引](M3_PROGRESS.md)。

每個案例都要看 fault hit、raw records、資料比對、退出碼與 cleanup，
不能僅以「程式印出 PASS」判斷完成。Native tests 與 VM 整合測試各有範圍，
未執行的舊設計要求不列為本版能力。
