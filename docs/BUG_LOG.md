# 實際失敗與修正紀錄

這份紀錄區分程式缺陷、來源版本的行為差異與測試工具問題。
歷史 PASS 只對應該次輸入；最新驗收以 [VALIDATION.md](VALIDATION.md) 的 checkpoint 為準。
測試範圍為 M0–M3／T01–T10；每筆紀錄區分症狀、根因、修正和回歸證據。

## B001：四個 owner 串行關閉造成 READY deadline 到期

狀態：**修正後 100 輪 PASS，完整受影響回歸 PASS**。

- 重現：`python3 tools/run_vm.py --gate m3-case --profile kasan --case T01 --timeout 2400`。
- 失敗紀錄：`artifacts/m3-case/20261012T165957Z-v6lqkeoi/`。
- 受影響回歸：`artifacts/m3-checkpoint/20261012T165003Z-0lbowior/`，T01 FAIL，後續案例 NOT_RUN。
- 症狀：第 16 個 measured cycle 的 owner 都已正確 UNMAP、close、reap、release，
  但 disable 後 VF 0 的旗標是 FENCED|KILLED，測試要求的乾淨基線不成立。
- 根因：`guest/admin/labd.c:lifecycle_case()` 的註解描述一起關閉，實作卻對每個 VF
  依序執行 `owner_stop()` 並等待退出。第一個 close 觸發真實 PCI FLR，進入 RECOVERING；
  等待其餘 VF 的清理和 socket 清除期間可能超過 1000 個 virtual ticks。
  `fwcore/context.c:fw_poll_deadlines()` 因 READY 逾時轉 FAILED，KILL 必須保留到 global recovery。
- 修正：將 stop 拆成 begin/finish。先對每個 owner fence 並送停止通知，再確認各 owner
  的 EXIT_ACK、pidfd/waitpid 退出與 lease release。最後一個 lease 關閉後立即 disable PCI VFs，
  再清除 socket、目錄與管理 fd。
- 保持的條件：不延長 core 的 READY deadline，不忽略 KILL，不直接把 FAILED 改 FREE，
  不省略 UNMAP 或 owner 退出確認。
- 修正後：`artifacts/m3-case/20261012T170334Z-brrcjgt_/` 暖機加 100 輪 PASS，
  每輪實際資源回到基線。semantic validator 另要求全部 fence 先於第一個 pidfd reap。
  完整受影響回歸 `artifacts/m3-checkpoint/20261013T005556Z-56ebu4kg/` PASS，
  其中新的 100 輪為 `artifacts/t01/20261013T010558Z-rz3hvxnp/`。
  這證明記錄範圍內的回歸通過，不是永不再發生的保證。

## B002：要求 WRITE-only 不代表 pinned Linux 會禁止讀取

分類：**平台行為差異，並非 toy DMA helper 漏檢查**。

- 重現：原生 pinned guest kernel 上執行 T05 variant 4。
- 原始失敗：`artifacts/m3-case/20261012T163725Z-at60ieyh/`。
- 症狀：VFIO map 要求 WRITE-only，但 VF RID 0101 的 CRC DMA 成功讀取八個 bytes，
  發出 OK completion，沒有 DMA fault。該次結果必須是 FAIL。
- 根因：Linux v6.12 Intel IOMMU 的 `__domain_mapping()` 自動加入
  `DMA_FL_PTE_PRESENT`，此 bit 在 second-level page table 也是 READ bit。
  只列 VFIO requested flags，無法證明有效頁表權限。
- 實驗處理：新增 SHA256-pinned、預設關閉的 guest patch；只有明確指定
  `toy_iommu_strict_wo=1`，且 domain 僅有一個 1234:e002 toy VF 時，保留獨立 R/W 權限。
  其餘 domain、first-level 與未啟用選項的行為維持上游。
- 實際開啟選項後，variant 4 開發測試 PASS：
  `artifacts/m3-case/20261012T164801Z-pgphhfe3/`。
- 驗證內容：讀取真的被 IOMMU 擋下、實際 requester 與 fault counter、A FAILED/fenced、
  B guard 不變、B 完成 4098 筆請求、owner 清理與 global recovery。
- 限制：不能將此結果說成 stock Linux 的 WRITE-only 安全保證。
  完整受影響回歸已通過 `artifacts/m3-checkpoint/20261013T005556Z-56ebu4kg/`。
  詳見 [D015](DECISIONS.md#d015--explicit-toy-only-strict-write-only-guest-iommu-mode)。

## B003：未映射的 CPU guard 被測試 helper 錯誤拒絕

狀態：開發測試修正後 PASS，完整受影響回歸 PASS：`artifacts/m3-checkpoint/20261013T005556Z-56ebu4kg/`。

- 失敗：`artifacts/m3-case/20261012T163400Z-nq5t6c5j/`。
- 症狀：T05 variant 1 在送 DMA 前以 EINVAL 結束。
- 根因：`owner_freeze()` 同時要求 CPU buffer 存在、VFIO mapping 成立。
  但「A 未 map、B map 相同 IOVA」正需要保留未映射的 A CPU guard。
- 修正：freeze 驗證 CPU allocation 並保存整個 buffer；mapping 有無由獨立 owner map 狀態管理。
  沒有替 A 補映射，也沒有繞過 IOMMU。
- 修正後：`artifacts/m3-case/20261012T164705Z-8qcbj5we/` PASS。

## B004：把 QEMU 裝置初次 realize 的 reset 誤當 PCI FLR

狀態：已在既有 checkpoint 回歸通過。

- 失敗：`artifacts/m1/20261011T041635Z-kzwhoi8v/`。
- 另一份早期 T03 `artifacts/m3-case/20261011T041729Z-0pr3pe82/` 雖被當時 runner 標 PASS，
  raw log 含未使用 VF 的 FAILED transition，因此已明確撤回驗收，不刪除原紀錄。
- 根因：QEMU hotplug realize 期間會先呼叫 reset；當時 `dev->realized` 尚未設 true。
  舊 hook 一律當 FLR，替尚無 tenant 的 VF 開始 READY deadline。
- 修正：初次 power-on 仍 fence、advance generation、clear queue，但不發 firmware FLR event。
  已 realized 的 VF reset 仍走真實 `toy_vf_flr()`。
- Trace：`device/toy_vf.c` 的 reset hook → `device/toy_hw.c:toy_vf_flr()` →
  `fwcore/fsm.c:fw_vf_event()`。raw log 分別使用 TOY_VF_POWER_ON／TOY_FLR。
- 回歸：`artifacts/m3-checkpoint/20261012T111926Z-eajy2snh/` 包含 M1、M2 與當時已實作的 M3 案例。

## B005：kernel printk 插入 JSON serial 記錄

狀態：已在既有 checkpoint 回歸通過。

- 失敗：`artifacts/m3-case/20261011T132643Z-n6tck8bh/`。
- 症狀：driver 的九個 failpoint 都有命中，但三行 JSON 被 printk 切斷，semantic validator 拒絕。
- 根因：kernel console 與 userspace 結構化事件共用同一 UART；完整的一次 printf
  不能保證與另一層的 console 輸出互斥。
- 修正：M3 的 UART0 保存 kernel.log，UART1 保存 serial.log。兩者都保存 hash 並檢查
  KASAN、lockdep、panic 等非預期診斷；沒有放寬 failpoint 命中條件。
- Trace：`tools/run_vm.py` → `guest/init` 的 console 設定 → `tools/vm_runtime.py` 收集 → semantic validator。
- 決策：[D013](DECISIONS.md#d013--keep-kernel-diagnostics-separate-from-m3-structured-records)。
- 後續：當時只修正 M3；M1／UEFI handoff 仍共用 UART，後來再次重現，見 B020。

## B006：guest 沒有選配的 proc children 介面

狀態：已在 T01 既有 checkpoint 回歸通過；與 B001 為不同問題。

- 失敗：`artifacts/m3-case/20261012T063358Z-5gdnrooj/`；診斷 run：
  `artifacts/m3-case/20261012T063625Z-dzt_o1b9/`。
- 根因：guest 未啟用 CONFIG_PROC_CHILDREN，讀取 `/proc/self/task/PID/children` 回 ENOENT。
- 修正：每個已知 child 仍使用 pidfd/waitpid 確認退出；最後要求
  `waitpid(-1, WNOHANG)` 回 -1 且 errno=ECHILD，確認既無存活 child，也無待 reap child。
- 為何不接受空檔／忽略 ENOENT：介面不存在無法證明沒有 child。
- 回歸：`artifacts/t01/20261012T112848Z-nzlrkpjz/`。

## B007：native decoder replay 遺漏 core 的連結單元

狀態：修正後 ASan/UBSan replay PASS。

- 失敗：`artifacts/t09-native/20261012T111906Z-dnia4alr/`，未解析 `fw_zero`。
- 根因：harness 只編入 codec.c；該檔使用同一 portable core 提供的 helper。
- 修正：連結完整 portable core，保留 sanitizer 與 Werror，不自行複製 helper 實作。
- 通過：`artifacts/t09-native/20261012T111929Z-itwqts6s/`。
- 驗證意義：以 guest 真正送出的六組 descriptor bytes 重播 bounded parser，
  不以手工重造的近似向量替代原始輸入。

## B008：重設後相同 IOVA 的新映射被 validator 認成舊事件

狀態：validator 修正後，新 VM 實跑 PASS：`artifacts/m3-case/20261012T171149Z-ctnvo3xr/`。

- 失敗：`artifacts/m3-case/20261012T171104Z-il8wl8_q/`。
- 症狀：guest 完成 ioctl reset、fresh CONFIG/REARM、close/open 與清理，
  host validator 卻拒絕「old UNMAP → new MAP」順序。
- 根因：兩代 mapping 的 pid、kind、IOVA、size、flags 可以完全相同。
  Python `list.index(dict)` 按欄位值相等尋找第一個事件，因而把第二代 MAP 指回第一代。
- 修正：解析完成後以各事件物件的 identity 建立其原始 log 位置索引。
  順序判斷使用保存的位置，不依欄位值是否相同。
- 證據原則：原始 FAIL 不改成 PASS；修正後另開新 VM 重跑。這不是裝置成功字串足夠的理由，
  其餘 raw requester、FLR、guard、epoch、mapping、healthy progress 斷言仍全部必要。

## B009：反向 apply 成功無法證明工作樹只有指定 patch

分類：建置來源驗證缺口；以獨立 Git 測試 fixture 重現，不是發現本機 kernel 曾混入額外修改。

- 原檢查：HEAD 與允許的 dirty filenames、patch hash、`git apply --reverse --check`。
- 缺口：在同一個允許的來源檔、遠離 patch context 的位置新增一行，reverse check 仍可能成功。
  因此「包含這個 patch」不能推出「只有這個 patch」。
- 修正：每次核對 HEAD，使用獨立 temporary Git index 從 pinned HEAD 依序套用 locked patches，
  取得預期 blobs，再逐 byte 比對工作檔案。正常 index、branch 和工作樹不被重設。
- 三項真 Git 測試：正確 patch 通過且正常 index 不變、額外同檔修改遭拒、不同 HEAD 遭拒。
  紀錄：`artifacts/source-verification-tests/20261013T073408Z-6vofnbff/` PASS。
- 目前實際 kernel 工作樹的精確唯讀稽核：
  `artifacts/source-exact-audit/20261013T011419Z-t0zcg9no/` PASS。
- Helper hash 是 kernel stamp 的一部分，修正後會重建／重新驗證 kernels 和受影響前提，
  不直接把舊 stamp 改成新 hash。

## B010：把 callback 執行後的 valid 誤當成執行前的有效性

狀態：修正診斷後以新 VM 通過 T08；完整 M3 回歸也已通過。

- 原始失敗：`artifacts/m3-case/20261013T073556Z-bcnekcar/`。
- 現象：STALE_CPL 實際增加兩次，卻出現 `try-stale-cpl valid=0`，host 拒絕缺少有效 retained callback 的證據。
- 根因：fault setter 複製真實 retired operation，呼叫 `toy_complete()` 後才讀 `copy.valid`。
  completion 路徑消耗 operation 時會把它設 false。這個欄位描述的是不同時點，不能用同一名稱混讀。
- 修正：呼叫前保存 `was_valid`，ACK 同時輸出 `valid=1` 與執行後的 `consumed=1`。
  Validator 同時要求兩者、真實 callback 的 old/live token、STALE_CPL、buffer 與 IRQ guards。
  沒有只放寬 regex 來接受無效 operation。
- 新 VM：`artifacts/m3-case/20261013T074144Z-r54944se/` PASS。
  T07 同版 QEMU 重跑：`artifacts/m3-case/20261013T074221Z-79mqmb0v/` PASS。
- 保存這兩次 raw events 的 fixture 後，42 項 controller/semantic 測試通過：
  `artifacts/m3-validator-tests/20261013T074457Z-3r9sxbee/`。
- 面試重點：觀測程式本身也會出錯。先確認資料的時點與語意，再判斷裝置行為；保留原始 FAIL。

## B011：Linux EFI stub 遇到新 compiler 的 C23 關鍵字

分類：已重現的工具鏈相容性問題；GCC 14 建置及 guest boot 已通過，GCC 15 原組合仍是 FAIL。

- 原始錯誤：`artifacts/build-kernel/20261007T154311Z-w2gl3ipa/compile.log`，
  `cannot use keyword 'false' as enumeration constant`、`'bool' cannot be defined via 'typedef'`。
- 原因：Linux v6.12 的 EFI libstub Makefile 替換一般 kernel CFLAGS，沒有保留明確 C dialect。
  Ubuntu GCC 15 的 C23 預設與舊 kernel 的 false enum、bool typedef 衝突。
- 修正方法：builder 明確使用已釘選的 GCC 14.3 `CC` 與 `HOSTCC`，同樣用於外部 PF module。
  不切換整台主機的 alternatives，不刪 EFI stub，也不掩蓋錯誤。
- 從哪裡追：`tools/build_kernel.py` 的 compiler argv → pinned
  `drivers/firmware/efi/libstub/Makefile` → 出錯的 `include/linux/stddef.h`／`types.h`。
- 修正後早期證據：`artifacts/build-kernel/20261008T005830Z-j07gwf5d/`；
  新版 KASAN/stress 的實際 runtime 另隨 M3 checkpoint 保存。
- 新手判斷：不要只看最後 `make Error 2`，向前找第一個 compiler error 與那一個 compile argv。

## B012：NASM 3 不再接受 EDK2 舊 X64 PUSH operand 寫法

分類：已重現的 assembler 相容性問題；已 backport upstream 修正並通過 M2。

- 原始錯誤：`artifacts/build-edk2/20261009T022324Z-lat9fnjj/positive.log`，
  `instruction not valid with 32-bit operand size`。
- 原因：ExceptionHandlerAsm.nasm 在 64-bit 模式使用 `push strict dword`。
- 修正：將 upstream commit `9ccf8751a74f26142e584c7b7c7572a182b67997` 的兩個 operand 修正
  backport 至 `uefi/patches/0001-nasm3-x64-push.patch`，改為 `strict qword`。
  patch 與目標內容都有 hash，僅套用 build worktree。
- 另一個實際陷阱：該 upstream 檔本身含 CRLF；初版 LF patch context 無法套用，
  `artifacts/build-edk2/20261009T022547Z-ein4_cny/` 保留失敗。最後保留 upstream bytes，
  只對該 patch 使用窄範圍 Git attribute，不修改全機 line-ending 設定。
- 詳細來源與 upstream 連結：[D009](DECISIONS.md#d009--backport-the-upstream-nasm-3-x64-push-fix)。

## B013：SR-IOV reset 覆蓋下一個 ACS capability header

分類：已重現的 register layout 問題；調整 toy layout 後 M1/M2 通過。

- 現象：UEFI 成功走過 ARI=0x100、SR-IOV=0x140，但 next pointer 指向 0x180 時讀到零。
  失敗：`artifacts/m2-probe/20261009T130032Z-zktgskjg/`。
- 根因：pinned QEMU 的 `pcie_sriov_pf_reset()` 依 `PCI_NUM_REGIONS` 迴圈，
  對 4-byte BAR offset 進行 8-byte 寫入，末筆涵蓋 0x17c..0x183，破壞放在 0x180 的 ACS。
- 修正：把 toy PF 的 ACS 移至 0x200，沿用 upstream capability helper 管理 header 與 next。
  M1 額外檢查 PF/VF 的 ACS/ARI，不能只讓 UEFI 忽略零 header。
- Trace：`device/toy_pf.c` 的 realize/capability 安排 → pinned
  `hw/pci/pcie_sriov.c` 的 reset 寫入範圍 → UEFI extended-capability walk。
- 面試重點：capability 初次建立正確還不夠；reset 路徑也會改寫同一片 config space。

## B014：UEFI PCI I/O protocol index 不等於實體 BAR 編號

分類：已重現的 API 語意誤解；probe 修正後三條 M2 boot path 通過。

- 失敗：`artifacts/m2-probe/20261010T023836Z-_uoy5wg0/`。
  BAR0 檢查通過，但以 physical BAR2 的數字呼叫 `GetBarAttributes(2)` 得 EFI_UNSUPPORTED。
- 根因：pinned EDK2 `GatherDeviceInfo()` 每呼叫一次 `PciParseBar()` 才增加 protocol 陣列 index；
  64-bit BAR 本身跨兩個 config DWORD。因此本裝置 physical BAR0/BAR2 對應 protocol 0/1。
- 修正：probe 先按 config BAR type 走實際佈局，推得 protocol index，再比對回傳的 type、length、base。
  不拿另一個 index 回 Success 就算通過，也不對使用中的 BAR 寫 all-ones 來 sizing。
- 對照：[D012](DECISIONS.md#d012--resolve-edk2-protocol-bar-indices-from-physical-bar-layout)。
  面試時說明是「這個 pinned implementation 的映射」，不要推廣成所有 UEFI 都有同樣細節。

## B015：EDK2 沒改來源卻幾乎整套重編

分類：已重現的可重現建置／增量效率問題；不是 firmware runtime defect。

- 重編記錄：`artifacts/build-edk2/20261009T180208Z-llo33trc/`。
- 根因：BaseTools 的 GenC.py 用 Python `hash(Info.Guid)` 選 STACK_COOKIE_VALUE；
  每個 Python process 的 hash seed 改變，生成的 AutoGen.h 隨之改變，連帶大量 module 重編。
- 修正：僅在 builder subprocess 固定 `PYTHONHASHSEED=0`，保留 cookie table；
  unchanged Conf/DSC/toolchain files 不無故更新 timestamp。真正 input 變更仍由正常 build dependency 處理。
- 證據：`artifacts/edk2-incremental/20261010T023823Z-u953zmyh/result.json`。
  positive/negative 步驟從約 116/109 秒降至約 12/11 秒，輸出 image hash 相同。
  這是當時兩次 build 的觀察，不是所有機器的固定速度保證。

## B016：第一次使用 requester 的 cache 被誤判成持續 leak

分類：已重現的基線定義問題；加入明確 warm-up 後檢查每輪資源是否回穩。

- 失敗：`artifacts/m1/20261008T162441Z-o7m628zi/`，冷機 340 個 QOM objects 變為 360。
- 根因：QEMU Intel IOMMU 的 `vtd_find_add_as()` 為新 requester 快取五個 memory-region objects，
  四個 VF 合計增加 20；不能僅憑一次總量增加判斷是每輪漏資源。
- 修正：保存 cold inventory，先做不列入 100 次的 warm-up，再建立 measured baseline。
  之後 100 次完成循環要求 initial/final 完整 inventory 相同、device-owned pending work 為零、
  guest ownership counters 回零。不以「可能是 cache」忽略後續成長。
- T01 的完整 tenant lifecycle 比 M1 enumeration 多了 owner/fd/map/reap，兩種證據分開保留。

## B017：重建之後沿用舊 core PASS

分類：測試證據綁定缺陷；審查發現並以反例測試驗證，不是 guest runtime crash。

- 原因：舊 `verify_m0 --reuse-core` 只比較當前 build stamp 的 inputs 和當前 sources，再接受過去 core result 的 PASS。
  修改 core 並只執行 build-core 後，當前 stamp 已更新，但舊 result 沒有證明測過這組 binary。
- 修正：build stamp 保存 binary hashes；core run 保存受測 inputs、binary、stamp hash 和 evidence hashes；
  reuse 全部核對。缺乏新欄位的歷史結果須重新驗證，不能自動補標。
- 回歸：`python3 -m unittest discover -s tests/scenarios -p 'test_m0_reuse.py' -v`。
  七項檢查含舊 execution／新 build、binary 被改、stamp 被換、失敗 build、legacy result，只有完全相符時接受。
- 教訓：證據要綁到「當時受測的產物」，不是只綁到目前目錄存在的檔案。

## B018：stale SQ 之後錯用 CQ slot 核對 payload

分類：真實 tenant workload 邏輯錯誤，已保存修正前 native 失敗與修正後 sanitizer 回歸。

- 重現：old-epoch descriptor 只前進 SQ，不產生 CQ。下一批正常 workload 的 submission slot 與 completion slot 相差一筆。
  原碼以 CQ head index 找 payload，可能出現 EILSEQ 或 EBADMSG。
- 原始失敗：`artifacts/release-preparation/20261013T181909Z-69pn08bk/owner-before-fix.log`。
  該目錄以 Git HEAD 取出修正前 owner.c，編譯同一個新測試，確實取得非零退出碼；沒有改動歷史 log。
- 修正：`guest/tenant/owner.c:owner_workload()` 以 `start + received` 的原始 SQ cursor 找 payload，CQ cursor 只定位 completion。
  仍核對 completion id、epoch、status、length、CRC 和 guards。
- 回歸：`make verify-guest-native` 執行真實 owner_workload，涵蓋五個 ring sizes 的正常、SQ/CQ 差一、u32 wrap，共 15 組。
  首次修正後 ASan/UBSan PASS：`artifacts/guest-native/20261013T181637Z-1ub_ttyc/`。
  Guest 整合回歸另見 [驗證紀錄](VALIDATION.md)。
- 為何舊測試沒發現：T09 送完 stale/malformed vectors 即進 cleanup，沒有再啟動一般 workload；正常 T03 的 SQ/CQ 則同步前進。

## B019：全新環境缺少教材依賴檢查

分類：安裝清單缺項，審查發現；不冒稱在全新 OS 重現過完整安裝失敗。

- 原因：renderer import `markdown_it`，但 bootstrap 未安裝 `python3-markdown-it`；environment 也未檢查 kernel 必需的 gcc-14。
- 修正：installer 補教材套件；env-check 檢查 gcc-14、套件狀態與目前 Python 能否 import renderer。
- 驗證：本機 env-check 及 handbook generation；完整 OS 首次 apt 安裝會受發行版套件更新影響，須以實際 apt log 為準。

## B020：UEFI handoff 仍沿用單 UART，偶發遺失 capability 證據

分類：真實整合驗收失敗，修正範圍包含 M1 和 UEFI handoff；不是 PCI capability 本身消失。

- 失敗：`artifacts/m2-handoff/20261014T145421Z-4j_x1q0l/`，連帶使
  `artifacts/project/20261014T143257Z-nrtvyinf/` 的完整驗收 FAIL。
- 症狀：guest 已印出 M1_CAPS 與最後 PASS，但 host validator 只解析到四筆 function_caps，
  拒絕「PF 加四個 VF」的必要條件。`serial.log` 第 383–384 行保存 clocksource 訊息切進 VF4 bdf 字串的原始 bytes。
- 根因：B005 只將 M3 分流；M1 和 UEFI→Linux 路徑仍讓 printk 與 userspace JSON 共用 UART0。
  延遲的 TSC calibration 訊息可能在任意 printf 輸出期間出現，不能靠先前幾次剛好通過推定沒有問題。
- 修正：所有 Linux gate 都以 UART0 記錄核心、UART1 傳輸測試 JSON／checkpoint ACK。
  UEFI handoff 的 UART0 同時保存 firmware→Linux 交接順序；probe-only 仍只用 UART0。
  原始檔案分別保存；語意檢查才讀取兩個 channel，不刪行、不修補損壞 JSON、不降低 capability 數量要求。
- 核心診斷：M1 及重用前提都檢查並 hash kernel.log；M2 重解析同時核對兩份 raw log。
  分流不能用來隱藏 KASAN、warning 或 panic。
- 修正過程中的額外失敗：`artifacts/m1/20261015T162947Z-_q0ku6op/` 的第一個 checkpoint timeout。
  只移動 stdout 還不夠：`host_checkpoint()` 原本重新開啟 `/dev/console`，仍在 UART0 等 ACK，
  而 host 已回覆 UART1。改成獨立開啟 init 傳入的 stdin 裝置，保留 nonblocking/raw/noecho 與恢復流程；
  真實 VM 要驗三次 checkpoint，不能只檢查 JSON 輸出。
- 回歸：`test_uart_transport.py` 涵蓋損壞紀錄拒收、雙 channel 正常解析、核心 warning 拒收、
  缺 kernel log 拒收及 probe-only 行為；全部 Python tests 共 73 項已通過。
  真實 VM 與最終整合結果以 [VALIDATION](VALIDATION.md) 首節為準。
- 教訓：修正共用機制時，要列出每個使用它的入口；單一情境修好不代表其他 boot path 一併修好。

## 驗證界線

M4 hostile remove／crash／併發全矩陣和 M5 持久化已移出本版範圍，不改標 PASS。
以上修正和有限測試不能證明零 BUG；一般排錯策略與真實重現紀錄分開閱讀。
