# Linux kernel 導讀：從 PCI probe 到最後一次 close

本章從 Linux driver 的角度閱讀專案。先掌握裝置怎麼綁定、誰持有資源、在哪種 context 執行，
再進入 DMA 與 reset。搭配 [系統圖](SYSTEM_ANALYSIS_ZH.md) 和 [逐函式追蹤](TRACE_CODE_ZH.md) 閱讀。

## 1. 先把三個執行環境分清楚

Windows 管理實體筆電；WSL 執行編譯器與 QEMU；QEMU 裡的 guest Linux 載入 `toy_pf.ko`。
因此，WSL 的 `uname -r` 不是 guest kernel 版本，WSL 的 `lspci` 也不會列出 QEMU 裡的 toy PF。
要查 guest，應讀該次 VM 的 `kernel.log`、`serial.log` 與 `command.json`。

這個專案涉及兩種不同的「核心」：

| 名稱 | 執行位置 | 職責 |
|---|---|---|
| Linux kernel | QEMU guest | PCI enumeration、driver binding、VFIO、IOMMU、中斷、程序與記憶體管理 |
| `fwcore` | WSL 中的 QEMU process；也可原生單獨測試 | 裝置的 FSM、mailbox、quota、scheduler；是 portable C 程式庫 |

PF driver 透過 MMIO/mailbox 操作裝置，不會直接呼叫 QEMU process 裡的 `fw_transition()`。
這條邊界讓 driver 面對的是裝置協定，而不是一個可以任意改寫的 C struct。

## 2. 一次開機如何走到 probe

讀碼順序：

1. `tools/run_vm.py` 組成 QEMU 命令，指定 guest kernel、initramfs、Intel vIOMMU 和 toy PF。
2. Guest Linux 啟動後執行 initramfs 的 `/init`，也就是 repository 裡的 `guest/init`。
3. init 掛載 proc/sys/devtmpfs，確認 PF 存在，再載入 `/lib/modules/toy_pf.ko`。
4. `module_pci_driver(toy_driver)` 註冊 PCI driver。`toy_ids` 指定 vendor/device，PCI core 將符合且未被占用的裝置交給 probe。
5. [`guest/pf/toy_pf.c:toy_probe()`](../guest/pf/toy_pf.c#L622) 建立管理資源；成功後註冊 misc device。
6. init 由 `/sys/class/misc/toy_pf0/dev` 建立 `/dev/toy_pf0`，管理程式才有可開啟的入口。

Driver 註冊和個別裝置 probe 是不同事件。模組載入成功，不代表指定裝置一定綁定成功；
T02 會另外檢查失敗 probe 沒有留下 control node，乾淨重試則確實建立 control node。
PCI driver 的 callback 結構與 probe 語意可對照 [Linux 6.12 PCI 文件](https://docs.kernel.org/6.12/PCI/pci.html)。

在 WSL terminal 切到 repository 根目錄後，用以下命令定位，不必先記行號：

```sh
grep -nE 'module_pci_driver|toy_ids|toy_driver|toy_probe' guest/pf/toy_pf.c
grep -nE 'insmod|make_node|labd|m1-spike' guest/init
```

閱讀後應能說出：誰呼叫 probe、成功時公開什麼入口、失敗時誰負責回收。

## 3. probe 的順序是一份資源帳本

`toy_probe()` 的主要步驟如下。讀每個取得動作時，立即往下找它對應的釋放動作。

| 步驟 | 實際程式 | 取得什麼／檢查什麼 |
|---|---|---|
| 1 | `kzalloc`、mutex/completion/work/kref 初始化 | PF context 與同步物件，尚未接觸裝置 |
| 2 | `pci_enable_device_mem` | 啟用 PCI memory resources |
| 3 | `pci_request_regions` | 宣告本 driver 使用 BAR resources |
| 4 | 檢查 BAR0 長度與類型，`pci_iomap` | 取得可用 MMIO helper 存取的映射 |
| 5 | 讀 magic、ABI、VF/queue/vector/credit caps | 確認 driver 與裝置協定相容 |
| 6 | `pci_alloc_irq_vectors`、兩次 `request_irq` | 配置兩個 MSI-X vectors 並註冊 handler |
| 7 | `pci_set_master`、VERSION_NEG | 啟用 bus mastering 並完成協定協商 |
| 8 | `misc_register`、debugfs | 公開管理入口與診斷節點 |

例如第二次 `request_irq` 失敗時，第一個 IRQ 已經取得，必須釋放；尚未取得的第二個則不能釋放。
程式以 `irq_count` 記錄成功取得的數量，沿 `free_irqs` → `unmap` → `release_regions` →
`disable_device` → `free_context` 反向回收。

這裡採用顯式 goto unwind，方便把每個 fault point 和資源配對。改用 devm 仍然需要處理裝置停止、
DMA fence 與 worker 同步；記憶體自動釋放不會替代這些協定。

## 4. open、ioctl、read、poll 各做什麼

`guest/pf/toy_control.inc` 是 `toy_pf.c` 納入的實作片段。先找檔案尾端的 `toy_fops`：

| Userspace 操作 | Driver 入口 | 主要用途 |
|---|---|---|
| `open("/dev/toy_pf0", ...)` | `toy_open` | 建立 per-open context，加入 files list，取得 PF reference |
| `ioctl(fd, TOY_GET_INFO, ...)` | `toy_ioctl` | 驗證 UAPI header，讀取一致的裝置 snapshot |
| lease/fence/reset/rearm ioctl | `toy_control_ioctl` → `toy_execute_ctl` | 檢查 owner 與世代，再執行管理交易 |
| `read(fd, ...)` | `toy_read_file` | 取出 driver event；無資料時依 blocking 模式等待或回 EAGAIN |
| `poll(fd, ...)` | `toy_poll` | 將程序掛入 wait queue，回報可讀或裝置移除 |
| 最後一次 fd reference 釋放 | `toy_release_file` | 回收 per-open context，降低 PF reference count |

一次 `open` 建立一個 open file description。`dup` 後的兩個 fd 指向同一個 `struct file`，
不能把它們當成兩個互不相關的 owner。專案用 per-open context 綁 lease，也用它保存回覆 cache 和事件佇列。

閱讀 [`guest/pf/toy_control.inc:toy_ioctl()`](../guest/pf/toy_control.inc#L390) 時，注意四個防線：

1. `dying` 表示已不能再操作 BAR，回 ENODEV。
2. `copy_from_user` 先複製到 kernel local struct，再驗 size、ABI、reserved 和 VF 範圍。
3. 可修改狀態的命令再驗 session、epoch、lease token 和 operation。
4. 裝置成功回覆後，仍讀回 fence/config/state 等後置條件。

`copy_to_user` 失敗不會撤銷已經做完的 reset。控制交易保存原結果，讓相同 operation 的重試取回結果。
這是協定語意，不是單靠 system call 的正負回傳值就能解決。

## 5. 三種執行 context，三種限制

| 路徑 | Context | 本專案的處理 |
|---|---|---|
| probe、ioctl、read | Process context | 可以在適當條件下睡眠；用 mutex 序列化管理操作 |
| `toy_irq` | IRQ handler | 讀 cause、ACK、complete、排 work；不取得管理 mutex |
| `toy_notify_work` | Workqueue | 可以睡眠；取得管理鎖後讀取一致快照，向各 open context 發布事件 |

沿 [`guest/pf/toy_pf.c:toy_irq()`](../guest/pf/toy_pf.c#L296) 看 mailbox completion：
handler 呼叫 `complete(&pf->mbx_done)` 喚醒等待者；等待者仍需核對 DONE 和 ACK_SEQ。
另一個 IRQ 先到、IRQ 被合併或回覆已過期，都不能靠「被喚醒」三個字判定交易成功。

鎖順序是 `admin_mutex` → `mbx_mutex`。Mailbox 等待期間，完成它的 IRQ handler 不取得這兩把鎖，
否則會出現等待者持鎖、完成者也等同一把鎖的循環。其他事件交給 workqueue，不在 IRQ context 做完整管理交易。

`event_lock` 則是短暫保護 per-open event ring 的 spinlock。
`toy_read_file()` 在鎖內把資料移到 stash，放鎖後才 `copy_to_user`；copy 失敗保留 stash，
避免 userspace 重試時事件已經遺失。完整鎖與引用表見 [Trace code](TRACE_CODE_ZH.md)。

## 6. BAR 位址、CPU pointer、IOVA 不可混用

| 位址 | 誰使用 | 例子 |
|---|---|---|
| Kernel MMIO mapping | PF driver | `pf->bar` 經 `readl/writel` 存取 PF registers |
| Guest userspace VA | Tenant CPU | SQ/CQ/payload allocation 的 C pointer |
| IOVA | 裝置發起 DMA | descriptor 裡的 payload 位址 |
| Guest physical address | Guest IOMMU 轉譯結果 | 被 VFIO pin/map 的 guest pages |
| Host virtual address | QEMU process | 模擬 guest RAM 的 host memory；不交給 tenant 當作 IOVA |

本專案 PF 負責管理，資料 DMA mappings 由 tenant 透過 VFIO TYPE1 建立。
因此不要在 PF driver 補一個 `dma_map_single`，再以為 tenant 的 IOVA 已經可用。
那是不同的 ownership 與 address space。

Classic VFIO 的 container/group/device 關係、group viability 與 DMA mapping 介面，
可對照 [Linux 6.12 VFIO 文件](https://docs.kernel.org/6.12/driver-api/vfio.html)。
本專案進一步用 T04/T05 驗證實際 DMA requester 與兩個 domain 的資料，group 名稱只是其中一個觀測。

## 7. 物件還活著，不代表硬體還能用

`kref` 保護 `toy_pf` 記憶體的生命週期，`dying` 則阻止移除後的 MMIO。
兩者缺一不可：只有 reference count，舊 fd 可能讀取已解除的 BAR；
只有 dying flag，舊 fd 仍可能持有已被釋放的 context pointer。

`toy_open()` 在管理鎖內確認可接受新操作，再取得 reference。
`toy_remove()` 關閉入口、fence、同步 worker/IRQ、釋放 PCI resources，最後放掉 driver 持有的 reference；
既有 file context 仍需完成自己的 release。Reference 的取得與釋放原則見
[Linux 6.12 kref 文件](https://docs.kernel.org/6.12/core-api/kref.html)。

讀碼能解釋預期生命週期，但本版驗收只涵蓋已列出的 T01–T10，未涵蓋所有 hostile hot-remove 交錯。
應把「設計如何處理」和「實際測到哪些路徑」分開回答。

## 8. 四個可自行完成的讀碼練習

所有命令都在 WSL repository 根目錄執行。

```sh
grep -nE 'CONFIG_(PCI_IOV|VFIO|INTEL_IOMMU|KASAN|PROVE_LOCKING)' build/kernel-kasan/.config
grep -nE 'toy_(probe|irq|remove)|request_irq|cancel_work_sync|kref_' guest/pf/toy_pf.c
grep -nE 'toy_fops|toy_open|toy_ioctl|toy_read_file|toy_poll' guest/pf/toy_control.inc
grep -nE 'VFIO_(GROUP|SET_IOMMU|IOMMU_MAP_DMA|IOMMU_UNMAP_DMA|DEVICE_SET_IRQS)' guest/tenant/vfio.c
```

第一輪只畫出呼叫關係；第二輪在每個箭頭旁補上 context 和位址種類；
第三輪從任一失敗點往回找 cleanup。最後用以下問題檢查理解：

1. probe 第七步失敗時，為何不能直接 kfree？
2. mailbox 完成時，哪個 context 會喚醒誰？為何還要核對 sequence？
3. driver 的 copy_to_user 失敗時，reset 是否可能已完成？
4. owner 關閉 VFIO fd 和 PF 放掉 lease，為何需要明確順序？

答案不必背 API 名稱，應能指出取得的資源、仍可能執行的工作，以及允許釋放它們的條件。

## 9. 練習題解答與下一條 trace 路徑

第一題先從 probe 的 `clear_master` label 往下讀。此時可能已經有 BAR mapping、兩個 IRQ 和 work，
所以要先讓裝置停止接受管理工作、同步相關執行路徑，再解除映射與釋放 context。
直接 kfree 會讓仍可能被 IRQ/work 使用的 pointer 失效。

第二題的等待者是發 mailbox 的 process context，喚醒者是 `toy_irq()`。
完成通知表示狀態可能有進度；DONE/ACK_SEQ 才把回覆綁到這一次 request。
這可對照正常 I/O：eventfd 同樣只是通知，CQ phase/id/epoch 才描述完成資料。

第三題的 reset 可能已經完成。若只因回傳 EFAULT 就建立新 operation 重送，可能再次產生副作用。
應理解既有 operation 的重試契約，再沿 `toy_control_ioctl()` 看完整 request 比對和 cached result。

第四題先畫 ownership：VFIO fd 持有 device、IRQ 與 mappings；PF lease 控制管理權限。
只有實際完成 fence、drain、UNMAP、fd close 和 owner 退出，管理端才能釋放 lease 或允許下一個 owner。
程式必須保留失敗時尚未確認釋放的資源，不能只讓計數看起來歸零。

完成這四題後，依序追下面三條路徑：

| 路徑 | 起點 | 終點 | 應記下的欄位 |
|---|---|---|---|
| 管理查詢 | labd 的 ioctl | PF SNAPSHOT 與 UAPI 回覆 | session、epoch、state、reserve、flags |
| 正常資料 | owner_workload | CQ 檢查與 CQ_HEAD 更新 | SQ/CQ cursor、IOVA、requester、phase、id |
| Reset | labd recovery | 新 owner configuration 與 REARM | lease、operation、epoch、ready_gen、cancel token |

每次只追一筆請求，將成功路徑走完，再回頭選一個 return/goto 追失敗回收。
詳細函式順序與實際紀錄在 [Trace code](TRACE_CODE_ZH.md)。
