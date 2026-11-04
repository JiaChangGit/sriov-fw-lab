# Requirements and verification checklist

歷史規劃：原 v3.0 規劃（已由 [現行範圍](../REQUIREMENTS.md) 取代），§20 要求完整建置 M0 → M5。本清單是施工追蹤表，非測試結果。建立時全部 **NOT_RUN**；PASS 只能由實際建置或執行證據支持，最終狀態與 raw log 路徑記於 VALIDATION.md / result manifests。BLOCKED 必須保留精確命令與錯誤；不得以 SKIP、原始碼存在、預期輸出或單一 PASS 字串冒充通過。

規格 SHA256：`0adb15cfd92f792483753914ddf9207fe6af7e7df0d662b86d3b9f39223bf3af`

## 環境、来源與可重現性

| ID | 規格 | 可驗收要求 | 狀態 |
| --- | --- | --- | --- |
| E01 | §3 | 保存 Windows/WSL version/status/list、.wslconfig 原值、Windows 實際 CPU/RAM/可用 SSD；不猜測機型能力 | NOT_RUN |
| E02 | §3 | 保存 L1 os-release/uname/lscpu/free/df/findmnt、Python/GCC/Clang、/dev/kvm；可選工具缺失 NOT_AVAILABLE 並繼續其他診斷 | NOT_RUN |
| E03 | §3 | repo realpath 為原生 Linux FS，拒絕 drvfs/9p/vboxsf/Windows 掛載建置；驗大小寫/symlink/可執行位 | NOT_RUN |
| E04 | §3 | build 前 L1 與 Windows backing disk 至少 60 GiB；建議 80–100 GiB；JOBS=2；各 heavyweight build 串行、單 guest | NOT_RUN |
| E05 | §3 | WSL 建議 8 GiB/6 processors/4 GiB swap；合併既有設定，不擅自 wsl --shutdown 或修改 host boot/security | NOT_RUN |
| E06 | §3,14 | TCG 必要；KVM 獨立 smoke 後可選；ACCEL=kvm 失敗不得 silent fallback | NOT_RUN |
| S01 | §4 | 解析四個候選 tag 的實際完整 peeled commit、來源 URL 與 recursive submodule commits；不得偽造 lock | NOT_RUN |
| S02 | §4 | apt-cache policy 分必要/optional dependencies，保存實際版本/compiler triple；Python>=3.12；repo venv/wheel hash，不 sudo pip | NOT_RUN |
| S03 | §4,21 | SOURCE_EVIDENCE 連到 pinned 檔名/函式/commit：QEMU SR-IOV/DMA/MSI-X/reset、Linux PCI/VFIO/Kconfig、EDK2 PCD/PciBusDxe/DSC/BaseTools | NOT_RUN |
| S04 | §4 | 原生 Ubuntu 26.04 GCC 最小 build/smoke 通過才 VALIDATED；替代 compiler/ref 要有 apt/source 證據、DECISIONS 與重跑 gates | NOT_RUN |
| S05 | §4,14 | lock 含 patches SHA256/submodules/tool versions；stamp 比對 lock/config/patch，不僅目錄存在；immutable upstream + 可重覆 apply patch | NOT_RUN |
| S06 | §14,18 | fetch-sources 為唯一下載步驟；腳本任意 cwd 可用、檢查 exit code；clean 僅已驗 repo build/run 邊界；persistent image 預設保留 | NOT_RUN |

## 分層與硬體介面

| ID | 規格 | 可驗收要求 | 狀態 |
| --- | --- | --- | --- |
| A01 | §5,9 | device fwcore 為 FSM/ledger/mailbox/scheduler 唯一權威；PF 只 snapshot/adapter/host transaction | NOT_RUN |
| A02 | §5 | native fake HAL、QEMU、UEFI dummy context 三個 core adapter；UEFI 不控制真實 device FSM | NOT_RUN |
| A03 | §5,9 | core C11、固定陣列、無 heap/VLA/OS/pthread/global mutable/recursion/unchecked arithmetic；HAL ownership/status 明確 | NOT_RUN |
| A04 | §5,11 | PF BAR/control root:root 0600，無 tenant mmap；QMP 僅 L1 mode0600；guest offline | NOT_RUN |
| H01 | §6 | PCI vendor1234/PF e001/VF e002/rev01；固定 class 記 manifest；driver ID 與 BAR magic/ABI 雙重驗證 | NOT_RUN |
| H02 | §6 | TotalVFs=InitialVFs=8；enable 1/2/4/8、預設4；N>8拒絕；每VF單SQ/CQ、queuepool8 | NOT_RUN |
| H03 | §6 | PF/VF BAR0 64-bit non-prefetchable 64KiB/16KiB；BAR1 high half；BAR2 32-bit16KiB、table0/PBA1000、固定2vectors；BAR3–5 unused | NOT_RUN |
| H04 | §6 | 正式 SR-IOV/ARI/ACS 能力、FirstOffset/Stride/RID 唯一且在 bus range；實際 forwarding 確認；禁止 ACS override/P2P 假宣告 | NOT_RUN |
| H05 | §6 | M1四VF完整groupsnapshot、每VF獨立 viablegroup/container；TYPE1實開+MAP/UNMAP；PF toy_pf、VF vfio-pci | NOT_RUN |
| H06 | §6 | 所有SQ/payload/CQ DMA用對應 VF PCIDevice requester，檢查每次 helper result；不使用 PF/global RAM | NOT_RUN |
| H07 | §6 | bounded <=4KiB payload/chunks，跨頁partialwrite不保證rollback；失敗不用未初始化資料、不crash/無限retry | NOT_RUN |
| H08 | §6,10 | PCI realize/unrealize 只 presentation、不偷偷修改 ledger；PROVISION在enable之前；失敗反向unwind含retainedledger | NOT_RUN |

## MMIO、ring 與 mailbox

所有逐欄 offset/權限/值由 REGMAP.md 與其規範摘錄覆蓋；兩份均須與共享 C schema 常數一致。

| ID | 規格 | 可驗收要求 | 狀態 |
| --- | --- | --- | --- |
| R01 | §7 | 全 MMIO 僅 aligned32LE；錯 size/alignment 回0、BAD_MMIO飽和++、無副作用；RO/reserved write不改值並計數 | NOT_RUN |
| R02 | §7 | PF SNAPSHOT一致擷取session及VF RO欄位；VF EPOCH_LATCH只擷取自己；64-bit low/high 不撕裂 | NOT_RUN |
| R03 | §7 | FENCE_TOKEN非0不可wrap；每VF(session,token,resultEpoch)去重；舊token拒絕；HW_RESULT/HW_STATUS readback確認 | NOT_RUN |
| R04 | §7 | PF FENCE_BEGIN原子fence→epoch++→cancel token++；FW hang不阻擋；KILL只globalreset可解除 | NOT_RUN |
| R05 | §7 | mailbox fields於SUBMIT同步snapshot，BUSY不覆蓋請求，DONE最後發布+PFvector0；ACK不清去重cache | NOT_RUN |
| R06 | §7 | staging CONFIG_COMMIT一次生效；N=16..256 power2、SQ=CQ、64B對齊、checked範圍、不重疊、liveepoch/session；RUNNING拒絕 | NOT_RUN |
| R07 | §7 | reset/config清indices、configvalid/readygen/ready_pending正確；租戶不能讀寫PF/其他VF | NOT_RUN |
| R08 | §7 | MSI-X table/PBA使用helper，mask/unmask pending正確；epoch/fence清舊pending；PF/VF各2固定vectors | NOT_RUN |
| R09 | §7,17 | M5前CAP_FEATURES A/B=0，stagewrite/UPDATE回UNSUP；M5後32KiB aperture與chunk<=4096、4B對齊、上下界/padding/target/args驗證 | NOT_RUN |
| Q01 | §8 | SQ/CQ精確32B與各offset，byte encode/decode LE不cast packed；FILL/CRC32向量123456789=cbf43926 | NOT_RUN |
| Q02 | §8 | len1..4096/op1,2/flags0，未知/overflow BAD_DESC；同epoch outstanding id唯一；一次fetch複本避免TOCTOU | NOT_RUN |
| Q03 | §8 | uint32模差tail-head<=N；mask index；nearUINT32_MAX、wrap、phase公式1^((counter/N)&1)驗證 | NOT_RUN |
| Q04 | §8 | release SQ→SQ_TAIL→doorbell；CQ內容/reserved→phase最後→CQ_TAIL→IRQ；tenant acquire/drain後CQ_HEAD | NOT_RUN |
| Q05 | §8 | CQfull停止fetch、不覆寫；每VF最多1delayed且CQreserved算容量；retained含session/epoch/token/desc copy | NOT_RUN |
| Q06 | §8 | staleSQ只consume與STALE_DESC++、不payload/CQ；staleCQ真callback最終gate只STALE_CPL++、不DMA/IRQ | NOT_RUN |
| Q07 | §8 | SQfetch/CQwrite fault→fence/cancel/FAILED/health；payloadfault可安全CQ則先DMA_ERROR再fence | NOT_RUN |
| Q08 | §8 | tenant CONFIG前全CQ清零；VFconfig RMW MemorySpace/BME讀回；eventfd計數非CQcount；x86barrier限制寫明 | NOT_RUN |
| B01 | §9 | 每session先VERSION_NEG；major不支援UNSUP；minor選交集；未neg NO_NEG；global一outstanding | NOT_RUN |
| B02 | §9 | (session,seq)+fingerprint去重；同key內容不同SEQ_CONFLICT、舊seqSTALE_SEQ、seq不可wrap；保完整最後reply | NOT_RUN |
| B03 | §9 | 01..09及20..24所有opcode/target/args完整驗證；16種fw_status固定值，kernel負errno明列 | NOT_RUN |
| B04 | §9 | 掉IRQ/ACK可poll狀態/ACK_SEQ；retry不重副作用；舊ACK不能complete新請求 | NOT_RUN |
| B05 | §9 | fw_init/fw_handle_mailbox/fw_vf_event/fw_schedule_round/fw_snapshot/fw_global_recover真實可用；所有非法state/event完整context不變 | NOT_RUN |

## 生命週期、Linux 與 coordinator

| ID | 規格 | 可驗收要求 | 狀態 |
| --- | --- | --- | --- |
| L01 | §10 | admin序列化provision/enable/disable；非0改非0拒EBUSY；所有stage及enable失敗reversecleanup | NOT_RUN |
| L02 | §10 | reset嚴格fence/token→quiesce/cancel→reset→RECOVERING→oldownerunmap→newmap/configREADY→REARM→RUNNING readback | NOT_RUN |
| L03 | §10 | 100ticks quiesce/reset、1000ticks ready；99/100/101邊界；host5s/20s與TCG30s/120s另記，不代logicaldeadline | NOT_RUN |
| L04 | §10 | timeout KILL/FAILED保quota/mappingquarantine；healthyVF續跑；FW全hang仍HWfence/kill；不擅自globalreset | NOT_RUN |
| L05 | §10 | 標準VF FLR與VFIOopen/close/reset：fence/bump/cancel/queue/PBAclear→RECOVERING；真事件去重；freshlatch/config必需 | NOT_RUN |
| L06 | §9,10 | globalrecover需全部leaseclose/VFsdisabled→fenceall/drain→session及各epoch++/ledger0/FREE→clearkill保持fence；overflow拒wrap | NOT_RUN |
| L07 | §10 | disable在lease或assigned存在時拒絕且不變；close順序stop/fence/drain/unmap/exactsize/fdsclose/lease/unbind/disable/destroy | NOT_RUN |
| L08 | §10 | remove void不拒絕；acceptingfalse/dyingtrue/fenceall；鎖外cancelwork/timer、mask/sync/freeIRQ、disable/unmap/release；fdref保護ENODEV | NOT_RUN |
| L09 | §10 | hostile PFremove獨立VM/controller並行通知owner退出；不假設assignedVF可同步強拆；仍被引用state不得提早free | NOT_RUN |
| D01 | §11 | PFprobe enablemem/requestregions/iomap/validate/allocirq/requestirq/master逐步failureinjection/反向cleanup | NOT_RUN |
| D02 | §11 | GET_INFO/GET_VF/LEASE_ACQUIRE/RELEASE/RESET_VF/REARM_VF/GLOBAL_RECOVER/UPDATE UAPI fixedwidth/aligned/size/reserved/abicheck | NOT_RUN |
| D03 | §11 | admin→mbx鎖序；IRQ不睡眠、不取admin；waitACK不靠持admin worker；cancelwork/disable/sync無互等 | NOT_RUN |
| D04 | §11 | 每open bounded事件queue、reset/FLR/fault/remove、overflow LOST_EVENTS需resync；root debugfs診斷/failpoint非正式ABI | NOT_RUN |
| D05 | §11 | QEMU mutable state BQL/mainloop，無IOThread；guest SMP/MMIO/BH/FLR序列化 | NOT_RUN |
| V01 | §11 | classic VFIO API/version/TYPE1/VIABLE/container/deviceinfo/region/IRQ/mmapcap/iovaranges/pagesizes全部檢查 | NOT_RUN |
| V02 | §11 | map SQ READ/CQ WRITE/payload READWRITE；每VF獨立container；UNMAP檢查retval/exactsize再free，不由PF撤map | NOT_RUN |
| V03 | §11 | labd lease先於open，u64token/session/epoch/readygen驗證；tenant crash pidfd/wait/fence/fdclose；不fork/SCM_RIGHTS外流 | NOT_RUN |
| V04 | §11 | Unix HELLO/RESET_NOTICE/READY/STOP/EXIT_ACK長度前綴<=4KiB；version/session/vf/lease/SO_PEERCRED uid/pid/timeout驗證 | NOT_RUN |
| V05 | §11 | reset oldrequest ERROR、保留舊buffer直到fence/drain與unmap完成；quarantine不能冒稱已clean | NOT_RUN |

## QoS、UEFI、建置与紀錄

| ID | 規格 | 可驗收要求 | 狀態 |
| --- | --- | --- | --- |
| P01 | §12 | cap64/reserveweight1..64/sum<=64；eligible ACTIVE/gate/backlog/CQroom且無delayed；FAILED保ledger | NOT_RUN |
| P02 | §12 | base min(reserve,backlog) + weightedfloor/largestremainder、同餘輪轉、cap需求後重算；uint64 boundedloops | NOT_RUN |
| P03 | §12 | timer1ms、BH<=64、無eligible停timer、service執行輪轉；doorbell只kick；非真吞吐/P99 | NOT_RUN |
| P04 | §12 | equal4×16、weighted4/8/12/16、1000rounds normalizedJain>=.99；noisy40/8/8/8；idle/fenced/full無service；零樣本N/A | NOT_RUN |
| U01 | §13 | 完整EDK2 INF/DEC/DSC/probe/coreadapter；serial真coreFSM/CRC/ledger selftest與deviceVERSION | NOT_RUN |
| U02 | §13 | PCI標準/extended walker aligned/bounds/visited/maxhops；EFI_PCI_IO失敗使用有證據segmentconfig，不把0當無cap | NOT_RUN |
| U03 | §13 | 記BDF/Total/Initial/VFID/pagesizes/Offset/Stride；GetBarAttributes及window觀測，不writeallones/enableVF | NOT_RUN |
| U04 | §13 | required VF BAR0/2各128KiB，highhalf只算一次；assigned PF/window為真觀測，aperture在upstream有效window | NOT_RUN |
| U05 | §13 | PCD存在/type/default/OVMFoverride來自pinnedsource；positive/disabledvariant；無法--pcd覆寫用versionedDSC | NOT_RUN |
| U06 | §13 | probeonly cases+marker；handoff同boot先PROBE_PASS再Linuxenable4，不pci=realloc/assign-busses；negative只記實際觀察 | NOT_RUN |
| U07 | §13 | CODEreadonly+matchingVARS每runfresh；ESP fallback BOOTX64；EFIstub LoadImage/StartImage Unicode options/initrdprotocol實測 | NOT_RUN |
| G01 | §14 | 指定完整repo路徑/Make targets/雙語README/demo/cleanup；LF、build/downloads/artifacts ignored，release含filehash | NOT_RUN |
| G02 | §14 | base/stress/kasan/kcsan final.config checker；KASAN/KCSAN分build；symbol漂移查Kconfig等價證據 | NOT_RUN |
| G03 | §14 | initramfs掛proc/sys/dev/devpts/run/tmp、單PF檢查、udevfree VFIOdevnodes、binaries/interpreter/遞迴libs正確封裝 | NOT_RUN |
| G04 | §14 | TCG q35/max/inteliommu intremap/rootport/toy/1536MiB/2CPU/offline/strictiommu；KASAN2048MiB；KVM獨立profile | NOT_RUN |
| G05 | §14 | controller同驗guestexit1/marker/所有case/無panicWARNsanitizer；exit3/timeout/missinglog/SIGKILL不可PASS | NOT_RUN |
| G06 | §14,16 | 每run短mode0700獨立socketdir；rawserial/QMP/argv/result/elapsed/accel/seed/env/repolockconfigSHA；probeexit0不等於PASS | NOT_RUN |
| G07 | §15,16 | QMPcap/schema/qom-list實際path；所有faultdefaultoff/target0..7/opcount/virtualticks/ack；真callback/IRQ嘗試非counter造假 | NOT_RUN |
| G08 | §15 | 獨立Pythonoracle不由C表生成；semanticgolden assertions；4seeds×2000management ops/固定IO/20resetfaultrepeat | NOT_RUN |

## M5 capsule / persistence model

| ID | 規格 | 可驗收要求 | 狀態 |
| --- | --- | --- | --- |
| F01 | §17 | 每run regularfile128KiB，journals0/1000、slots2000/a000各32KiB、reserved12000；明確--initialize僅不存在檔 | NOT_RUN |
| F02 | §17 | TFW1 header64B所有LE/offset/length/ABI/flags/reserved/schema檢查；headerCRC只清自己、payloadCRC保留；固定正負向vectors | NOT_RUN |
| F03 | §17 | payload固定schema creditcap64/defaultreserve1..8/protocolminor/knownfeatures，未知欄位拒絕；不執行nativecode | NOT_RUN |
| F04 | §17 | flashHAL與boundedworker pread/pwrite/fdatasync檢查所有retval；MMIO不得blockingI/O；stage只inactive | NOT_RUN |
| F05 | §17 | TJNL4KiB完整CRC與header64B/generation/slotmetadata/state/reason/reserved；未知enum/overflow拒絕 | NOT_RUN |
| F06 | §17 | 新journal→sync→readback；保舊valid直到新完整；recover選CRCvalid最高generation，不mtime；全壞readonlysafeerror | NOT_RUN |
| F07 | §17 | ACTIVATE需leases0/VFdisable/fenceall；pending不改lastgood；health/selftest/version後CONFIRM alternatecopy同步commit | NOT_RUN |
| F08 | §17 | unconfirmedreboot/healthfail回lastgood，generation續增/reason；每checkpoint SIGKILL真processrestart同storage驗證 | NOT_RUN |
| F09 | §17,19 | 文檔一致聲明AB生命週期模型，無binary替換/簽章信任根/可信antirollback/真SSD掉電保證 | NOT_RUN |

## 20 條合法 FSM 轉移

每條皆需至少一個實際單元測試；所有未列出的 state/event 組合必須回 STATE 且完整 context 不變。硬體緊急 fence/kill 與全域 recovery 分層驗證。

| Edge | From | Event | To | 前提/副作用 | 狀態 |
| --- | --- | --- | --- | --- | --- |
| FSM01 | FREE | CREATE | PROVISIONING | fenced，預留ledger | NOT_RUN |
| FSM02 | PROVISIONING | COMMIT | ACTIVE | verify，qcfg invalid、fence仍關 | NOT_RUN |
| FSM03 | PROVISIONING | ABORT_OK | FREE | device disabled、ledger全釋放 | NOT_RUN |
| FSM04 | PROVISIONING | ABORT_FAIL | FAILED | 保留可能持有quota | NOT_RUN |
| FSM05 | ACTIVE | FENCE | QUIESCING | 先hardwarefence/bump | NOT_RUN |
| FSM06 | QUIESCING | DRAIN_OK | RESETTING | outstanding0 | NOT_RUN |
| FSM07 | QUIESCING | TIMEOUT | FAILED | KILL、quota保留 | NOT_RUN |
| FSM08 | RESETTING | RESET_DONE | RECOVERING | qcfg invalid、indices0 | NOT_RUN |
| FSM09 | RESETTING | TIMEOUT | FAILED | KILL | NOT_RUN |
| FSM10 | RECOVERING | READY_REARM | ACTIVE | newconfig/session/epoch、原子開gate | NOT_RUN |
| FSM11 | RECOVERING | VERIFY_FAIL | FAILED | fence/kill | NOT_RUN |
| FSM12 | ACTIVE | FLR | RECOVERING | fence/bump/clear/cancel、quota保留 | NOT_RUN |
| FSM13 | QUIESCING | FLR | RECOVERING | supersede FWreset、canceloldcallback | NOT_RUN |
| FSM14 | RESETTING | FLR | RECOVERING | supersede FWreset、canceloldcallback | NOT_RUN |
| FSM15 | RECOVERING | FLR | RECOVERING | 每真event bump，eventtoken去重 | NOT_RUN |
| FSM16 | ACTIVE | DESTROY | DESTROYING | tenantclose/unbound，fence/bump | NOT_RUN |
| FSM17 | FAILED | DESTROY | DESTROYING | killed/fenced，kill保持 | NOT_RUN |
| FSM18 | RECOVERING | DESTROY | DESTROYING | 無lease/outstanding | NOT_RUN |
| FSM19 | DESTROYING | RELEASE_OK | FREE | quota0/qcfginvalid/no callback，kill可保持 | NOT_RUN |
| FSM20 | DESTROYING | RELEASE_FAIL | FAILED | 保quota、待globalrecover | NOT_RUN |

## 不變量

| ID | Invariant | 檢查點 | 狀態 |
| --- | --- | --- | --- |
| I1 | sum queues/vectors/reserve <= cap；FREE無quota，FAILED可保留 | 每core op與host idle | NOT_RUN |
| I2 | DMA allowed ⇒ ACTIVE && CONFIG_VALID && !fence && !kill && BME | 每DMA/IRQ之前 | NOT_RUN |
| I3 | epoch/session不可wrap，oldcallback不可寫newring | fence/FLR/reset/recovery/callback | NOT_RUN |
| I4 | FREE ⇒ invalidqcfg/outstanding0/ledger0，kill仍可阻provision | destroy/unwind stablepoint | NOT_RUN |
| I5 | 每VF獨立viablegroup/container且VFrequesterDMA | M1startup/M3 activeisolation | NOT_RUN |
| I6 | ownerclose ⇒ fd/maps/events/heap退出；PFremove ⇒ work/IRQ/BAR/ref清理 | 各ownership獨立，不以FREE冒稱unmap | NOT_RUN |
| I7 | fence/kill readback後無payload/CQwrite或tenantIRQ | guard/callback/IRQ injection | NOT_RUN |
| I8 | core無heap/OS/globalmutable，storagebounds固定 | source/lint/nm/reachability合查 | NOT_RUN |
| I9 | 未consumeCQ不覆寫，indices與phase一致 | 每descriptor/wrap/full | NOT_RUN |
| I10 | 同(session,seq)retry最多一次副作用 | mailbox property/PFretry | NOT_RUN |

## Milestone gate

| Gate | 必要驗證 | 狀態 |
| --- | --- | --- |
| M0 | 環境/lock/schema/core/fakeHAL；20edges+全非法；100000ops、至少4seeds獨立oracle；line>=90%；ASan/UBSan clean；各fuzzer100000runs再300s | NOT_RUN |
| M1 | pinned QEMU+minimal PF+guest；TCGboot；4VF獨立group/domain MAP/UNMAP；enable/disable100；BAR/cap/RID/FLR/MSIX/source evidence | NOT_RUN |
| M2 | OVMF/probe真selftest；同boot Linuxhandoff enable4無realloc；disabled觀察；matchingCODE/VARS/freshNVRAM | NOT_RUN |
| M3 | T01–T10；KASAN/lockdep無report；VFIOowner清map、sameIOVA isolation；重跑M1 | NOT_RUN |
| M4 | T11–T21；4seeds×2000managementops/固定IO；KCSAN/lockdep/UAF無report；reset/fault至少20repeat | NOT_RUN |
| M5 | T22–T25；同storage跨QEMUrestart/journal recovery，真SIGKILL checkpoint | NOT_RUN |
| BASE | M0–M4全部必要TCGgate，isolation不可跳過 | NOT_RUN |
| EXTENDED | 乾淨build執行make verify-extended ACCEL=tcg，BASE+M5全部必要gate | NOT_RUN |

## T01–T25 測試案例

機器可讀操作/斷言/證據要求見 [tests/scenarios.json](../../tests/scenarios.json)。以下完整保留規格 §15–§17，包含每個 testcase 的精確期望與 fault / report / capsule 契約，避免摘要丟失要求。

## 15｜Milestone、測試矩陣與證據

每個 gate 先 build 再 run；not run、工具缺失、隔離失敗均不算 PASS。M1 就建立最小 PFdriver，不使用 stockvfio PF 造成 ownership 轉換；M3 在同 driver 擴充，所有修改回跑 M1。M2 在 directLinuxprofile 之外增加 OVMFprofile，兩套不能互相替代。

| Milestone | 施工內容 | 通過門檻 |
| --- | --- | --- |
| M0 | 環境、lock、REGMAP/schema、core、fakeHAL、oracle、sanitizer | 20 合法 edges 全覆蓋；全部非法 state/event 拒絕且 context 不變；100000 操作/至少 4seeds 與獨立 oracle 一致；core line≥90%；ASan/UBSan clean；每個 fuzz target100000runs，額外 300s 探索 |
| M1 | pinnedQEMU、PCI/SR-IOVcap、最小 PF、guest、vIOMMU/groupspike | TCGboot、4VFgroup/domain 可行；enable/disable4VF×100；BAR/cap/RID/FLR 宣告、MSI-X vectors、source evidence 齊全 |
| M2 | OVMF、UEFIprobe/selftest、handoff、disabled variant | probe-only 與 UEFI→Linux 同 boot 正向通過；Linuxenable4VF 不依賴 pci=realloc；負向觀察報告；matchCODE/VARS 與 freshNVRAM |
| M3 | fullPF、labd、VFIOtenant、rings、reset handshake | T01–T10 必要功能通過；kasan/lockdep 無报告；所有映射由 owner 清除；同 IOVA 隔離證據 |
| M4 | fault/QMP、併發、QoS、remove、KCSAN | T11–T21 通過；4seeds×2000 管理 ops，固定 io 負載；無 KCSAN/lockdep/UAF；至少 20 次 reset/faultcase 重複無 flaky |
| M5 | capsuleAB 持久化模型、CRC、generationjournal | T22–T25 通過；重啟 QEMU 後讀同 storage 可恢復；不以 QMP 單次設定值冒充斷電持久性 |

| Case | 操作 / fault | 精確預期與證據 |
| --- | --- | --- |
| T01 | 4VF enable/disable100 循環 | ledger 回 0；QEMUobject/guest 工作無增長；VF 不存在；VFIOfd/maps 為 0 |
| T02 | PROVISION 每個 stage 與 pci_enable_sriov 失敗點 | 反向 unwind 成功 FREE；unwind 失敗 FAILED+retainedledger；不超 capacity |
| T03 | FILL/CRC、N16/32/256、≥4096requests、多圈 wrap | CRC 正確，SQ/CQ 不得覆寫；index 近 UINT32_MAX 的 modeltest 正確 |
| T04 | 兩 VF 用同 IOVA 但不同 guestbuffer | A 寫只改 Apattern，B 只改 B；sourceDMA 須對 VFrequester |
| T05 | A 未 map 而 Bmap 該 IOVA、guard 頁／permission | A DMA 失敗，Bguard 不變，B 有效 I/O 繼續；log 含 A RID 與 devicefaultcounter |
| T06 | VFIO_DEVICE_RESET + open/close 重設 | epoch 更新、oldqcfg 消失；freshCONFIG/REARM 才 RUNNING；其他 VF 不中斷 |
| T07 | resetunderload + tenant 慢 ready | RECOVERING 期間 payload/CQ 不動；ready 新 epoch 後再恢復 |
| T08 | delayedcompletioncallback 於 reset 後執行 | STALE_CPL 上升，舊 CQ/payload 不寫；跟 STALE_DESC 分開 |
| T09 | tenant 提交舊 epoch、未知 opcode、len 溢位 | STALE_DESC／BAD_DESC 正確，有界 parser；無 OOB/UB |
| T10 | lease 在用 disable0；nonzero 改 nonzero | EBUSY/EPERM；VFs 與 quota 不變；close 後 0 成功 |
| T11 | fw_hang_vf + QUIESCE／RESETignore | KILL/FAILED；gate 讀回；healthyVF 持續 I/O；不偷偷全 PFreset |
| T12 | FW 全掛 + 硬體 FENCE_ALL | mailboxtimeout，PF-onlyregister 仍封所有 DMA/IRQ；host 能做隔離 cleanup |
| T13 | dropreply/duplicateACK/staleACK/seqconflict | 重送不重做副作用；staleACK 不 complete 新 req；unsupportederr 保持 context |
| T14 | 惡意 BMEtoggle/doorbell/config 寫＋fencedIRQattempt | BME 重新開仍無 DMA；configinvalid 拒絕；IRQsuppressed；不能解除 PF-onlykill |
| T15 | CQfull、SQtail 超 N、head 跳躍、不對齊 base | CQ 不覆寫；queueerror/fence；其他 VF 進度不受影響 |
| T16 | reset/destroy/disable 三路＋tenant 退出 | admin 序列化；driver 無死鎖/UAF；不持 mutexcancelwork |
| T17 | PFunbind/remove 期間 tenantIO 與 ctrlfd 存活 | 不能靠 remove 拒絕；fenceall、fdENODEV、close 安全；work/IRQ/BARref 清理 |
| T18 | tenantcrash/signal/fdcleanup | pidfd 偵測，VFfence；VFIOownerclose 解除 map；labdlease 撤銷；無遺留 child |
| T19 | QoS equal/weighted/idle/noisy/CQfull | 依§12oracle 逐 round 一致；Jainsample 集合正確 |
| T20 | GLOBAL_RESET 與 reprovision 同 VFslot | 所有 lease 先關閉，session/epoch 增加；老 callback/fd 不可恢復；kill 僅此解除 |
| T21 | uint64epoch/session 極限、seq 極限 | 禁止 wrap；FAILED/quarantine，不重新從 0 復用；modeltest 無需跑 2⁶⁴次 |
| T22 | M5badlength/CRC/ABI/rollbackpolicycapsule | inactive/activeslot 不被非法內容取代；失敗 code 與 crc 向量 |
| T23 | ACTIVATE 後未 CONFIRM、healthfail | 回 lastgoodslot；留 generation 與 rollbackreason |
| T24 | 在每個 journalwrite 階段 killQEMU 重啟 | 至少一個 validrecord；恢復合法 slot；不把 qemuprocess 重啟計 epochcoldreset 當 update 驗證 |
| T25 | stage/activate 與 VFtenant 並行 | stage 可 boundedsafe 執行；activate 必須所有 lease 退出/VFsdisable，否則 EBUSY |

### 不變量：層次與檢查點不可混淆

| ID | Invariant | 檢查点 |
| --- | --- | --- |
| I1 | Σquota queues/vectors/reserve≤cap；FREEslot 無 quota；FAILED 可保留 | 每個 coreoperation 後與 hosttransactionidle 後 |
| I2 | DMA allowed ⇒ ACTIVE & CONFIG_VALID & !fence & !kill & BME | 每次 DMA 與 IRQ 前，不等到 idle 才查 |
| I3 | epoch/session 不 wrap；oldcallback 不能寫新 ring | FENCE_BEGIN/FLR/reset/globalrecovery 與 callback 執行 |
| I4 | FREE ⇒ qcfginvalid/outstanding0/ledger0；KILL 可仍存在阻止 provision | destroy/unwindstablepoint |
| I5 | 每 VF 獨立 viablegroup+container+VFrequesterDMA | M1startup 與 M3activeisolationtest |
| I6 | ownerclose 完成 ⇒ fd/maps/events/heap 退出；PFremove 完成 ⇒ work/IRQ/BAR/ref 清理 | 各 ownership 獨立檢查，不假定 FREE 代表所有 mapping 不存在 |
| I7 | fence/kill 完成 readback 之後無 payload/CQwrite 與 tenantIRQ | guardpattern/callback/irqinjection |
| I8 | core 無 heap/OS/globalmutable 依賴；storagebounds 固定 | source/lint/nm/reachability，不能只 nm -u 就宣稱無 allocator |
| I9 | 未消費 CQ 不覆寫；validindices 與 publishedphase 一致 | 每 descriptor 以及 wrap/fulltest |
| I10 | 同(session,seq)重送最多一次副作用 | mailboxproperty 與 PFretrytest |

KASAN/KCSAN 與 lockdep clean 只能表示測試中未抓到問題；report 需列操作數、seeds 與 profile。failslab/kernelfault 功能可輔助，不靠 systemwide 隨機 allocationfail 使 guest 整體 OOM。主要用 namedfailpoint，每 point 第 n 次、once，保存控制腳本與 faultack；case 期望 fail 的 status 不當成 testfail。coreoracle 不可從 C 產生，故能抓設計差異；characterizationgolden 需要另寫 semanticassertions，不盲目接受首次 bug 輸出。


## 16｜故障注入介面與報告格式

QMP controller 先執行 qmp_capabilities，再用 query-qmp-schema/qom-list 與專案 properties 確認名稱；不猜 QOMpath。QOMid 為 toy，controller 解析 actualpath，保存 query 結果。所有 faultdefaultoff，target-vf 限定 0..7，trigger-after operationcount、delay-ticks 以 virtualtime，不以 hostsleep 作邏輯 fault。faultack 必須進 serial/QMPobservedlog，否則 test 不能算已注入。

| Property / knob | 作用 | 哪些不能受到影響 |
| --- | --- | --- |
| fault-vf / fault-after | 目標 slot 與指定 operationcount | 其他 VF |
| fw-hang-vf / fw-hang-all | 停止指定 corequeue/mailbox 進度 | PFhardwarefence/kill、machine/resetcleanup |
| drop-mbx-reply-once | 命令 sideeffect 與 cache 已 commit，只隱藏一次 ACK/IRQ | 同 seqretry 仍回原結果 |
| ignore-quiesce / ignore-reset | 延遲 FWack 超 deadline | hardwarecancel/fence 能力 |
| delay-cpl-ticks | 保留 descriptorcopy+token+CQreservation，再 timercallback | 不能繞過 finalgatecheck |
| try-stale-cpl / try-irq-fenced | 真正呼叫舊 callback/IRQgate 來證明被擋 | 不能直接 incrementcounter 偽造測試 |
| failpoint-name / failpoint-hit | PROVISION/driveralloc 等 named 一次失敗 | 不修改 testexpected 結果 |
| M5 power-cut-at | journal 持久化 step 通知 controller 以 SIGKILL 終止 QEMU | 不可用正常 shutdown 假裝斷電 |

```text
{
  "schema_version": 1,
  "gate": "m3",
  "status": "PASS|FAIL|BLOCKED|NOT_RUN",
  "repo_commit": "record-real-hash",
  "sources_lock_sha256": "record-real-hash",
  "accelerator": "tcg",
  "profile": "kasan",
  "seed": 101,
  "cases": [{"id": "T04", "status": "PASS", "evidence": ["serial.log", "groups.json"]}],
  "invariants": {"I5": "PASS"},
  "unexpected_kernel_reports": [],
  "limitations": []
}
```

上面是 schema 範例，非已完成 log；程式輸出的 hash 必須真值。VALIDATION.md 有表：case/profile/accel/seed/status/logpath，最終 completionreport 列未完成原因。用 Python 生成結果，禁止 grep 到 PASS 一字就讓整個 gate 成功。


## 17｜M5：A/B capsule 與斷電恢復模型

這一層是 firmware update lifecycle model。QEMU裡compiled C fwcore不因capsule被替換；capsule可切換受限韌體設定/宣告版本，驗證stage/validate/activate/confirm/rollback與持久化。不能稱「刷入真正controllerfirmware」、「signedsecureupdate」或「抗惡意rollback」。
storage 為每 demo/run 獨立的 regularfile，固定 128KiB；兩 32KiBslots 與兩 4KiBjournalrecord，其餘 reserved。QEMU property `fw-storage=...` 明確帶入；persistenttest跨process重啟沿用同一檔，一般測試freshcopy。core用flashHAL read/write/sync，fakeflash測tornwrite；QEMUadapter用有界pread/pwrite與fdatasync，每步驗retval。不是在MMIOhandler阻塞hostI/O，stage boundedchunk在worker進行；測試controller可在checkpoint殺process。

| 區域 | 固定契約 |
| --- | --- |
| Capsule header 64B | magic[4]=TFW1、header_len u16=64、format u16=1、image_len u32（≤32768−64）、fw_version u32、min_abi/max_abi u16、flags u32=0、payload_crc32 u32、header_crc32 u32；餘 32Breserved 零；所有 LE、CRC 欄位本身計 headerCRC 時置 0 |
| Payload | 小型固定 schema 設定：creditcap 必須 64、defaultreserve1..8、protocolminor、knownfeatureflags；不含 nativecode；未知 field 拒絕 |
| Journal 4KiB | magic、format、generation u64、active_slot/last_good/pending/state、slotcrc/length、reason、recordCRC、reserved；整 record 有 CRC；generation 禁止 wrap |
| STAGE/VALIDATE | 只寫 inactive；逐 chunk 與 totalcap 有界；全 capsuleCRC/ABI/schema 核對後 VALID |
| ACTIVATE | 所有 leases0、VFdisable、fenceall；寫 pendingjournal 並 sync；新 slotboothealthcheck；未 CONFIRM 不更新 last_good |
| CONFIRM | 新 slotselftest/version/health 通過；更新 confirmedjournal 至 alternatecopy，sync 後 commit |
| ROLLBACK | 失敗回 last_good；generation 繼續增加；記 reason；完全無 validjournal 則 readonlysafeerror 不是隨便 bootslot0 |

journalupdate：寫 inactivejournal 新 generation→sync→驗 readback；舊 validrecord 直到新 record 完整才可覆蓋。重啟選 CRCvalid 且 generation 最大的 record，不依 hostmtime。採 partialwrite/tornrecord/crcwrong 的 modeltests；killQEMU 只能驗 processcrash，無法證明真實 SSD 掉電保護。初始 factoryslotA 已 confirmed，初始化不與 recover 混在一起，只有--initialize 且檔不存在才允許建 factoryimage。

payloadCRC 與 headerCRC 只是非惡意完整性檢查；hash 可供發布 manifest 用 SHA256，但 manifesthash 不等於裝置驗簽。拒絕較舊 fw_version 是模型 policy，若 host 可改 storage 並無可信 antirollbackroot。M5 安全限制必須在 README 與面試說明一致。

### 持久化位址與 metadata

| 範圍 | 內容 |
| --- | --- |
| 0x0000..0x0fff / 0x1000..0x1fff | Journal copy 0 / 1，各 4KiB |
| 0x2000..0x9fff / 0xa000..0x11fff | Slot A / B，各 32KiB，capsule header 包含在 slot 內 |
| 0x12000..0x1ffff | Reserved；固定檔案總長度 0x20000（128KiB） |
| Journal record bytes 0..63 | magic[4]=TJNL、format u16=1、header_len u16=64、generation u64、active_slot u8、last_good u8、pending_slot u8（0xff=none）、state u8、reason u32、slotA_len u32、slotA_crc u32、slotB_len u32、slotB_crc u32、record_crc u32、reserved[20]；剩餘 block 零。record_crc 欄位計算時置 0，計算完整 4KiB。 |
| State / reason | state 0 FACTORY、1 CONFIRMED、2 PENDING、3 ROLLED_BACK；reason 0 NONE、1 BAD_CRC、2 BAD_ABI、3 HEALTH_FAIL、4 UNCONFIRMED_REBOOT、5 TORN_WRITE。enum 未知拒絕 recover。 |

Capsule header byte offsets：magic 0x00、header_len 0x04、format 0x06、image_len 0x08、fw_version 0x0c、min_abi 0x10、max_abi 0x12、flags 0x14、payload_crc32 0x18、header_crc32 0x1c、reserved 0x20..0x3f。header_crc32 計算完整 64 bytes 時只將 header_crc32 本身置零，payload_crc32 保留；payload_crc32 單獨對 payload 計算。這條精確規則優先於「CRC 欄位置0」的簡稱，測試必須提供正負向固定 capsule 向量。

## Case status index

This index records initial execution status only; actual verified results belong in VALIDATION.md and run manifests.

| Case | Milestone | Status |
| --- | --- | --- |
| T01 | M3 | NOT_RUN |
| T02 | M3 | NOT_RUN |
| T03 | M3 | NOT_RUN |
| T04 | M3 | NOT_RUN |
| T05 | M3 | NOT_RUN |
| T06 | M3 | NOT_RUN |
| T07 | M3 | NOT_RUN |
| T08 | M3 | NOT_RUN |
| T09 | M3 | NOT_RUN |
| T10 | M3 | NOT_RUN |
| T11 | M4 | NOT_RUN |
| T12 | M4 | NOT_RUN |
| T13 | M4 | NOT_RUN |
| T14 | M4 | NOT_RUN |
| T15 | M4 | NOT_RUN |
| T16 | M4 | NOT_RUN |
| T17 | M4 | NOT_RUN |
| T18 | M4 | NOT_RUN |
| T19 | M4 | NOT_RUN |
| T20 | M4 | NOT_RUN |
| T21 | M4 | NOT_RUN |
| T22 | M5 | NOT_RUN |
| T23 | M5 | NOT_RUN |
| T24 | M5 | NOT_RUN |
| T25 | M5 | NOT_RUN |
