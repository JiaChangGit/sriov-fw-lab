# 面試介紹：用證據解釋 SR-IOV 的隔離與生命週期

目標職缺是 Linux kernel／driver／虛擬化工程師。專案涵蓋 M0–M3／T01–T10，
以 PCI 裝置、VFIO、DMA、中斷與資源生命週期為主軸。
講解時以 [VALIDATION.md](VALIDATION.md) 的實跑範圍為準，不說「沒有任何 BUG」。

## 90 秒版本

我做的是一個可在 16 GB Windows 筆電的 WSL2 執行的 SR-IOV 教學平台。
QEMU 模擬一個最多支援八個 VF 的 PCIe 加速器，guest Linux 的 PF driver 負責管理，
不同 tenant 透過 classic VFIO 使用各自的 VF。資料工作是 FILL 和 CRC32，
因此可以把重點放在位址隔離、重置時序及資源所有權。

我把問題拆成三部分：IOMMU 決定 requester 可以碰哪個位址；PF-only fence
決定裝置現在能否 DMA；session、epoch、cancel token 決定 callback 是否還屬於這一代。
這三種保護不能互相取代。控制面則由 portable C core 管理 VF 生命週期的 FSM 和資源 ledger，
Linux driver 管理 host transaction、lease、IRQ 與 fd lifetime。

我用真實 VFIO MAP／UNMAP、VF requester DMA、整個 buffer 的檢查及故障注入驗證行為。
例如兩個 VF 使用相同數字 IOVA，A 沒有映射時必須 fault，B 仍能完成正確工作。
另一個實際問題是四個 owner 串行關閉，讓最早 close 的 VF 等到 READY timeout；
我修正停止與等待的安排，保留原來的 deadline，再以 100 輪測試確認資源回到基線。

這是軟體裝置模型，沒有實體板卡效能或硬體功耗的結論。驗證結果保存了
原始紀錄和版本雜湊，並提供清空 build 後重新建置與驗證的命令。

## 五分鐘版本：每段回答一個工程問題

| 時間 | 問題 | 展示內容 |
|---|---|---|
| 0:00–0:45 | 為何需要這個系統？ | 在一般筆電重現隔離、VF lifecycle、DMA fault，讓錯誤可觀察、可重播 |
| 0:45–1:30 | 誰擁有什麼？ | 架構圖；core 擁有 ledger/FSM；PF 擁有控制序列；tenant 擁有 VFIO fd/mapping |
| 1:30–2:20 | 一筆請求怎麼走？ | SQ → VF requester → IOMMU → payload → CQ phase → MSI-X/eventfd |
| 2:20–3:20 | reset 為什麼難？ | fence/readback、cancel/drain、舊 owner UNMAP、新 CONFIG/READY/REARM 的時序 |
| 3:20–4:20 | 怎麼知道沒有假通過？ | 同 IOVA 的兩個 domain、guard、故障 RID、healthy VF 進度、raw log 再解析 |
| 4:20–5:00 | 找到什麼 BUG？ | B001 串行 close 的 deadline 問題，或 B002 requested flags 與有效權限不同 |

不要先列所有 API。先指出一個必須維持的條件，再給設計、程式位置、反例與證據。
若面試官追問「完整做到哪裡」，直接打開本版需求與實測表，說明刪減的範圍和原因。

## 系統分析：把需求轉成可檢查的條件

| 需求 | 設計決定 | 故障時必須觀察什麼 |
|---|---|---|
| VF A 不能碰 VF B 的 memory | 每 VF 獨立 viable group/container；DMA 使用該 VF PCI requester | A fault、B guard 不變；不能只檢查 map flags |
| reset 後不能讓舊工作完成到新 ring | 關 gate、advance generation、取消 retained work；callback 最後再次核對 | 舊 callback 確實執行但被丟棄；新舊 buffer/CQ/IRQ 均需檢查 |
| 失敗時不能假裝資源已釋放 | FAILED 保留 reserve；driver 確認 owner 關閉、core 確認硬體清理條件後才釋放 ledger | reserve、outstanding、lease、PCI presence、fd/process 都需收斂 |
| timeout 仍可停止 DMA | 硬體 fence register 不依賴 firmware mailbox worker | readback 確認；mailbox timeout 本身不等於成功停止 |
| 重送不能重複副作用 | session + sequence + 逐欄比較完整 request + reply cache | 原回覆重現、resource count 不增加、內容衝突被拒 |
| 筆電可重跑 | JOBS=2、一個 guest、TCG 必要、來源/patch/config/build hash | 失敗 exit code、timeout、缺 log 不得轉 PASS |


## Kernel 職缺的講解順序

如果面試官關心的是 kernel 開發，先用兩分鐘說明 PF driver，再展開整個系統：

1. **Probe 與 unwind**：畫出 BAR、IRQ、misc device 的取得順序，挑第二個 IRQ 註冊失敗說明反向回收。
2. **UAPI 與 ownership**：從 open file context 說明 lease、ioctl header、replay cache，解釋為何不能只拿 PID 當 owner。
3. **IRQ 與同步**：說明 IRQ handler、workqueue、process context 的工作分配，以及 admin → mailbox 的鎖順序。
4. **Lifetime 與 DMA**：區分 kref、dying、fence、UNMAP，最後連到 reset 時序圖。

白板上至少保留兩欄：「CPU 正在執行的程式」與「裝置仍可能執行的工作」。
Mutex 可以保護 driver 的管理資料，卻不會自動取消已經取走的 descriptor。
能把這兩欄接起來，比列出用過多少 API 更能說明你理解 driver 的責任。

### 為何 insmod 成功還要檢查 probe？

Module registration 成功和裝置綁定成功是兩件事。T02 會檢查失敗時沒有 control node，
並在移除模組後乾淨重試，確認同一個裝置可以正常 probe。只看 insmod 的 return code，可能漏掉裝置初始化失敗。

### 為何 kref 與 dying 都需要？

kref 確保仍被 file context 引用的記憶體不被提前釋放；dying 阻止已移除裝置的 MMIO。
物件存在不等於 BAR 還有效。回答時沿 toy_open、toy_remove、toy_release_file 各指出一次 reference 操作。

### 為何 copy_to_user 要放在 event spinlock 外？

Copy 可能遇到 userspace page fault，不能把它放在需要短暫持有的 spinlock 區段。
這裡先將事件搬到 per-open stash，放開鎖後複製；若 copy 失敗，stash 保留供下一次讀取。
Read mutex 序列化同一個 open context 的讀者，event lock 則保護短小的 queue 操作。

### Native 32-bit layout check 能代表 32-bit guest 實測嗎？

不能。它檢查 UAPI 結構的大小、offset 與編譯配置；整合 VM 是 x86_64。
Driver 提供 compat_ioctl 的安排，不代表已跑過所有 32-bit userspace 系統呼叫。
面試時應先說實際測試層級，再說程式的相容性設計。

## 三種狀態要分開說

Firmware 的 ACTIVE 表示 slot 有可用的已 provision 資源，**不單獨保證 queue 正在執行**。
第一次 PROVISION commit 後，VF 仍可能 FENCED 且尚未 CONFIG。
是否能 DMA，還要查 config_valid、BME、FENCED、KILLED、generation 及 queue 狀態。

Owner 的 lease/fd/mapping 狀態是另一個維度。Firmware FREE 不會自動關閉 userspace fd；
close VFIO fd 也不等於 driver 可以忽略尚未完成的 lease transaction。
用一個「enabled」布林值代表所有層，會丟掉失敗回復需要的資訊。

## 常見追問與回答重點

### 為何不把 SR-IOV 當作 IOMMU 隔離保證？

SR-IOV 提供 functions 與 requester 身分；隔離仍取決於拓樸、group、IOMMU domain、
mapping 及 DMA 的實際 requester。這個模型檢查每 group 的成員，禁止 ACS override，
並讓兩個 owner 映射相同 IOVA 做正反向測試。

### 為何關 BME 還不夠？

Tenant 能經 VF config 存取自己的 BME。管理者的安全保證需要 VF 無法繞過的 PF-only gate，
並在 payload/CQ/IRQ 的真正執行處檢查。BME 仍是必要的 PCI bus mastering 條件，但不是唯一權限。

### 為何先 fence，再 UNMAP？

UNMAP 是 owner 的位址映射操作；裝置可能已有 descriptor copy 和排程中的 callback。
先確認 fence、generation advance 與 outstanding drain/cancel，才能撤映射。
若 UNMAP size 不符，不可立刻 free buffer；保留 allocation，直到確認 VFIO fd 全部關閉。

### eventfd 的數值是否等於 completion 數量？

不能這樣假設。IRQ 可合併或遮罩，eventfd 是通知；有效資料以 CQ phase、id、epoch、status 為準。
Tenant 被喚醒後 drain CQ，再交還 CQ_HEAD。測試同時核對 IRQ 行為和實際 completion 資料。

### 為何要有 session、epoch 和 token？

Session 區分全域 recovery 前後；epoch 區分該 VF 的世代；cancel token 區分硬體取消的工作。
Lease token 表示管理所有權，ready_gen 表示新的 queue configuration 已完成。
它們的用途不同，不能拿一個單調數字替所有欄位作模糊的「版本檢查」。

### 裝置 callback 已序列化，為何還需要 lifetime 管理？

本模型的裝置 callback 依 QEMU BQL/main loop 的規則執行；guest 仍有 SMP、IRQ、workqueue、open fd 和 remove。
即使裝置 callback 依序執行，PF driver 仍須管理鎖順序與引用生命週期，防止 guest 端的 UAF。
本版沒有 KCSAN／hostile remove 全矩陣，不能以模型序列化宣稱已驗證所有 guest races。

### 規格跟 upstream 行為不一樣怎麼辦？

T05 是具體例子：VFIO requested WRITE-only 在 pinned Intel IOMMU second-level mapping
中實際被加上 READ。先保留真實失敗與來源定位，再增加預設關閉、只限 toy VF 的 pinned patch，
使嚴格權限成為明確的實驗條件。不能把通過的客製 kernel 結果描述成 stock Linux 保證。

### CRC 能不能證明 secure firmware update？

CRC 能找出部分資料毀損，不能驗證作者身分。本版的 CRC32 是資料工作負載，
不提供 capsule／A/B update，也沒有信任根或簽章驗證；不能把 checksum 稱為 secure update。

## 用一個 BUG 說明除錯方法

### SQ 與 CQ 不一定同步前進

B018 適合白板講解，因為只需要畫兩條 ring：stale descriptor 前 SQ/CQ 同步，
丟棄 stale SQ 後 SQ 比 CQ 多一筆；下一筆工作使用 SQ slot 6，但完成記錄位於 CQ slot 5。
問題不是 CRC 算法錯，而是用完成位置猜原請求的 payload 位置。
我把 payload 定位改回 submission cursor，加入五個 N、正常／差一／wrap 共 15 個 native regression，
保留舊碼實際 EILSEQ 失敗，再跑 sanitizer 與整合 gate。這也說明為何要測異常後的正常工作，
只測異常回覆和 teardown 可能漏掉後續狀態污染。

### 跨 owner 的時間累積

以 B001 為例，先展示原始 FAIL，再問哪一層失敗。所有 UNMAP、close、reap 都成功，
但最後硬體 flags 是 3，代表 FENCED 加 KILLED。因此問題不在 mapping size 或 child leak。
往前對齊 FLR、RECOVERING、deadline 與各 owner 關閉順序，才找到時間累積的根因。

修正沒有延長 deadline：先送出所有停止，再等待每個 owner；最後一個 lease 釋放後立即 disable。
驗證也不是只重跑到一次成功，還強化 validator 的跨 owner 順序檢查，保留兩次 100 輪證據，
並跑受影響的 M1/M2/M3 回歸。完整狀態請見 [BUG_LOG.md](BUG_LOG.md)。

## 更深入的追問

### 為何不用一個 global mutex 包住全部操作？

Mutex 能序列化部分 host threads，無法回收已被 device fetch 的 descriptor，也不能代替 VFIO domain。
還要考慮 IRQ callback 不能睡眠、等待 ACK 的 thread 不應阻擋完成它的 worker。
本版 admin_mutex → mbx_mutex 有固定順序；remove／fd lifetime 另需引用保護。不要宣稱一把鎖解決所有時序問題。

### DMA helper 成功一次，整筆工作就成功了嗎？

不是。SQ fetch、payload 各 chunk、CQ body、CQ phase 都可能各自 fault。
每次 helper 都核對回傳值；跨頁寫可能部分成功，不保證 transaction rollback。
即使 payload 完成，CQ 失敗也必須保守 fence，不能只回一個猜測成功的 IRQ。

### 為何需要獨立 oracle？

把實作表格直接複製成預期值，兩邊可能一起錯。Native Python oracle 以獨立模型核對 C core 操作，
ring oracle 以 FIFO 比較 u32 wrap；guest validator 則從 raw records 核對跨層關係。
它們提供不同角度，但仍受輸入預算與模型假設限制。

### KASAN 通過能證明沒有 race 嗎？

不能。KASAN 幫助偵測受測 kernel 路徑的越界與 UAF，不等於 race detector。
本版只保留已驗證的 KASAN 情境；沒有 KCSAN 與所有 hostile hot-remove 的實測結果。
對面試官應說明覆蓋邊界，而不是把 sanitizer 名稱當成保證。

### 跨 u32 wrap 為何仍能算 pending？

Counter 使用 unsigned arithmetic，`tail - head` 按 2^32 取模；前提是有效 outstanding 永遠有界且不超過 N。
Array index 再取 `counter & (N-1)`，phase 依 counter/N 的圈數交替。
不能用 signed subtraction，不能只保存 masked index，也不能忽略 `pending > N` 的非法輸入。

### 為何 retained quota 比直接歸零更安全？

失敗的 release 表示仍不確定資源是否可重用。歸零會讓下一個 owner 再次分配同一份資源，
把未清理的狀態交給別人。保留 quota 限制可用容量，但讓錯誤顯性存在，直到明確 recovery 成功。

### 軟體模型的價值及限制是什麼？

價值是可控制 requester、logical clock、fault checkpoint 與 retained callback，使跨層錯誤可重現。
Linux PCI/VFIO/IRQ 路徑實際執行，能練習 driver 與協定設計。
限制是沒有真實 PCIe 電氣／cache coherency／功耗／吞吐與板級韌體部署證據。

## 10 分鐘現場展示腳本

完整命令與報告讀法見 [DEMO 操作手冊](DEMO_ZH.md)。

| 時間 | 動作 | 要講的重點 |
|---|---|---|
| 0:00–1:30 | 90 秒介紹，指出本版範圍 | 問題是跨 VF 隔離與 reset ownership |
| 1:30–3:00 | 架構＋正常 I/O 時序 | 分清 guest CPU VA、IOVA、requester、completion phase |
| 3–5 分 | `make demo-reset` 或打開已保存 T07 結果 | 一個 VF reset，另一個仍完成正確資料；先 fence 後 UNMAP |
| 5–7 分 | 點 `toy_complete` 與 `owner_workload` | 最終 generation guard、SQ/CQ 索引分離 |
| 7–9 分 | B018 原始 FAIL 與 regression | 假設、反例、修正與覆蓋增強 |
| 9–10 分 | `project-latest.json`、範圍與限制 | 可重現證據；不作實體硬體或零 BUG 主張 |

展示前完成完整 gate、接上電源、關閉其他大型 VM，預先打開六張圖與結果。
現場不重新編譯整套 kernel；若 demo 失敗，保留當次 log，先指出是 build prerequisite、guest boot 還是 case failure。
已保存的結果要報出它自己的版本與時間，不能拿舊 PASS 覆蓋現場 FAIL。

## 新手自我檢核與履歷描述

準備好回答以下五題，才算能自己介紹這個專案：

1. 從 `owner_workload` 追到 `dma`，說出每一層的執行位置與位址種類。
2. 在 reset 圖上標出舊 mapping 最早何時能撤除，哪個回傳值證明它完全撤除。
3. 解釋 ACTIVE、RUNNING、lease acquired 三者為何不同。
4. 用一個 raw artifact 說明 PASS 不只靠 log 字串。
5. 選一個 BUG，先說錯誤假設，再說反例、修正和測試，最後說仍未覆蓋的範圍。

履歷可寫：「在 16 GB WSL2 筆電建立 QEMU SR-IOV 教學加速器，整合 portable C firmware core、
Linux PF driver 與 VFIO tenant；以同 IOVA 隔離、DMA fault、reset/stale callback、KASAN 與原生 fuzz/oracle
驗證 M0–M3，建立可追溯建置流程和故障紀錄。」
只有在本版完整結果確實 PASS 後才使用「驗證完成」，並能逐段解釋設計、程式及實際測試結果。

## 英文摘要

I built a software SR-IOV lab on a 16 GB Windows laptop using WSL2, QEMU and a custom Linux guest.
The device exposes a PF and up to eight VFs. Each tenant owns its classic VFIO container, group,
device descriptors and DMA mappings. A portable C core owns the device state machine and resource ledger.

The central design separates address isolation, permission to issue DMA, and generation validity.
The IOMMU limits addresses for each VF requester. A PF-controlled fence stops device access.
Session, epoch and cancellation tokens prevent old work from touching newly configured queues.

My tests use actual VFIO mappings, requester-specific DMA traces, complete buffer checks,
fault injection and raw-log validation. A useful debugging example was a teardown timing failure:
serial owner shutdown allowed an earlier VF's recovery deadline to expire. I changed the shutdown
ordering and verified repeated lifecycle runs without relaxing the deadline.

The deliverable is scoped to M0 through M3. I separate software-model evidence from physical hardware
claims, and I do not claim production readiness, real-device performance or authenticated firmware updates.
