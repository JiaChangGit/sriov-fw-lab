# SR-IOV 隔離與生命週期指南

本指南整理系統設計、故障處理及其驗證界線。實際執行結果見 [VALIDATION.md](VALIDATION.md)。

本章用問題檢查設計推理；完整狀態／資料流見 [系統分析](SYSTEM_ANALYSIS_ZH.md)，
實際下命令定位問題見 [BUG 定位、反證與修正](DEBUGGING_ZH.md)。
第一次作答時先遮住說明，寫出「管理者、guard、失敗後保留什麼、如何驗證」四項，
再與答案比對。只記術語定義，無法判斷異常路徑是否安全。

## 系統分析：把需求轉成可檢查的條件

| 需求 | 設計決定 | 故障時必須觀察什麼 |
|---|---|---|
| VF A 不能碰 VF B 的 memory | 每 VF 獨立 viable group/container；DMA 使用該 VF PCI requester | A fault、B guard 不變；不能只檢查 map flags |
| reset 後不能讓舊工作完成到新 ring | 關 gate、advance generation、取消 retained work；callback 最後再次核對 | 舊 callback 確實執行但被丟棄；新舊 buffer/CQ/IRQ 均需檢查 |
| 失敗時不能假裝資源已釋放 | FAILED 保留 reserve；driver 確認 owner 關閉、core 確認硬體清理條件後才釋放 ledger | reserve、outstanding、lease、PCI presence、fd/process 都需收斂 |
| timeout 仍可停止 DMA | 硬體 fence register 不依賴 firmware mailbox worker | readback 確認；mailbox timeout 本身不等於成功停止 |
| 重送不能重複副作用 | session + sequence + 逐欄比較完整 request + reply cache | 原回覆重現、resource count 不增加、內容衝突被拒 |
| 筆電可重跑 | JOBS=2、一個 guest、TCG 必要、來源/patch/config/build hash | 失敗 exit code、timeout、缺 log 不得轉 PASS |


## PF driver 閱讀順序

可以依下列順序追蹤 PF driver：

1. **Probe 與 unwind**：畫出 BAR、IRQ、misc device 的取得順序，挑第二個 IRQ 註冊失敗說明反向回收。
2. **UAPI 與 ownership**：從 open file context 說明 lease、ioctl header、replay cache，解釋為何不能只拿 PID 當 owner。
3. **IRQ 與同步**：說明 IRQ handler、workqueue、process context 的工作分配，以及 admin → mailbox 的鎖順序。
4. **Lifetime 與 DMA**：區分 kref、dying、fence、UNMAP，最後連到 reset 時序圖。

應區分「CPU 正在執行的程式」與「裝置仍可能執行的工作」。
Mutex 可以保護 driver 的管理資料，卻不會自動取消已經取走的 descriptor。
因此必須同時檢查 driver 的同步與裝置工作的取消流程。

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
應區分實際測試層級與程式的相容性設計。

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

B018 可用兩條 ring 說明：stale descriptor 前 SQ/CQ 同步，
丟棄 stale SQ 後 SQ 比 CQ 多一筆；下一筆工作使用 SQ slot 6，但完成記錄位於 CQ slot 5。
問題不是 CRC 算法錯，而是用完成位置猜原請求的 payload 位置。
修正後將 payload 定位改回 submission cursor，並加入五個 N、正常／差一／wrap 共 15 個 native regression，
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
測試結果受執行路徑和工具能力限制。

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

## 情境推演：修改條件後會發生什麼

### 情境一：Tenant 已寫完 queue bases，可否立即 doorbell？

**先預測。** 此時 core 可能已是 ACTIVE，且 SQ／CQ 的 IOVA 都合法；還缺哪個條件？

**推導。** 分次寫入只更新 staging。`config_commit()` 驗證完整設定後才複製到 live config，
並設為 READY_PENDING。接著 owner 的 READY tuple 必須經 labd／PF 核對，
`fw_rearm()` 再確認 epoch、ready_gen、BME、fenced、無 outstanding、未 killed 和 deadline，才由 HAL 開 gate。
單獨寫 bases 或 CONFIG_COMMIT 都不是 DMA 授權。

**讀碼位置。** [`device/toy_vf.c`](../device/toy_vf.c) 的 `config_commit()`／`write_vf()` →
[`fwcore/admission.c`](../fwcore/admission.c) 的 `fw_rearm()` →
[`device/fw_adapter.c`](../device/fw_adapter.c) 的 `hal_gate()`。

**檢查答案。** 應同時說出「staging 與 live config 不同」和「READY_PENDING 仍 fenced」。
若只說 queue 地址是否合法，尚未回答誰有權開 DMA。

### 情境二：A 已 reset，舊 callback 延遲到新 queue RUNNING 才到達

**先預測。** 新 queue 已 RUNNING，為何 gate 還能拒絕舊 callback？

**推導。** 新 live tuple 的 epoch/token 已變；舊 operation 帶著當時的 tuple。
`toy_complete()` 先比 `same_generation()`，不符便增加 STALE_CPL 並消耗該 operation。
即使目前 queue 合法、BME 打開、core ACTIVE，這筆舊 operation 仍不屬於現在的世代。
新 queue 可工作與舊工作可完成，是兩個不同命題。

**如何反證修正不完整。** 如果僅檢查 callback 在 fenced 階段被擋，會漏掉重開 gate 後的情況。
T08 分別在 fresh config 仍 fenced，以及 REARM 後執行真實 retired callback，檢查新舊 buffers、CQ 和 IRQ。
應從 [`validate_recovery()`](../tools/validate_recovery.py) 找到兩次 callback 的必要條件。

### 情境三：Payload 已改變，但寫 CQ phase 時 fault

**先預測。** 可否直接發 IRQ，讓 tenant 知道 payload 完成了？

**推導。** 不可。協定以有效 CQ entry 宣告結果；phase 尚未成功發布，就沒有可接受的 completion。
`toy_complete()` 先寫 body，最後單獨寫 phase；任何 CQ DMA 失敗就 `toy_fail()`，
不會經成功路徑增加 CQ tail 或發 completion IRQ。payload 的部分或全部副作用可能已存在，
錯誤處理不能宣稱跨所有 DMA 有 transaction rollback。

**檢查答案。** 應說明 owner 必須把受影響的 outstanding request 當作失敗處理並協調 reset，
不能因 buffer 看起來像預期值就自行宣告完成。讀碼位置在
[`toy_complete()`](../device/toy_hw.c) 的 body／phase 寫入與 `s->cq_tail++` 之間。

### 情境四：VFIO UNMAP 回傳長度小於 requested，接著 close 成功

**先預測。** 最後所有 fd 都關掉，是否可以把整個 cleanup 改成成功？

**推導。** 不能。安全回收與測試是否成功是兩件事。程式可在 UNMAP 不完整時保留 allocation，
先關閉 owner 的 VFIO device／group／container，讓映射生命週期收斂後才釋放 RAM；
但應保留先前的 failure。否則就把已發生的契約違反藏在成功 teardown 裡。

**讀碼位置。** [`lab_vfio_close()`](../guest/tenant/vfio.c) 的 `failed` 累積與
[`guest/tenant/owner.c`](../guest/tenant/owner.c) 的 owner mapping cleanup。
應辨認 baseline VFIO helper 與三種 OWNER_MAP 的實作各自保存哪些狀態，不要將兩套結構混用。

### 情境五：FREE、reserve=0，但 flags 同時有 KILLED

**先預測。** 是否可直接開始下一輪 PROVISION？

**推導。** 不可只看 FREE。`fw_create()` 拒絕 killed hardware；FREE 是 core 資源狀態，
KILLED 是硬體 gate 狀態。FAILED 經 DESTROY／RELEASE 可以釋放 ledger，
但 KILL 仍保留到 global recovery。乾淨基線要檢查全部 slots 的 core、flags、config、outstanding、lease。

**如何驗證。** 對照 [`fw_create()`](../fwcore/admission.c)、
[`fw_global_recover()`](../fwcore/context.c) 及測試 validator 的 final snapshot。
若只是將 state 或 reserve 手動歸零，沒有證明全域復原前提成立。

### 情境六：本地圖文正常，推上 GitHub 後連結失效

**先預測。** 問題可能不在 Markdown 語法，還缺哪項發布條件？

**推導。** 本地存在的 ignored artifacts、build outputs 或 HTML viewer 不一定能在 GitHub 取得。
公開入口應指向 tracked Markdown、SVG 與 source；離線 HTML 供下載後開啟。
本地連結檢查還需採公開檔案集合，不能因開發目錄剛好存在 raw log 就建立可點擊連結。
B023 是這種差異的實際案例。

**檢查答案。** 應分開驗證相對路徑／anchor 正確，以及 target 真的會隨 repository 發布。
教材生成成功只表示文件檢查通過，不能替代裝置／driver 的 M0–M3 驗收。

## 不看程式先回答，再用原始碼修正答案

| 問題 | 答案至少應包含 | 用哪裡確認 |
|---|---|---|
| 一個 VF 有幾筆 delayed operation？ | 每 VF `pending` 最多一筆；CQ capacity 仍須足夠 | `service_one()`、`ToySlot` |
| 舊 descriptor 為何與舊 callback 分開計數？ | 前者在 fetch 後判斷 descriptor epoch；後者帶已保存 tuple，在完成入口被拒 | `service_one()`、`toy_complete()` |
| 哪裡提交 ledger 釋放？ | owners_closed、無 PCI/config/outstanding 且 guard 成立後，由 core 釋放 | `fw_transition()` 的 RELEASE_OK／ABORT_OK |
| Deadline 到了是否一律同一事件？ | QUIESCING／RESETTING 用 EV_TIMEOUT；RECOVERING 用 VERIFY_FAIL，且無 PCI 時等待 DESTROY | `fw_poll_deadlines()` |
| 觀測欄位讀取時點為何重要？ | 同一欄位可在呼叫中被消耗或改寫，必須定義 before／after | B010、`toy_complete()` |
| 哪個測試能證明實體吞吐？ | 本版沒有該證據；TCG 只支持受測模型與功能結論 | VALIDATION 與本版範圍 |

作答不完整時回到對應函式，補上輸入、guard、狀態變化與一個失敗分支。
能說出名稱之後，下一步是指出哪一行阻止反例，以及哪一個測試會在那行被移除後失敗。

