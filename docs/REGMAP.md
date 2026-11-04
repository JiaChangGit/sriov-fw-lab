# MMIO / mailbox / descriptor contract

契約來源：v3.0 HTML §6–§9、§17；本版依 REQUIREMENTS.md 保留 M0–M3，更新功能設計已移至 archive。下面描述介面與必要條件；實作以 spec/abi.json、include/toy_regs.h 及各層程式為準，實跑覆蓋見 VALIDATION.md。ABI major=1/minor=0；欄位固定 little-endian。C header、QEMU model、PF driver、tenant 與 UEFI 必須使用一致常數。任何更動需版本及 DECISIONS 記錄與受影響 gate 重跑。

PCI identity：vendor 0x1234、PF 0xe001、VF 0xe002、revision 0x01；class code=0x058000。Total/Initial VFs=8；1 SQ/CQ per VF；PF 2 MSI-X（mailbox0/health1）；VF 各 2，completion 使用 vector 0，vector 1 預留給 health，本版沒有主動使用它的正常路徑。PF/VF BAR0=64KiB/16KiB，64-bit non-prefetchable；BAR1是highhalf。BAR2=16KiB 32-bit MMIO，table0/PBA0x1000；BAR3–5不用。

## 07｜完整 MMIO 暫存器契約

PF/VF 的 toy BAR0 register callback 僅接受 aligned 32-bit little-endian 存取（MSI-X BAR2 由 QEMU helper 另行處理）；其他 size/alignment 回固定零值、計 BAD_MMIO、不觸發副作用。64-bit 值拆 low/high；VF 設定先寫 staging，最後 CONFIG_COMMIT 才一次生效。所有 RO 欄位寫入忽略並計 BAD_MMIO；reserved 位必須零，非法命令以 status 回報。Mailbox/PF 命令由 device event loop 同步 snapshot，耗時處理放 BH/timer。

### PF BAR0（64KiB）

| Offset | 名稱 / 權限 | 定義 |
| --- | --- | --- |
| 0x000 | MAGIC RO | 0x5352494f（SRIO） |
| 0x004 | ABI RO | major=1 放 bits31:16，minor=0 放 bits15:0 |
| 0x008 | CAP_VFS RO | 8 |
| 0x00c | CAP_QUEUES RO | 8 |
| 0x010 | CAP_VECTORS RO | 16（VF only） |
| 0x014 | CAP_CREDIT RO | 64 descriptors / logical round |
| 0x018 | CAP_FEATURES RO | bit0 DMA_FENCE、1 VF_KILL、2 modeled_FLR；bit3 reserved，固定為 0 |
| 0x020 / 0x024 | SESSION_LO/HI RO | 同一 QEMU instance 內的全域 recovery 世代；SNAPSHOT 捕捉一致值，QEMU 重啟後不保證唯一 |
| 0x028 | SNAPSHOT W1 | bit0 捕捉 PF session 與所有 VF RO snapshot；讀者必須由 PF coordinator 序列化 |
| 0x02c | PF_EVENT RO/W1C | bit0 MBX_DONE、1 VF_HEALTH、2 FLR_EVENT；bit3 UPDATE_EVENT 為保留 ABI，本版不產生更新事件；先讀再 W1C |
| 0x030 | PF_CONTROL W1 | bit0 FENCE_ALL；bit1 GLOBAL_RESET；driver 在 GLOBAL_RESET 前要求無 lease 且 VFs 已停用；裝置 HAL 不能查 guest fd。FENCE_ALL 不需 FW 回應 |
| 0x034 | BAD_MMIO RO | saturating u32 計數 |
| 0x040 | MBX_SEQ RW | 1..UINT32_MAX；0 invalid |
| 0x044 | MBX_OPCODE RW | 命令 ID，寫欄位不啟動 |
| 0x048 | MBX_TARGET RW | VF index 或全域 0xffff |
| 0x04c..0x058 | MBX_ARG0..3 RW | 4 × u32，依 opcode 解碼 |
| 0x05c | MBX_SUBMIT W1 | bit0=1 snapshot request；busy 回 BUSY，不改正在執行命令 |
| 0x060 | MBX_STATUS RO | 0 IDLE、1 BUSY、2 DONE；error code 另欄位 |
| 0x064 | MBX_RESULT RO | fw_status 數值 |
| 0x068 | MBX_ACK_SEQ RO | 必須與請求相符 |
| 0x06c..0x078 | MBX_REPLY0..3 RO | 結果 payload；DONE 最後更新，再觸發 PF vector0 |
| 0x07c | MBX_ACK W1 | bit0 清 DONE；不清除冪等 cache |
| 0x100 + i×0x80 + 0x00 | VF_HW_CTL W1 | bit0 FENCE_BEGIN；bit1 KILL；FENCE_BEGIN 在序列化的裝置 callback 中設 fence、取消 pending、清 IRQ，檢查溢位後增加 epoch/cancel token；不可直接寫 epoch |
| 同上 +0x04 | VF_HW_STATUS RO | bit0 FENCED、1 KILLED、2 PCI_PRESENT、3 BME、4 CONFIG_VALID、5 READY_PENDING |
| +0x08/+0x0c | VF_EPOCH_LO/HI RO | 從 SNAPSHOT 讀；硬體層 monotonic u64，FW hang 仍能 FENCE_BEGIN |
| +0x10 | VF_STATE RO | fwcore enum；不以此欄位取代 HW_STATUS 安全檢查 |
| +0x14 | VF_RESERVE RO | quota 0..64；未分配為 0，有配額時為 1..64 |
| +0x18 | VF_OUTSTANDING RO | 最多 1 retained/delayed operation / VF |
| +0x1c / +0x20 | VF_STALE_DESC / VF_STALE_CPL RO | 分開計數；saturating u32 |
| +0x24 | VF_FENCE_BLOCKED RO | DMA/IRQ 的 gate 檢查失敗次數，包含 fence、kill、BME、config、state 或世代條件 |
| +0x28 | VF_DMA_FAULT RO | DMA helper 回失敗的次數，不保證與 kernel fault log 一對一 |
| +0x2c | VF_LAST_FAULT RO | 0 NONE、1 DMA、2 BAD_DESC、3 FW_HANG、4 RESET_TIMEOUT、5 BAD_QUEUE |
| +0x30/+0x34 | VF_SERVICED_LO/HI RO | scheduler 累計分配的 logical credits，並非已完成 DMA 數；snapshot 讀 |
| +0x38 | VF_IRQ_SUPPRESSED RO | IRQ gate 檢查失敗次數，條件不只 fence/kill |
| +0x3c | VF_READY_GEN RO | CONFIG_COMMIT 次數；PF 用 epoch/session 比對 ready request |
| +0x40..0x7f | RESERVED | 讀零、寫忽略並計 BAD_MMIO |

### VF BAR0（16KiB；每 VF 相同）

| Offset | 名稱 | 定義 |
| --- | --- | --- |
| 0x000 / 0x004 | MAGIC / ABI RO | 與 PF 相同 ABI；MAGIC=0x56465249 |
| 0x008 | STATUS RO | 與 PF HW_STATUS 相同的六個 flags（含 PCI_PRESENT/BME）；由 EPOCH_LATCH 捕捉 |
| 0x00c | EPOCH_LATCH W1 | bit0 捕捉本 VF epoch/session/status；只影響 snapshot，不可 bump epoch |
| 0x010/0x014 | EPOCH_LO/HI RO | latch snapshot |
| 0x018/0x01c | SESSION_LO/HI RO | latch snapshot |
| 0x020/0x024 | SQ_BASE_LO/HI RW | staging IOVA |
| 0x028 | SQ_ENTRIES RW | staging N；2 的冪、16..256 |
| 0x02c | RESERVED | 讀零 |
| 0x030/0x034 | CQ_BASE_LO/HI RW | staging IOVA |
| 0x038 | CQ_ENTRIES RW | 必須等於 SQ_ENTRIES |
| 0x03c | CONFIG_COMMIT W1 | bit0：驗 base alignment、尺寸、state/epoch；清 ring index→VALID/READY_PENDING；RUNNING 時拒絕重設 |
| 0x040 | SQ_TAIL RW | tenant release-publish 後的單調 producer index；32-bit modulo |
| 0x044 | SQ_HEAD RO | device 已 consume 的 index |
| 0x048 | CQ_HEAD RW | tenant 已 consume 的 index；device 檢查單調且不超過 CQ_TAIL |
| 0x04c | CQ_TAIL RO | device published count |
| 0x050 | DOORBELL W1 | bit0 schedule worker；只表示 kick，不是 descriptor 數 |
| 0x054 | QUEUE_STATUS RO | 0 UNCONFIGURED、1 READY_PENDING、2 RUNNING、3 ERROR；錯誤碼在 0x058 |
| 0x058 | QUEUE_ERROR RO | 與 fw_status 相同 |
| 0x05c | IRQ_CAUSE RO/W1C | bit0 CQ、bit1 預留 HEALTH；目前 completion 使用 CQ。先讀後 W1C，CQ empty 前不可假定只一筆 |
| 0x060/0x064 | CONFIG_EPOCH_LO/HI RW | staging；CONFIG_COMMIT 必須等於 live epoch |
| 0x068/0x06c | CONFIG_SESSION_LO/HI RW | staging；必須等於 live session |
| 0x070..0x3fff | RESERVED | 讀零；VF 不可在任何 offset 存取其他 VF 或 PF register |

MSI-X BAR2 由 QEMU MSI-X helper 提供 table/PBA 行為；固定 vector count=2。若 vectors masked，就設 pending bit 並在 unmask 依 helper 規則送出。epoch/fence 變更必須清除該 VF 的舊 pending vector，避免解除 fence 後重送過期 IRQ。PF-only KILL 無解除位；只有受控 GLOBAL_RESET 解除，不能在 VF BAR 或 mailbox ENABLE 繞過。

### FENCE token 與未支援的 reserved aperture

| 區域 | 補充契約 |
| --- | --- |
| PF BAR0 0x038 FENCE_TOKEN RW | FENCE_BEGIN 前寫入非零 u32 token；device 依每 VF 的 last_fence_token 判斷重送；全域 recovery 將 last_fence_token 歸零。同 token 重送只維持原 fence，不再次 bump；較舊 token 拒絕，token 禁止 wrap。同 PF 同時只有一個 admin transaction。這個 cache 獨立於 mailbox seq cache。 |
| PF BAR0 0x03c HW_RESULT RO | 0 OK；其餘 fw_status。driver 必須在 VF_HW_CTL 寫後讀回 HW_RESULT 與 HW_STATUS，不只依 PCI posted write 已送出判斷。 |
| PF BAR0 0x4000..0xbfff | 保留位址區，讀零；寫入設 HW_RESULT=UNSUP，沒有 staging buffer。 |
| UPDATE_STAGE / VALIDATE / ACTIVATE / CONFIRM / ROLLBACK | 保留 opcode；合法格式的更新請求回 UNSUP，不執行更新。完整舊設計見 archive，不屬於本版功能。 |


## 08｜SQ/CQ、DMA、記憶體順序與資料流

### 32-byte descriptor：明列 offset，不直接 cast packed pointer

| Offset | SQ | CQ |
| --- | --- | --- |
| 0x00..0x07 | addr u64：payload IOVA | id u16、status u16、len u32 |
| 0x08..0x0f | len u32、op u8、flags u8、id u16 | epoch u64 |
| 0x10..0x17 | epoch u64 | result u64 |
| 0x18..0x1f | arg u64 | phase u8、reserved[7] |

Wire entry 固定為 32 bytes，依下列 offset 編解碼；不以 host C struct 的 sizeof 或 padding 決定位元組配置。op=1 FILL：將 arg 低 8 bits 填入 payload；op=2 CRC32：IEEE reflected polynomial 0xEDB88320、init/final xor 0xffffffff，result 低 32 bits；標準向量 `123456789 → 0xcbf43926`。len=1..4096，flags=0，未知 op、非零 flags 或非法 len/範圍回 BAD_DESC。id 可重用但 tenant 限制同 epoch 下 outstanding id 不重複。session 是 queue contract，不塞進 SQ；device retained callback 持有 epoch+session+token。

### ring 協定與發布順序

每 VF 單 producer/consumer。SQ/CQ base 64-byte 對齊；owner 負責映射完整 ring。CONFIG_COMMIT 驗證設定形狀，不預先檢查全部 IOMMU mappings。Entries N 相同、16..256 且為 2 的次方。使用會回繞的 uint32 累計 counter：pending = tail - head（unsigned 模 2³²）；任何 pending>N 都是 queue protocol error，不能拿 tail 直接 index memory。index = counter & (N-1)。queue reset 清所有 indices=0，CQ 初始 phase=0；第一圈 device phase=1，公式 `1 ^ ((counter / N) & 1)`。

tenant：填 SQ → release barrier → 寫 SQ_TAIL → MMIO doorbell；device：檢查 state/gate → fetch SQ → decode/copy 到固定本地 buffer → 執行 → 寫 CQ bytes 0..23 與 reserved → 最後單獨寫 phase → 更新 CQ_TAIL → MSI-X。tenant：等待 eventfd → acquire load 檢查 phase → 讀 completion → 驗 epoch/id/status → 更新 CQ_HEAD。payload 與 CQ phase 間有 adapter ordering；C `volatile` 本身不足以保證順序。x86_64 tenant 使用 C11 fence 與 VFIO pwrite/pread helper，詳見 [記憶體順序](MEMORY_ORDER.md)。這條路徑的測試不代表 non-coherent 實體平台的 DMA 保證。

CQ 滿時停止取新 SQ；不能覆寫 unread CQ。每 VF 最多一筆 delayed completion，預留 CQ slot 後才能執行 payload；pending 存在時停止 fetch 下一筆 SQ，維持這個預留 slot。descriptor fetch 或 CQ write translation 失敗：fence VF、清 pending callback、FAILED/health fault，保留錯誤計數；payload DMA 失敗若 CQ 仍安全可寫，先回 DMA_ERROR 再 fence。epoch 不符的 SQ 只消耗 SQ、STALE_DESC++，不 DMA payload/CQ，tenant 自行在 reset handshake 後中止舊 request。

### 一次 CRC I/O：資料經過哪些邊界

| 步驟 | 執行者 / 介面 | 必須成立 |
| --- | --- | --- |
| 1 | tenant 用 VFIO MAP_DMA 設 SQ/CQ/payload，CONFIG_COMMIT，通知 labd | VF lease 有效；map permissions 分 READ/WRITE |
| 2 | labd→PF REARM；device 檢查 READY_PENDING | live session/epoch 符合；ACTIVE 且 QCFG valid 才開 gate |
| 3 | tenant 發 SQ_TAIL/DOORBELL | SQ memory 已 release-publish |
| 4 | device 以該 VF requester DMA fetch | BME=1、fence=0、kill=0、state ACTIVE；SQ index 有界 |
| 5 | fwcore 驗 descriptor；adapter DMA payload | len/overflow/op 合法；每 chunk 前重新驗 token |
| 6 | device 發 CQ → vector0 | phase 最後；CQ slot 不重複；任何 stale callback 不可寫 |
| 7 | VFIO eventfd 喚醒 tenant，drain CQ | eventfd 是 IRQ 計數，不等於 completion 數；CQ 才是真相 |

tenant 對自己的 buffer 惡意改 descriptor 是允許測試的輸入；device 讀入副本後不得再次讀同一欄位形成 TOCTOU。IOMMU 不解決 tenant 自己競爭修改自身記憶體；安全目標是有界 parsing 與跨 domain 寫入被擋。

CONFIG_COMMIT 前 tenant 將全 CQ 清零；同 epoch 的 RUNNING queue 不接受 commit。BASE＋N×32 要以 checked arithmetic 驗證沒有 u64 overflow；SQ/CQ memory 區段不得互相重疊。裝置不要求 payload 一定不同於 ring（IOMMU 只管理範圍），但測試 tenant 將 payload 與 ring 分離；惡意自我覆寫仍須保持 device parser 有界。初次或 reset 後 rearm 前，tenant 透過 VFIO 的 PCI config region read-modify-write VF Command 的 Memory Space/BME bits，讀回確認；這只開本 VF 的 PCI 功能，PF-only fence 仍關閉。


## 09｜Mailbox、冪等、狀態機與核心 API

PF mailbox 全域只有一個 outstanding。Core 以 session/seq 辨識請求，逐欄比較 opcode、target、arg[4]，沒有另算 fingerprint 雜湊。Cache 保存最近已接受的 request 與完整 reply，包括尚未完成的 QUIESCE/RESET；poll 推進交易後更新 cached status。同 key、同內容回 cache，不再次執行；同 key、不同內容回 SEQ_CONFLICT；較舊 seq 回 STALE_SEQ。Seq 不允許回繞，新 session 需先完成所有 owner/lease 與 VF 清理。Driver 等待中斷後仍讀 MBX_STATUS/ACK_SEQ；ACK_SEQ 不符不能完成新請求。

| Opcode | 名稱 / arg | 行為 |
| --- | --- | --- |
| 0x01 | VERSION_NEG / major,min_minor,max_minor,0 | 每 session 至少一次；選共同 minor；未知 major 回 UNSUP |
| 0x02 | GET_STATE / target VF | snapshot state、epoch low/high、HW flags |
| 0x03 | PROVISION / reserve,0,0,0 | FREE→PROVISIONING→ACTIVE；固定 reserve 預設 8；ACTIVE 但 fence=1、queue 尚無效 |
| 0x04 | QUIESCE / target VF | 要求硬體 FENCE_BEGIN 已完成；取消該 VF retained I/O；不停止其他 VF 的 worker；QUIESCING→RESETTING |
| 0x05 | RESET / target VF | 清 queue config/indices/vector pending，保留 quota 與新 epoch；RESETTING→RECOVERING |
| 0x06 | REARM / epoch_lo,epoch_hi,ready_gen,0 | 驗 CONFIG_COMMIT 的 session/epoch；初次 ACTIVE 與 reset RECOVERING 都需此步；成功才清 fence |
| 0x07 | DESTROY / target VF | driver 先完成 owner/lease 清理與 PCI disable；core 檢查 VF 不存在、config/outstanding 清除後釋放 quota；DESTROYING→FREE |
| 0x08 | SET_RESERVE / reserve,0,0,0 | 只在有 fence 且無 outstanding 的 ACTIVE/RECOVERING 修改；Σreserve≤64 |
| 0x09 | GLOBAL_RESET / global | 經 mailbox 執行需 FW 可回應；另有 PF_CONTROL 硬體路徑可繞過 mailbox hang。兩者皆要求 VF 已移除；lease 由 driver 檢查 |
| 0x20..0x24 | UPDATE_STAGE/VALIDATE/ACTIVATE/CONFIRM/ROLLBACK | 請求通過 session／seq 檢查後回 UNSUP；保留號碼但不提供更新 |

fw_status 固定：OK=0、INVAL=1、NOSPC=2、STATE=3、FENCED=4、DMA=5、TIMEOUT=6、UNSUP=7、NO_NEG=8、SEQ_CONFLICT=9、STALE_SEQ=10、BUSY=11、OVERFLOW=12、BAD_DESC=13、BAD_QUEUE=14、IO=15。Linux adapter 明列 errno mapping，例如 NOSPC→ENOSPC、STATE/BUSY→EBUSY、TIMEOUT→ETIMEDOUT；不得把正值 fw_status 直接回 kernel syscall。

### VF 生命週期的 authoritative FSM：20 條合法事件轉移

| From | Event → To | 條件 / 副作用 |
| --- | --- | --- |
| FREE | CREATE → PROVISIONING | fenced；先保留 ledger，固定 VF slot 不另 malloc |
| PROVISIONING | COMMIT → ACTIVE | quota/device verify；queue invalid，gate 仍關 |
| PROVISIONING | ABORT_OK → FREE | device disabled、ledger 釋放完全 |
| PROVISIONING | ABORT_FAIL → FAILED | 保留可能仍持有的 quota，不提前重用 |
| ACTIVE | FENCE → QUIESCING | 硬體先 fence+bump；driver 觀察時 FW 可能尚未服務 |
| QUIESCING | DRAIN_OK → RESETTING | outstanding=0 |
| QUIESCING | TIMEOUT → FAILED | KILL，quota 保留 |
| RESETTING | RESET_DONE → RECOVERING | qcfg invalid、CQ/SQ indices 清零 |
| RESETTING | TIMEOUT → FAILED | KILL |
| RECOVERING | READY_REARM → ACTIVE | new config/session/epoch；原子開 gate |
| RECOVERING | VERIFY_FAIL → FAILED | fence/kill |
| ACTIVE | FLR → RECOVERING | hardware callback 完成 fence+bump+queue clear；quota 保留 |
| QUIESCING | FLR → RECOVERING | PCI reset supersedes FW reset work；舊 callback 取消 |
| RESETTING | FLR → RECOVERING | 同上 |
| RECOVERING | FLR → RECOVERING | 每次實際 FLR callback 都增加世代；不使用 FENCE_TOKEN 去重 |
| ACTIVE | DESTROY → DESTROYING | tenant 已退出且 VF driver unbound，先 fence+bump |
| FAILED | DESTROY → DESTROYING | killed/fenced；不能靠此解除 kill |
| RECOVERING | DESTROY → DESTROYING | 無 lease、無 outstanding |
| DESTROYING | RELEASE_OK → FREE | quota=0、qcfg invalid、無 callback；若 killed 仍不可重新 provision |
| DESTROYING | RELEASE_FAIL → FAILED | 保留 quota，等待全域復原 |

所有其他 state/event 回 STATE，完整 context 不改變，硬體緊急 fence/kill 另層測試。GLOBAL_RESET 是全域復原 transaction，不硬塞成每 VF FAILED→FREE 一條邊：先所有 owner 關閉、fence_all、cancel/drain 全部 callbacks、clear PCI queue state、session++、每個 epoch++、ledger=0、重設 fwcore slots FREE；最後再清 kill，保持 fence=1。這個全域函式有獨立 precondition 與測試，不是任意直接寫 state 的後門。

主路徑示意；完整 20 條事件轉移以表格為準。FLR 走 RECOVERING；GLOBAL_RESET 是獨立全域 transaction，必須先撤銷全部 lease。

### C core API 契約

```text
/* 跨平台整數型別由 fw_port.h 提供；內部固定陣列，不依賴 OS。 */
fw_status fw_init(struct fw_context *ctx, const struct fw_hal *hal);
fw_status fw_handle_mailbox(struct fw_context *ctx,
                            const struct fw_request *req,
                            struct fw_reply *reply);
fw_status fw_vf_event(struct fw_context *ctx, fw_u16 vf,
                      enum fw_event event);
fw_status fw_schedule_round(struct fw_context *ctx,
                            const fw_u32 backlog[FW_VFS],
                            fw_u32 service[FW_VFS]);
fw_status fw_snapshot(const struct fw_context *ctx, struct fw_snapshot *out);
fw_status fw_global_recover(struct fw_context *ctx,
                            fw_u64 new_session,
                            const fw_u64 epochs[FW_VFS]);
```

HAL 使用 now_tick/log/read_hw/cancel_io/set_gate 等 adapter callback；core 不直接 DMA、不直接操控 QEMU object。hardware epoch/fence 擁有者是 adapter，core mirror 只讀 hardware snapshot，防止 FW hang 時擋住 FENCE_BEGIN。公開函式回 status，沒有未說明的 ownership transfer；禁止全域可變狀態、malloc family、VLA、recursion、unchecked arithmetic。


## 本版不提供 capsule 更新

原始 M5 capsule／journal 設計保留於 [歷史設計](archive/M5_REGMAP_DESIGN.md)，不屬於本版實作。
保留 ABI opcode 與 aperture 值以明確拒絕不支援操作；feature bit 3 維持 0。
