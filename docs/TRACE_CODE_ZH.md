# 從一筆請求理解 VFIO、IOMMU 與裝置生命週期

本章對應 M0–M3 實作，沿控制交易、資料請求與 reset 追蹤各層程式。
執行狀態見 [VALIDATION.md](VALIDATION.md)，真實故障見 [BUG_LOG.md](BUG_LOG.md)。
以下檔案路徑均從 repository 根目錄起算，函式名稱比開發期間會變動的行號更適合追蹤。

## 先分清楚程式究竟在哪裡執行

Windows 是 L0，負責執行 WSL；WSL Ubuntu 是 L1，負責編譯與執行 QEMU。
QEMU 裡的自建 Linux 是 L2，PF driver、labd 與 tenant 都在 L2。
`device/` 的 C 程式在 L1 的 QEMU process 裡執行，模擬 L2 看見的 PCI 裝置。
`fwcore/` 被連進 QEMU，管理這個模擬裝置的 firmware 狀態；並非在筆電上的真實網卡執行。

這個區分決定除錯位置。tenant 的 ioctl 回錯，要看 guest 的 kernel.log；
模擬 DMA 使用錯誤 requester，要看 QEMU 的 qemu.log；QEMU 本身編不過，要看 L1 build log。
在 WSL 載入 toy_pf.ko 不會連上 guest 裡的 PCI function，也不屬於本專案的操作流程。

## 四種位址與兩種權限

tenant 的 C pointer 是 guest process 的虛擬位址，CPU 經 guest page table 存取它。
descriptor 裡填的是 IOVA：裝置 DMA 要使用的位址。這兩個數字不必相同。
VFIO 將 tenant buffer pin 住，讓 guest IOMMU 將某個 domain 的 IOVA 對應到 guest physical pages。
QEMU 最後再把 guest physical memory 解析到自己的 backing memory；toy 裝置不能直接跳過此路徑。

| 名稱 | 本專案中的例子 | 誰使用 |
|---|---|---|
| Userspace VA | `owner.maps[2].memory` | guest CPU 執行 tenant 的 load/store |
| IOVA | payload 起點 `0x400000` | VF descriptor 與 PCI DMA helper |
| Guest physical address | VFIO/IOMMU 建立的轉譯結果 | guest 的 DMA remapping |
| Host virtual address | QEMU RAM backing 的內部映射 | QEMU memory subsystem |

PF-only fence 決定「現在能否發 DMA」；IOMMU 決定「這個 requester 能碰哪些位址」。
BME 是 PCI Command register 的 bus-master enable，tenant 可操作自身 PCI config；
因此 BME 不能代替 PF-only fence。epoch 則用來辨認舊請求，不是位址隔離機制。

IOMMU group 是不能任意拆分的隔離邊界。本拓樸要求每個 VF 各有一個只有自身的 group；
兩個 container 若仍受同一 group 限制，不能宣稱獲得兩份隔離 domain。
T04/T05 因此同時檢查 group membership、獨立 owner、相同數字 IOVA，以及實際 requester DMA。
Group/container 的背景可對照 [Linux VFIO 文件](https://docs.kernel.org/driver-api/vfio.html)。
該網頁會隨 upstream 更新；本版實際採用的 ABI 與實作以 pinned Linux 6.12、
`guest/tenant/vfio.c` 及 `downloads/linux/Documentation/driver-api/vfio.rst` 為準。

## 控制流程入口：啟用 VF 之前先預留資源

從 `guest/pf/toy_pf.c:toy_sriov_configure()` 開始看。寫入 PF 的 `sriov_numvfs` 後，
Linux 會進入這個 driver callback。driver 在管理序列化區段中先驗證要求值、既有 VF 數與 leases，
對每個 slot 執行 PROVISION，最後才呼叫 `pci_enable_sriov()`。

這個順序避免「Linux 已經公布可使用 VF，但 firmware 沒有資源」的半完成狀態。
PROVISION 不是單純設一個 enabled bit，還要管理 queue/vector/reserve 的 ledger。
權威 ledger 與 VF FSM 放在 `fwcore`；PF driver 保存 host transaction、lease 與觀察快照，
不另寫一份自行決定 ACTIVE/FAILED 的 firmware FSM。

繼續追 `toy_mailbox_status()` → QEMU `device/toy_pf.c:write_pf()` →
`toy_mbx_work()` → `fwcore/mailbox.c:fw_handle_mailbox()`。
mailbox 的 session/sequence 與回覆 cache 處理重送；重送相同 request 不能再次預留資源。
讀到 callback 不代表已完成，還要看它回傳的 status、transaction commit 與 unwind 分支。

若第 N 個 VF 的 PCI 建立失敗，Linux 自己的 enumeration unwind 與 firmware 的 rollback
是兩件事，都必須驗證。T02 用 pinned guest kernel hook 在真實枚舉迴圈中注入失敗；
若 firmware rollback 也失敗，就留下 FAILED 與 retained quota，不能為了清表而直接寫 FREE。

## 一個 tenant 如何真正取得 VFIO 所有權

先讀 `guest/admin/labd.c:isolated_owner_start()`，再讀 `start_owner()`。
labd 取得 per-open PF lease、查詢 VF 的 group、啟動 child、保存 pidfd，並檢查 Unix socket 的
SO_PEERCRED。child 在 exec 前關閉管理 fd，不繼承 PF fd 或另一個 tenant 的 VFIO fd。

再讀 `guest/tenant/vfio.c:lab_vfio_open()`。重要順序如下：

1. 將該 VF 綁定 vfio-pci，核對 sysfs driver link。
2. 開啟 container，核對 VFIO API 與 TYPE1 IOMMU extension。
3. 開啟 group，要求 VIABLE，將 group 加入 container，再設定 TYPE1 IOMMU。
4. 取得指定 BDF 的 device fd，核對 PCI、RESET、region、IRQ capabilities。
5. 核對 BAR 尺寸、兩個 MSI-X vectors、vendor/device identity、magic 與 IOVA 範圍。

此處的 `argsz` 很重要：capability 回覆可能比第一個固定 struct 大。
`owner.c:region_caps()` 會重新查詢實際大小，檢查 offset、範圍、重複與循環；
不能把 kernel 傳回的 offset 當成可信任的 C pointer 直接加上去讀。

VFIO open 本身可能觸發 PCI FLR。`owner_open()` 因此在取得 fd 後重新 latch session/epoch。
不能沿用 labd 在 open 前讀到的 epoch；這是 T06 要以真實 ioctl/open/close 驗證的重點。

## 建立 ring：有 CONFIG 還不能立即跑

`guest/tenant/owner.c:map_buffers()` 建立三種映射：

| Buffer | IOVA | 裝置方向 | VFIO flags |
|---|---|---|---|
| SQ | `0x100000` | 裝置讀 descriptor | READ |
| CQ | `0x200000` | 裝置寫 completion | WRITE |
| Payload | `0x400000` | FILL 寫／CRC 讀 | READ＋WRITE |

這裡的 READ/WRITE 是從裝置 DMA 的角度，不是 C 程式能否讀寫 memory。
T05 還揭露 pinned Intel IOMMU 對 WRITE-only 的平台差異，詳見 BUG_LOG B002，
不能用 requested flags 的列印結果代替實際權限測試。

`owner_configure()` 建立 eventfds、登記 MSI-X IRQ、設定 BME，再寫 staging queue registers
與 session/epoch，最後寫 CONFIG_COMMIT。QEMU `device/toy_vf.c:config_commit()`
驗證範圍、entries、當前 generation 及 fence，成功後成為 READY_PENDING。

tenant 送 READY 給 labd。labd 核對自己的 lease、peer、PF live snapshot、ready_gen，
透過 `TOY_REARM_VF` 要求 driver 重新核對，才讓裝置開 gate。
收到 RUN 訊息之後，`owner_workload()` 還會讀回 QUEUE_STATUS==RUNNING；Unix 訊息本身不等於硬體就緒。

## 追一筆 FILL：從 C array 到 PCI DMA

入口是 `owner_workload()`。它為每個 outstanding entry 分配獨立 payload 區，
以 `lab_put16/32/64()` 明確編碼 32-byte little-endian descriptor。
避免直接把 C struct memcpy 到 ring，可以排除 compiler padding、alignment 與位元組序的歧義。

CPU 填好 SQ 後執行 C11 release fence；`owner_write32()` 再執行 sequentially consistent fence，
經 VFIO region pwrite 寫入 register。本版只驗證 x86_64 QEMU 路徑；這些 fence 不等於跨平台 DMA coherency 保證。
程式透過 `owner_write32()` 存取 register；程式不 mmap MSI-X table 保留頁。
SQ_TAIL 與 DOORBELL 進入 `device/toy_vf.c:write_vf()`，喚醒有界 worker。

`device/toy_hw.c:toy_worker()` 計算 backlog、交給 core 的 logical scheduler，
再由 `service_one()` 取 descriptor。最重要的轉折點是 `dma()`：
它把 **該 VF 的 PCI device** 傳給 `pci_dma_read/write()`，所以 QEMU vIOMMU 看到的是該 VF RID。
若改成 PF 或直接寫 guest RAM，即使 FILL 的 byte pattern 看似正確，隔離契約也已經破壞。

`service_one()` 保存 descriptor copy、session、epoch、cancel token 與 CQ reservation。
格式不合法和 epoch 過期不同：舊 epoch 增加 STALE_DESC 並丟棄；非法 opcode/length 產生 BAD_DESC。
合法 FILL 進入 `toy_complete()`，先核對 generation 和 gate，才寫 payload。
跨 4 KiB 邊界時以 page boundary 切成 chunk；若後半段 fault，前半段可能已經寫入。
軟體不能把 CPU buffer 回填原樣後聲稱 DMA 沒發生。T05 guard-page 變體專門驗證這個差別。

## Completion 的發布順序與 eventfd

`toy_complete()` 先編碼 CQ，再分別寫內容和保留 bytes，最後單獨 DMA 寫 phase byte。
phase 成功後才更新 CQ_TAIL 並呼叫 `toy_irq()`。`toy_irq()` 再檢查同一個 gate，
透過該 VF 的 MSI-X 發通知；guest 的 VFIO 將它接到 owner 註冊的 eventfd。

tenant 被 eventfd 喚醒後仍要用 acquire load 檢查 CQ phase；IRQ 是提醒，不是 completion 資料。
程式逐項核對 id、status、length、epoch、CRC、reserved bytes 與 payload guard，
最後才把 CQ_HEAD 交回裝置，允許它重用 ring slot。

索引使用 u32 累計 counter，按模 2^32 回繞，實際 array index 才取 `counter & (N-1)`。
phase 每圈交替，讓同一個位置的新資料和上一圈不同。
「只對 index 取模然後比較相等」無法可靠區分 empty、full 與 wrap。
native ring oracle 另用獨立 Python FIFO 比較跨 UINT32_MAX 的行為，不必真的提交四十多億筆請求。

## FENCE、FLR 與 delayed completion 的先後關係

`device/toy_hw.c:toy_fence()` 先關 gate，取消 pending work、清除 pending IRQ，
檢查溢位後才增加 epoch 與 cancel token。同一個非零 fence token 重送時，維持 fence 並取消 pending，
但不再次增加世代；FLR 則不使用這個重送去重分支。
不能先 unmap，再希望裝置剛好沒發 DMA。`toy_vf_flr()` 共用 clear/cancel 邏輯，
清掉 queue registers，再送 firmware FLR event，使已存在的 VF 進 RECOVERING。

即使 callback 已排入 timer，執行時也要在真正 payload/CQ DMA 前核對原先的 generation。
`toy_complete()` 在第一個資料存取前檢查 `same_generation()`；不符只增加 STALE_CPL，
不寫 payload/CQ，也不通知 tenant IRQ。STALE_DESC 與 STALE_CPL 是不同證據，不能互相替代。
T08 的個別 VM 與完整 M3 回歸已通過；下節以保存的真實輸出追蹤它。

owner 的 mapping 最後由 `owner_unmap()` 解除。VFIO_UNMAP 回傳 size 必須等於要求值；
短 unmap 或失敗會把 memory 留到所有 VFIO fds 都關閉後再清理。
PF 的 ledger 歸零只能證明 firmware 資源釋放，不能證明 userspace fd、pinned pages 或 child 已退出。

## 跟著 T08 的實跑資料逐步 trace

本節數值來自 `artifacts/m3-case/20261013T074144Z-r54944se/`，是開發時的一次獨立 PASS。
每次 run 的 pid、virtual tick 和 healthy VF 完成數可不同，驗證器檢查的是關係與不變條件，
不要求其他 run 重現同一 pid 或 wall-clock 耗時。

### 1. 先讓裝置真的持有一筆工作

由 `guest/admin/recovery.inc:recovery_handshake()` 設定 delay-cpl-ticks=60000，
再讓 `guest/tenant/recovery.c:owner_submit_pending()` 提交 FILL。
先完成 4096 筆正常 I/O 才注入延遲，避免把初始化與故障觀察混在一起。

這筆 descriptor 的 id 是 0x708、length 是 8、payload IOVA 是 0x400000、FILL byte 是 0x79。
SQ_HEAD 變成 4097，CQ_TAIL 仍是 4096，表示裝置已取走工作而尚未發布 completion。
Host 另從 QMP 讀到 pending-completions=1，不能只靠 tenant 自己的 outstanding 變數。

```text
TOY_PENDING vf=0 epoch=4 token=5 cq_index=4096 due=74704
```

`due` 是實際 device virtual tick。這筆工作並沒有等到 74704；reset 會取消它，
測試稍後透過 fault hook 呼叫保存的真實副本，以重現已取消 callback 仍被執行的情境。

### 2. 管理者停止提交，硬體先建立安全邊界

labd 發 STOP_REQUEST，執行 PF FENCE，核對 STOPPED 的完整 tuple，再要求 RESET。
不能只核對訊息類型，因為前一代 owner 的 STOPPED/READY 可能晚到。
`guest/tenant/reset.c` 的 reset-notice 路徑要求 FENCED readback 與 outstanding=0，
並重新讀出 16 個已清空的 queue registers。

此 run 的變化如下：

| 階段 | epoch | firmware state | flags | ready_gen | reserve | outstanding |
|---|---:|---|---:|---:|---:|---:|
| retained work | 4 | ACTIVE (2) | 28 | 1 | 8 | 1 |
| recovering | 5 | RECOVERING (5) | 13 | 1 | 8 | 0 |
| slow_ready | 5 | RECOVERING (5) | 13 | 1 | 8 | 0 |
| fresh_config | 5 | RECOVERING (5) | 61 | 2 | 8 | 0 |
| rearmed | 5 | ACTIVE (2) | 28 | 2 | 8 | 0 |

Flags 應按 `include/toy_regs.h` 定義解碼，不背十進位數字：FENCED=1、KILLED=2、
PCI_PRESENT=4、BME=8、CONFIG_VALID=16、READY_PENDING=32。
因此 61 表示「新 config 已 READY，但仍 FENCED」，28 才是已解除 fence 的有效執行配置。
這個表同時說明：ready_gen 增加與解除 fence 是兩個不同動作。

### 3. 舊請求明確取消，舊記憶體保留作證

`RECOVERY_CANCELLED` 記錄 old_epoch=4、new_epoch=5、requests_marked_error=1。
它不是一筆成功 completion，也不把 completed 從 4096 偷加到 4097。

owner 先回 HELLO，等待實際 device virtual time 至少走過 200 ticks。
此時先不送 HELLO_REPLY 授權新配置。這是在 1000-tick READY deadline 內的慢 owner，
不是把 host sleep 的 200 ms 當作裝置時間。

`guest/tenant/recovery.c:owner_retire()` 要求三個 VFIO UNMAP 都完整返回，
把舊 CPU allocation 移入 retired 陣列，保留每個 byte 的 guard copy。
隨後 `owner_configure()` 配置另一組 CPU memory，在同一 container 重用相同 IOVA。
驗證器要求兩代 vaddr 不相同。若直接 free 舊 buffer，allocator 可能重用同一位址，
會失去同時檢查舊、新記憶體的能力。

### 4. 兩次真正呼叫舊 callback，分別跨過 fence 的前後

第一次在 fresh_config、尚未 REARM 時呼叫，第二次在 REARM 後、新 I/O 尚未開始前呼叫：

```text
TOY_STALE_CPL vf=0 old_session=2 live_session=2 old_epoch=4 live_epoch=5 old_token=5 live_token=6 count=1
TOY_FAULT_ACK property=try-stale-cpl valid=1 ack=5 consumed=1
TOY_STALE_CPL vf=0 old_session=2 live_session=2 old_epoch=4 live_epoch=5 old_token=5 live_token=6 count=2
TOY_FAULT_ACK property=try-stale-cpl valid=1 ack=6 consumed=1
```

`toy_complete()` 先檢查 generation，再檢查 gate，因此這兩次都在世代不符時返回。
第二次在 gate 已解除後執行，用來確認新 queue 恢復運作時，舊 callback 仍不能寫入。`device/toy_pf.c:stale_set()` 將 retained operation 副本交給
`device/toy_hw.c:toy_complete()`，不是直接把 STALE_CPL 加一。

ACK 的 valid=1 指「呼叫前副本有效」，consumed=1 指「callback 已消耗該副本」。
這兩個時點曾被記成同一個值，導致 B010 的真實 validator FAIL。
最後 STALE_CPL=2、STALE_DESC=0、DMA_FAULT=0；每次檢查中 eventfd 非阻塞 read
回 EAGAIN (11)，舊、新 SQ/CQ/payload 的 bytewise guards 均不變。

### 5. 再證明裝置不是被整台停掉

A 新 ring 完成另一批 4096 筆，累計 8192，先前取消的一筆仍單獨計為 error。
B 在 pending、slow_ready、configured_fenced、fresh_io 四個 checkpoint 的 completed
分別為 4608、6656、7424、11776，session=2、epoch=4 沒改。
每批都驗 payload/CRC，並非只看到計數器增加。

最後先 fence 兩個 owner，再 UNMAP／關閉 VFIO／pidfd reap／釋放 lease／disable VFs。
全部八個 slot 回 FREE、reserve=0、outstanding=0、lease=0、flags=1。
這是該次情境的完整成功條件，不包含已移出本版的 hostile remove/crash 全矩陣。

## 不啟動 VM 也能先做的三個 trace 練習

這些是閱讀練習，不算新增的 runtime PASS。先寫下答案，再打開原始碼核對：

1. 在 `device/toy_hw.c:toy_complete()` 找出第一個可能寫 payload 的點。
   沿此前每個 return 分支，說明 old token、fence、BME、bad descriptor 分別在哪裡被拒絕。
   如果只在 SQ fetch 檢查 epoch，延遲 completion 會留下哪個空窗？
2. 在 `guest/tenant/owner.c:owner_workload()` 找出 release、phase acquire、CQ_HEAD 三個位置。
   解釋為何 eventfd 成功讀取後還要 drain CQ，以及 phase 檢查不能搬到讀取 CQ body 之後。
3. 在 `guest/admin/recovery.inc:recovery_handshake()` 列出所有可能回錯的 handshake 步驟。
   對每一步標出當時誰持有 lease、舊 mapping、新 mapping、pending work。
   說明為何出錯不能一律執行 memset 清 ledger。

閱讀 completion 時推薦把資料寫成一行：
`owner pid → VF BDF/RID → container/domain → IOVA → session/epoch/token → CQ id/phase`。
任一欄位對不上，就先定位身分、位址或世代，不急著把所有失敗稱作 DMA bug。

## 重要實作一：把 bytes 安全轉成 descriptor

打開 `fwcore/codec.c:fw_decode_sq()`。讀法分三步：先將 local struct 清零，再以逐 byte LE helper
讀固定 offset，最後驗證後才賦值給呼叫者的 `out`。這讓失敗輸入不會留下半解碼結果。

| SQ offset | 長度 | 欄位 | 必要條件 |
|---:|---:|---|---|
| 0 | 8 | addr | `addr <= UINT64_MAX - len`，避免範圍相加溢位 |
| 8 | 4 | len | 1–4096 bytes |
| 12 / 13 | 1 / 1 | opcode / flags | FILL=1 或 CRC32=2；flags=0 |
| 14 | 2 | id | 識別請求，completion 必須核對 |
| 16 | 8 | epoch | parser 只解碼；裝置另比對 live epoch |
| 24 | 8 | arg | FILL pattern 等操作參數 |

不要把 bytes cast 成 packed struct：即使 x86 允許部分 unaligned access，也會把 layout 與 endianness
假設散落到各層。逐欄解碼較容易在 native fuzz、QEMU、UEFI 重用。

`fwcore/codec.c:fw_queue_validate()` 再檢查 N=16..256 且為 2 的次方、SQ/CQ 相同大小、64-byte 對齊、
session/epoch 相符、加法不溢位、兩段範圍不重疊。Descriptor 正確不等於 ring configuration 正確，兩層都要驗證。

## 重要實作二：有表格也要有 guard 的 FSM

沿 `fwcore/fsm.c:fw_transition()` 讀：

1. 在 `edges` 找 `(current state, event)`。找不到時，在任何 HAL 呼叫之前回 FW_STATE。
2. 取得硬體 snapshot，要求 fenced；需要新 deadline 時以 checked arithmetic 計算。
3. 依 event 檢查條件。例如 RELEASE_OK 必須 PCI 不存在、config/outstanding 清除、owners_closed 為真。
4. 只有能釋放時才扣 ledger；失敗的 release 進 FAILED，保留 reserve。
5. 最後寫入 epoch、deadline、state，輸出 transition log。

`READY_REARM` 另由 `fw_vf_event()` 導向 rearm helper，所以不能只看 edges 就認為所有轉移都走同一 switch。
Native tests 不只數合法邊，還驗非法事件後整份 context 不變；HAL side effects 則另受 adapter 契約與測試約束。

### 手算一次 admission 與 quota

搭配 `fwcore/admission.c:fw_create()` 閱讀：先驗 VF 範圍與 reserve=1..64，
再要求 FREE、fenced、未 killed，且沒有 PCI function、live config 或 outstanding work。
所有檢查成功後，才一起更新 state、reserve、epoch 與三個 resource counters。

| 資源 | 每個 VF 取得 | 上限與 admission 檢查 |
|---|---|---|
| Queue | 1 | 現有 used_queues 必須小於 8 |
| Vector | 2 | 現有 used_vectors 必須不大於 14，新增後最多 16 |
| Credit | 要求的 reserve | 先確認 reserve 合法，再要求 used_credit ≤ 64 − reserve |

例如四個 VF 各 reserve=8，ledger 為 4 queues、8 vectors、32 credits。
把其中一個 reserve 從 8 改為 16，`fw_set_reserve()` 先檢查可安全相減，再算
remaining=32−8=24，最後確認 24≤64−16，才寫回 used_credit=40。
這個操作還要求 ACTIVE/RECOVERING、fenced、無 outstanding 且未 killed；有 quota 不代表可隨時修改。

用「上限減需求」檢查，比先做可能溢位的加法再判斷更清楚；但減法本身也必須先驗需求不超過上限。
回收則沿 `fw_transition()` 的 RELEASE_OK／ABORT_OK 檢查 ownership 後扣帳。
若 release 失敗而保留 reserve，下次 admission 仍會計入它，避免同一份資源被重複分配。
這些數值是由當前程式推導的手算例子；執行證據仍讀 M0 oracle 和 T02 的實跑結果。

## 重要實作三：重送與 exactly-once 的邊界

`fwcore/mailbox.c:fw_handle_mailbox()` 先驗非零 seq 與 live session。
如果 seq 等於最近已接受的 seq，必須比較完整 opcode、target、args；相同才回 cached reply，
不同則 SEQ_CONFLICT。比最後 seq 舊的請求回 STALE_SEQ。這是「單一 outstanding、最後一筆 cache」
的去重設計，不是無限歷史的分散式 exactly-once 系統。

PF 的 `toy_mailbox_status()` 寫欄位後 SUBMIT，再同時檢查 DONE 與 ACK_SEQ；單靠 IRQ 到達不能判斷
是這筆回覆。若裝置回 IDLE 且允許 retry，就重送相同 staging request，不重新做 fence 或增加 sequence。
讀者可以把「同 key 同內容」「同 key 不同內容」「舊 key」「新 session」列成四格，逐一找到 return 分支。

## 重要實作四：logical scheduler 與效能的區別

`fwcore/scheduler.c:fw_schedule_round()` 每輪最多分配 64 credits。先篩 ACTIVE、unfenced、非 killed、
BME、config_valid、沒有 delayed outstanding 的 VF，給 `min(reserve, backlog)` 基本配額；
剩餘以 reserve 作權重分配，先取整數，再依餘數補足，相同餘數用 round 做輪替。
需求較少的 VF 先封頂，再重算其他 VF 的份額；迴圈由八個 VF 的固定上限限制。

所有 serviced counter 先驗溢位，再一起 commit，避免半數已加、後半數失敗。
這些測試核對 logical work allocation 的規則，不能拿 credits 或 TCG wall time 當成實體裝置 bandwidth／P99。
這段邏輯由 M0 native scheduler 測試覆蓋；不包含實體裝置的 QoS 壓力或效能驗證。

## 重要實作五：為何 SQ index 不等於 CQ index

這個缺陷位於 `guest/tenant/owner.c:owner_workload()`。
例：SQ tail=6、CQ head=5，表示先前有一筆 stale SQ 被 consume，但沒有產生 CQ。
下一筆請求寫 SQ slot 6，也把 payload 放在 slot 6 對應的區段；completion 卻會寫 CQ slot 5。
若用 CQ index 找 payload，就會核對到別筆 buffer。

修正後兩個索引分開計算：

```c
/* CQ entry 依完成游標定位。 */
uint32_t cq_index = lab_ring_index(o->cq_head, o->entries);
/* Payload 依原提交游標定位；CQ id 另行核對。 */
uint32_t payload_index = lab_ring_index(start + received, o->entries);
```

這是為閱讀整理的等價片段，實際變數名與 guard 以可點擊源碼為準。
`tests/guest/test_owner_workload.c` 執行真實 workload 函式，以模擬的 pread/pwrite device boundary
回覆 CQ；分別測正常游標、相差一筆、跨 UINT32_MAX，N=16/32/64/128/256 共 15 組。
這是 native regression；T03/T09 和完整 M3 另驗 guest 路徑。


## 重要實作六：Linux driver 的 UAPI、鎖與中斷

先打開 [PF control 實作](code/guest__pf__toy_control.inc.html)，由 `toy_fops` 找到
`toy_ioctl()` → `toy_control_ioctl()` → `toy_execute_ctl()`。這份 .inc 被
`guest/pf/toy_pf.c` 納入同一編譯單元，不是另一個 kernel module。

一筆 REARM 的閱讀順序：

1. `file->private_data` 取得本次 open 的 `toy_file`，再取得共同的 `toy_pf`。
2. 取得 `admin_mutex` 並檢查 `dying`；若裝置已移除，返回 ENODEV，不再讀 BAR。
3. `copy_from_user` 只複製到 kernel local value。檢查 size、ABI major/minor、reserved=0、
   VF 範圍與 command-specific flags，避免直接信任 userspace struct。
4. `operation` 做 per-open replay 檢查：相同 operation 必須 command 和完整 request 相同；
   舊 operation 拒絕。同一個 fd 的 cached result 和 firmware mailbox cache 是不同層。
5. 核對 live session/epoch、lease owner/token、ready_gen，以及 FENCED、CONFIG_VALID、BME、
   READY_PENDING 和 outstanding=0。每個條件回答不同問題，不能只檢查「enabled」。
6. 發 mailbox REARM，再讀回 ACTIVE、config、epoch、fence/kill。回覆成功仍需驗證硬體後置條件。
7. 保存結果再 `copy_to_user`。即使最後 copy 失敗，已執行的操作不能當作未發生；保留 cache
   讓同一 operation 可取回原結果，避免重做副作用。

這是原始碼行為的解釋；本版整合覆蓋仍以 T01–T10 的實際 case 為準，不表示每種 EFAULT 時序都已注入。

| 同步機制 | 保護什麼 | 讀碼時檢查什麼 |
|---|---|---|
| `admin_mutex` | PF 管理交易、lease、files list、共同 snapshot | 管理操作序列化；不能在持有它時等待需要同一把鎖的 worker |
| `mbx_mutex` | 一次 outstanding mailbox request | 管理路徑按 admin → mailbox 順序，不反向取得 |
| `event_lock` | 每個 open 的 event ring、count、lost | spinlock 內只做短操作；不包住可能 fault/sleep 的 copy_to_user |
| `read_mutex` | 同一 fd 的讀者與暫存 event | copy_to_user 失敗保留 stash，下一次 read 仍能取回 |
| `kref` | `toy_pf` 記憶體壽命 | open 增加引用、release 減少；與裝置是否可 MMIO 是兩件事 |

接著讀 `guest/pf/toy_pf.c:toy_irq()`。IRQ handler 讀 cause、ACK，對 mailbox 呼叫
`complete()`，將其他事件累積後排入 workqueue；它不取得會睡眠的管理 mutex。
`toy_notify_work()` 在可睡眠的 context 取得 admin_mutex，擷取一致快照，再發布 event。
因此「IRQ 喚醒」與「查詢完整狀態」被分成不同 context，應區分各自允許做的事。

最後讀 `toy_read_file()`：先 wait_event，短暫取得 event_lock 將一筆資料搬到 stash，
放開 spinlock 後才 copy_to_user。若 event ring 滿，記錄 lost count，之後明確回 LOST_EVENTS，
不把資訊遺失偽裝成完整事件序列。這個通知機制提醒管理者重新讀取 authoritative snapshot。

## 重要實作七：probe 失敗的反向回收

從 `guest/pf/toy_pf.c:toy_probe()` 把取得資源的順序寫下來，再沿 goto label 反向核對：

| 取得資源 | 出錯後對應處理 | 為何順序重要 |
|---|---|---|
| context／kref | 最後 kref_put | IRQ/work/fd 若仍會使用 context，不能提早 free |
| pci_enable_device_mem | pci_disable_device | 先停用裝置存取及回收上層資源 |
| pci_request_regions | pci_release_regions | 先解除 BAR mapping 才歸還 region ownership |
| pci_iomap | pci_iounmap | IRQ/work 已停止後才能讓 BAR pointer 失效 |
| MSI-X vectors、已成功 request 的 IRQ | free_irq、pci_free_irq_vectors | 用 irq_count 回收已取得的部分，不假設兩個 IRQ 都成功 |
| workqueue callback | cancel_work_sync | 舊 worker 不得在 context/BAR 釋放後執行 |
| BME、driver data、misc device | fence／clear master／移除入口 | 阻止新的操作進入正在拆除的裝置 |

T02 在真實 probe、owner setup 和 PCI enumeration 路徑注入失敗，驗證回收與可重試狀態。
對失敗保留 quota 的情況，正確結果是隔離並呈現 FAILED；不是讓所有計數無條件變成零。
先理解這套取得／釋放配對，再讀 `toy_remove()` 的停止入口、同步 IRQ/work、引用釋放。
本版不宣稱已完成 hostile hot-remove 與所有同時持有 fd 的時序矩陣。

## Build 與測試工具也要 trace

從 `tools/verify_project.py` 讀 stage 清單，再跟到各 builder；每個 stage 保存 command／log／exit code。
它先記所有受測輸入 hash，完成後再比對，避免執行到一半改程式卻仍沿用 PASS。
`tools/verify_core.py` 記錄當時 core inputs、binary hashes、build stamp hash 和 raw evidence hashes；
`tools/verify_m0.py` 在 reuse 時核對這些關係。只有「目前 build 符合目前 source」不夠，
還必須證明「這個 PASS 的確測過這個 build」。

證據管道本身也要 trace。`guest/init` 將測試 stdin/stdout 指到 UART1，kernel console 留在 UART0。
沿 `guest/tenant/m1_spike.c:host_checkpoint()`，確認等待 ACK 的 fd 也來自同一條協定 UART，
而不是重新開啟 `/dev/console`；後者仍指向核心 console，會讓事件送得出去、回覆卻永遠讀不到。
它先設 raw/noecho，再送 WAITING，逐 byte 核對 token，成功／失敗都恢復 termios。
`tools/run_uefi.py:read_uefi_output()` 則分別讀取 firmware/kernel 與測試日誌，檢查後才組合成解析輸入。
這條路徑對應 B020 的兩次實際失敗：先是 JSON 被 printk 切斷，再是搬移 UART 後 ACK 讀錯端點。

UEFI 路徑可從 `uefi/SriovFwPkg/SriovProbe.c` 找 `UefiMain`，依序追 core selftest、PCI capability probe、
mailbox version，再看 EFI-stub handoff；實際檔名與入口若需要快速定位，用
`grep -n 'UefiMain' uefi/SriovFwPkg/*.c`。詳見 [UEFI.md](UEFI.md) 的 CODE/VARS、PCD 與 BAR API 語意。

## 沿資料路徑追蹤

先說需求：「我必須在兩個 VF 同時使用相同 IOVA 時，證明裝置只改到各自的 buffer。」
接著說設計：「tenant 各自擁有 viable group/container；裝置每筆 DMA 都帶實際 VF requester。」
再指出程式位置：「從 owner_workload 追到 service_one，再進 dma 的 PCI helper。」
最後拿證據：「T04 比較兩個 owner 的整個 buffer 與 RID；T05 讓 A 不映射該 IOVA，
B 仍映射它，實際觀察 A fault 與 B 的正確進度。」

遇到 reset 問題則先說時間順序與所有權，再列函式。
以 B001 為例，所有 API 都可以成功返回，整個流程仍會因串行等待跨越 deadline 而失敗。
這正是需要狀態圖、時序圖、raw log 與回歸測試一起判斷的原因。
