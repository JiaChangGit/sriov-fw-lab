# 新手建置與除錯手冊

適用環境：Lenovo R7000P 2024、實體 RAM 16 GB、Windows 11、WSL2 Ubuntu 26.04。
專案內容是 **M0–M3：portable core、QEMU SR-IOV 裝置、guest PF driver、VFIO tenant、
UEFI handoff 與 T01–T10 隔離／生命週期驗證**。以下從相依套件與來源開始建置，
每一階段都列出命令、輸出位置和成功判斷。實際驗收版本及建置方式見 [VALIDATION.md](VALIDATION.md)。

第一次閱讀的順序是：本章完成環境與建置 → [系統分析](SYSTEM_ANALYSIS_ZH.md) 看責任和圖 →
[Kernel 導讀](KERNEL_WALKTHROUGH_ZH.md) 認識 driver → [逐函式追蹤](TRACE_CODE_ZH.md) 跑過一筆請求 →
[DEMO 手冊](DEMO_ZH.md) 實際操作 → [系統指南](SYSTEM_GUIDE_ZH.md) 查閱設計與限制。
遇到問題，先找本章最後的症狀表，再看 [真實 BUG 紀錄](BUG_LOG.md)。

## 開始前：要建立什麼

你不需要購買支援 SR-IOV 的網卡，也不需要把筆電 GPU passthrough 給 VM。
WSL 裡的 QEMU 用 TCG 模擬 x86 CPU 與 toy PCIe device；QEMU 內的 Linux 執行真正的
PF driver、VFIO ioctl 和 IOMMU 路徑。資料工作只有 FILL（寫固定 byte）與 CRC32（讀資料計算 checksum）。
簡單工作負載讓隔離、錯誤處理與 reset 更容易逐 byte 驗證。

| 名稱 | 新手先記住的意思 |
|---|---|
| PF / VF | PF 是管理 function；VF 是分配給 tenant 的資料 function；此模型最多八個 VF |
| Driver / module | Linux 核心程式；本專案的 `toy_pf.ko` 只載入 QEMU 裡的 Linux |
| VFIO | guest kernel 提供的介面，讓 userspace 擁有 VF、配置 DMA mappings 與中斷 |
| IOMMU / IOVA | 依裝置 requester 身分轉譯 DMA 位址；IOVA 不是 C pointer |
| SQ / CQ | Submission Queue 存請求；Completion Queue 存完成結果 |
| Gate | 程式要求通過的驗收關卡；另有硬體 DMA gate，兩者要依上下文區分 |
| Artifact | 一次建置或測試留下的 command、log、result、hash；不是只截一張 PASS 圖 |

首次下載和編譯需要網路與足夠磁碟；guest 測試不需要外部網路。整套流程包含 Linux、QEMU、
EDK2 建置與多輪 VM，應安排數小時的工作窗口；實際時間依 CPU、磁碟、快取與熱節流而變。
不要把其他機器的耗時當成 timeout 標準。全程保持筆電供電，關閉佔用大量 RAM 的其他 VM。

## 0. 先辨認終端機在哪一層

| 提示 | 所在層 | 工作 |
|---|---|---|
| Windows PowerShell | L0 | 查看 WSL、Windows RAM、磁碟、啟動 Ubuntu |
| WSL bash，路徑 `$HOME/src/sriov-fw-lab` | L1 | 編譯、啟動 QEMU、保存 artifacts |
| QEMU serial log | L2 guest | 執行 init、toy_pf driver、labd、tenant |

除本章明列為 PowerShell 的命令外，其餘都在 L1 的 repository 根目錄執行。
在 WSL 的專案目錄執行 `explorer.exe .`，可以用檔案總管開啟同一份檔案。
真正編譯應由 WSL 在原生 Linux 檔案系統執行；不要搬到 `/mnt/c` 後照抄命令。

## 1. Windows 與 WSL 檢查

PowerShell：

```powershell
wsl --version
wsl --status
wsl --list --verbose
Get-CimInstance Win32_ComputerSystem | Select-Object Model, TotalPhysicalMemory
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors
Get-PSDrive C, D
Get-Content "$env:USERPROFILE\.wslconfig" -ErrorAction SilentlyContinue
wsl -d Ubuntu-26.04
```

確認 Ubuntu-26.04 的 VERSION 是 2。`wsl --status` 的預設發行版不一定就是這個專案使用的發行版，
因此明確使用 `-d Ubuntu-26.04`。缺 `.wslconfig` 表示使用 WSL 預設，不是本身就有錯。

既有實測記錄位於 `artifacts/environment/windows.json`，包含 Ryzen 7 7840H、8 核心/16 threads、
Windows 可見約 16 GB RAM，以及原本 `memory=9319743488` 的設定。
這是當時的機器觀測，不據機型名稱猜 KVM 能力；以重新檢查結果為準。

16 GB 機器的操作預算是 JOBS=2、一次一項大型建置、一次一個 guest。
一般 guest 1536 MiB，KASAN guest 2048 MiB。Swap 只能緩解壓力，不代替可用 RAM。
建置預算建議 8 GiB WSL RAM、6 processors、4 GiB swap；已存在的設定若可完成驗收，無須僅為配合範例而修改。
若自行調整 `.wslconfig`，先備份並合併其他設定，再於所有工作停止後重啟 WSL。
執行中的 build/VM 期間不要 `wsl --shutdown`，它會中斷測試與保存程序。
設定檔的作用範圍與重新啟動規則可對照 [Microsoft WSL 設定文件](https://learn.microsoft.com/en-us/windows/wsl/wsl-config)。
這裡的 8 GiB／JOBS=2 是本專案工作預算，不是 Microsoft 對所有工作負載的保證。

## 2. 進入專案並檢查 Linux 環境

先取得公開儲存庫的 Git URL，在 WSL 建立工作目錄。下列第一行請填入實際網址；
已經有專案時，直接進入現有目錄即可。

```sh
LAB_REPO_URL='填入儲存庫的 Git URL'
mkdir -p "$HOME/src"
git clone "$LAB_REPO_URL" "$HOME/src/sriov-fw-lab"
```

如果取得的是 ZIP，先解壓縮到 WSL 的 Linux 檔案系統，再於專案根目錄執行：

```sh
git init -b main
```

並設定自己的 Git 顯示名稱與公開用信箱，再建立首次 commit。建置工具需要 Git 工作目錄，
不會替你設定身分；公開提交可使用 GitHub 帳號提供的 noreply 信箱。
以下以 `$HOME/src/sriov-fw-lab` 為例，`$HOME` 會展開為目前 WSL 使用者的家目錄。

原始碼應包含 `Makefile`、`sources.lock.json`、`fwcore/`、`device/`、`guest/`、`tools/`、
`tests/`、`patches/` 與 `uefi/`。Linux upstream 由 fetch-sources 取得，不是頂層 `kernel/`。
下載的 upstream、編譯產物和歷史 artifacts 不是替代原始碼的方式。

```sh
cd "$HOME/src/sriov-fw-lab"
pwd -P
findmnt -T .
free -h
df -h .
git status --short
git rev-parse HEAD
test -f Makefile && test -f sources.lock.json
make env-check
```

成功判斷：最後 JSON 的 status 是 PASS，`artifacts/environment/wsl.json` 沒有 blockers，
filesystem semantics 的大小寫、symlink、executable 都成立。至少預留 60 GiB，建議 80–100 GiB。
還要檢查 Windows 的實體 backing disk 可用空間；WSL 的稀疏 VHD 顯示大容量不代表 Windows 磁碟未滿。

`/dev/kvm` 存在仍不代表巢狀 KVM 可初始化。本專案必要路徑是 TCG，所有主驗收明確用 `ACCEL=tcg`。
KVM 不在本版交付範圍，CLI 只接受 TCG；不能改成 KVM 來跳過失敗。

若工具缺失，環境檢查會列出缺項。首次安裝套件，在一般使用者的 WSL 終端機進入專案根目錄後執行：

```sh
sudo python3 tools/install_deps.py
```

它安裝系統 apt dependencies，保存 apt-cache、apt-get 和 dpkg-query log 到
`artifacts/dependencies/`。這不是每次 build 都要做的步驟，也不使用 sudo pip。
完成後回到一般 user 的 WSL shell 重跑 `make env-check`。

若依賴安裝因網路失敗，先看 `artifacts/dependencies/` 最新一輪的 `00.log`～`03.log` 和
`result.json`。修正網路／套件來源後重跑同一支 bootstrap；不要把缺少套件造成的後續編譯錯誤
當成 C 程式問題。`make env-check` 回 BLOCKED 時，先處理 JSON 的 blockers，再往下走。

## 3. 釘選來源與工具鏈

```sh
make fetch-sources
python3 -m json.tool sources.lock.json
```

唯一來源下載入口是 `fetch-sources`。`downloads/` 保存 pinned upstream checkout，
`build/` 裡建立可套 patch 的建置工作樹；不要直接修改 immutable upstream 來修專案。

| 元件 | Ref | 精確 commit |
|---|---|---|
| QEMU | v9.2.0 | ae35f033b874c627d81d51070187fbf55f0bf1a7 |
| Linux | v6.12 | adc218676eef25575469234709c2d87185ca223a |
| EDK2 | edk2-stable202411 | 0f3867fa6ef0553e26c42f7d71ff6bdb98429742 |
| BusyBox | 1_36_1 | 1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4 |

Lock 還保存 recursive submodules、patch SHA256 與工具鏈。FETCHED 只證明取得來源；
不代表編譯、開機、隔離測試成功。完整依賴背景見 [DEPENDENCIES.md](DEPENDENCIES.md)。

目前 QEMU/guest userspace 使用 GCC 15；pinned Linux 與 EDK2 使用已實測的 GCC 14，
EDK2 的工具鏈 tag 名稱仍是 GCC5。這個 tag 不代表實際執行 gcc 5。
Kernel builder 明確指定 CC/HOSTCC，不用修改全機 compiler alternatives。

來源目錄與工作目錄的用途如下。`downloads/` 與 `build/` 是兩層，是為了讓原始 upstream
保持可比對，也讓 patch 只進入可重建的工作樹。

| 路徑 | 內容 | 能否手動改了就繼續驗收 |
|---|---|---|
| `fwcore/`、`device/`、`guest/` | 本專案實作 | 修改後重建並重驗受影響 gate |
| `sources.lock.json` | commit、submodule、patch、toolchain 記錄 | 修改代表換了驗證輸入，必須重新驗證 |
| `downloads/` | 釘選 upstream checkout | 不可直接修；應建立有 hash 的專案 patch |
| `build/` | 可丟棄的 binary、worktree、stamp | 不用手改 stamp 讓舊 binary 冒充新版本 |
| `artifacts/` | 每一次真實執行的證據 | 保存原 FAIL；新結果放新目錄 |

若 fetch 因網路中斷，修復連線後重跑 `make fetch-sources`；若是 commit/hash 不符，
先保存錯誤與 `git status`，確認 checkout 是否被修改。不要隨手改 lock 配合手上的檔案。

## 4. M0：先驗 portable core

```sh
make build-core
make verify-m0
```

成功判斷：`artifacts/m0-latest.json` status PASS，指向的 result 也 PASS；
core 證據涵蓋 FSM、非法轉移 context 不變、獨立 Python oracle、ASan/UBSan、coverage 與 fuzz。
目前完整 core 基線有四個 seeds 各十萬操作、每個 fuzz target 十萬次加額外 300 秒探索。
這一步可能數分鐘，不因一段時間沒有終端輸出就判定卡死；詳細輸出寫入各步驟 log。

若 native unit 失敗，先修 core 再進 VM，因為同一 core 會連進 QEMU。
不要只看 coverage 百分比：有執行過某一行，不等於獨立 oracle 認可其結果。

## 5. M1：QEMU、guest kernel 與真實 VFIO

```sh
make build-qemu JOBS=2
make build-kernel PROFILE=stress JOBS=2
make build-initramfs PROFILE=stress JOBS=2
make verify-m1 PROFILE=stress ACCEL=tcg JOBS=2
```

每條命令成功才執行下一條。`verify-m1` 本身包含 build prerequisites，已完成的工作會使用
建置系統的增量處理；stamp 不符時不能只因 binary 存在就當成可用。

| 輸出 | 用途 |
|---|---|
| `build/qemu/qemu-system-x86_64` | 含 toy device 的 host QEMU |
| `build/kernel-stress/arch/x86/boot/bzImage` | L2 Linux kernel，不是 WSL kernel |
| `build/initramfs-stress/initramfs.cpio.gz` | BusyBox、driver module、guest programs 和實際 ELF dependencies |
| `artifacts/m1-latest.json` | 指向本次 M1 結構化報告 |

M1 不是只看 `lspci` 出現四個 VF。它檢查 PCI BAR/capabilities、實際 group viability、
每個 VF 的獨立 container、TYPE1 MAP/UNMAP、反覆 enable/disable 和資源回收。
Guest 不需要外部網路。PF module 只放入 L2 initramfs，不要在 WSL 執行 `insmod toy_pf.ko`。

## 6. M2：UEFI probe 與同次開機交接

```sh
make verify-m2 PROFILE=stress ACCEL=tcg JOBS=2
```

工具會編譯 EDK2/BaseTools、positive/disabled OVMF variants 與 SriovFwPkg，
先做 probe-only，再測 UEFI probe → Linux 的同次開機 handoff，最後跑 disabled PCD 對照。
每次都複製該 build 配套的新 VARS；不要拿舊 NVRAM 或另一版 CODE 拼在一起。

成功判斷：`artifacts/m2-latest.json` 和其 result PASS，三個 mode 都有 raw serial、QMP、
CODE/VARS provenance、ESP files 和結果。UEFI application exit 0 本身不足以宣稱 Linux handoff 成功。

Negative variant 關閉 PCD 後，Linux 仍可能在既有 bridge window 中成功配置 VF。
這是本機實測觀察，測試不硬寫「必須失敗」來迎合名稱。
更多細節與 source anchors 見 [UEFI.md](UEFI.md)。

## 7. M3：KASAN 與隔離／生命週期驗證

```sh
make build-kernel PROFILE=kasan JOBS=2
make build-initramfs PROFILE=kasan JOBS=2
make verify-m3 ACCEL=tcg
```

完整 M3 gate 會重驗目前 M0 來源、重建 stress archive，依序執行 KASAN/stress M1、
三次 M2 boot、T01–T10 及 M3 controller/validator 測試。T07 包含精確 native deadline 測試。
修正後版本的完整 aggregate 證據見 [VALIDATION.md](VALIDATION.md) 首節；
`artifacts/m3/20261013T074512Z-64uwugjd/` 是較早的開發基線，不能替代目前版本的驗收。
舊的 `tools/verify_m3_checkpoint.py` 只跑 T01–T06/T09/T10，仍可用來定位回歸，但不能當作完整 M3。
執行時不要修改 device、guest、kernel patch 或 validator，以免測試中的來源與報告不一致。

單獨定位問題時可執行：

```sh
make verify-t01
make verify-t02
make verify-t03
make verify-t04
make verify-t05
make verify-t06
make verify-t07
make verify-t08
make verify-t09
python3 tools/run_vm.py --gate m3-case --profile kasan --accel tcg --case T10
```

這些工具要求目前 binary 的 M1/M2 前提已通過。若先更動 guest 或 QEMU，重新建置後要重跑受影響
前提，不能沿用另一組 binary 的 PASS。T01 的完整生命週期與 M1 的 enumeration loop 是不同覆蓋。

T05 的六個 DMA fault 變體使用已釘選的 toy-only strict WRITE-only guest kernel patch。
Boot marker 與真實 fault 皆要成立。Stock Linux 的 WRITE-only read rejection 曾實跑 FAIL；
原因與限制見 [BUG_LOG B002](BUG_LOG.md#b002要求-write-only-不代表-pinned-linux-會禁止讀取)。

T07/T08 的單一 VM 診斷入口如下。它不重建前提，也不取代 aggregate：

```sh
python3 tools/run_vm.py --gate m3-case --profile kasan --accel tcg --case T07
python3 tools/run_vm.py --gate m3-case --profile kasan --accel tcg --case T08
```

成功後查看 `artifacts/m3-case-latest.json` 指向的目錄。用 Python 讀取指標可以避免抄錯 timestamp：

```sh
python3 - <<'PY'
import json
from pathlib import Path
p = json.loads(Path('artifacts/m3-case-latest.json').read_text())
directory = Path(p['path'])
r = json.loads((directory / 'result.json').read_text())
print('run:', directory)
print('status:', r['status'])
print('error:', r.get('error', 'none'))
print('kernel reports:', r.get('unexpected_kernel_reports'))
print('evidence:', *r.get('evidence_hashes', {}), sep='\n  ')
PY
```

不要在進行中的 run 檢查舊 latest 指標並誤認本次通過；先等 command 返回，再核對時間、case、
command.json 與 build hashes。缺 result 或未跑的 case 仍是 NOT_RUN。

## 8. 從乾淨 build 重新建置

完整建置包含 M0–M3。Capsule 更新、持久化模型及實體板卡操作不屬於本專案。
部分底層程式有防禦性處理，但可主張的測試覆蓋仍以 T01–T10 的實際結果為準。

第一次建置時 `build/` 不存在，直接依序執行下列命令。若要驗證已有環境的乾淨重建，先等所有
build 和 QEMU 停止、保存需要的輸出，再單獨執行 `make clean-build`。
它會刪除已檢查邊界內的整個 `build/`；原始碼、upstream downloads 與歷史 logs 保留。
不要在另一個終端機仍在測試時清理。

```sh
cd "$HOME/src/sriov-fw-lab"
make env-check
make fetch-sources
make verify-project JOBS=2
make build-handbook
```

`verify-project` 會先跑完整 M0（包括兩個 300 秒 fuzz exploration），建立 QEMU、兩個 kernel 與
initramfs，再跑 M3 aggregate；其中會建立 EDK2，執行兩個 M1 profile 與三條 UEFI boot path。
最後跑全部 Python validators 和 native guest regression。每一步失敗就停止；這個入口不下載來源，
也不會自動刪除 build。第 4–7 節則是拆開定位問題時的逐步版本，不必在完整入口前再重複整輪。

完整入口的進度與結果：

```sh
python3 - <<'PY'
import json
from pathlib import Path
pointer = json.loads(Path('artifacts/project-latest.json').read_text())
result = json.loads((Path(pointer['path']) / 'result.json').read_text())
print(pointer['path'], result['status'])
print('空 build 起跑:', result['build_started_empty'])
for step in result['steps']:
    print(step['id'], step['status'])
print(result.get('error', '無錯誤'))
PY
```

這段要等命令結束後執行，首次 run 未結束可能尚無 latest。執行中可在另一個 WSL terminal 看
`ls -td artifacts/project/*/ | head -1` 找本次執行目錄，讀其中 `result.json` 和當前 stage 的 `.log`。
不要只依 timestamp 猜測完成，需核對程序與 step。

### 執行範例

完整入口已 PASS 後，可依 [系統指南](SYSTEM_GUIDE_ZH.md) 理解設計，再執行：

```sh
make demo
make demo-reset
```

前者展示 M1 的四 VF／domain 與 100 次生命週期；後者展示 T07 的 outstanding-work reset 和健康 VF。
逐步操作、結果讀取程式及現場講解順序見 [DEMO 手冊](DEMO_ZH.md)。
兩者各自產生新的 artifacts；它們不是完整測試的捷徑。若要展示同 IOVA 隔離，使用 `make verify-t04`；
要展示舊 callback 跨 reset 被擋，使用 `make verify-t08`。查看已保存結果時，應核對其版本與時間。

逐條執行，前一條 exit code 非零就停止並除錯。不要用分號串起整批命令後只看最後一條。
M3 aggregate 會再次檢查當前 M0/M1/M2 prerequisites，前面的拆解仍有教學與提早定位錯誤的用途。
這是可重建程序；它是否在指定版本實際從乾淨 build 跑完，仍由 VALIDATION 的當次紀錄回答。

要確認目前歷史指標而不啟動 VM，可執行以下唯讀命令。它不等於重新測試：

```sh
python3 - <<'PY'
import json
from pathlib import Path
for gate in ('m0', 'm1', 'm2', 'm3'):
    pointer = Path('artifacts') / f'{gate}-latest.json'
    if not pointer.exists():
        print(gate, 'NOT_RUN')
        continue
    latest = json.loads(pointer.read_text())
    report = Path(latest['path']) / 'result.json'
    result = json.loads(report.read_text()) if report.exists() else {}
    print(gate, latest.get('status'), result.get('status', 'MISSING_RESULT'), report)
PY
```

如果已通過的 binary 被修改，latest 檔案仍可能存在；必須再核對 result 中的輸入與建置 hash。
歷史資料不能自動替現在的 source tree 背書。

## 9. 讀懂一次測試的證據

先看 aggregate result 的 status，再追 `path`，不要只看某段 log 的 PASS 字串。
例如 M3 一次 run 通常有以下檔案：

| 檔案 | 回答的問題 |
|---|---|
| `result.json` | exit code、timeout、cases、build/evidence hashes、unexpected reports 是否成立？ |
| `command.json` | 真正執行了哪個 QEMU、kernel、initramfs、profile、accelerator？ |
| `kernel.log` | Linux boot、driver、IOMMU fault、KASAN/lockdep 等是否異常？ |
| `serial.log` | 哪個 owner、VF、epoch、lease、request、MAP/UNMAP 在何處失敗？ |
| `qemu.log` | 實際 VF requester、FLR、fence、core transition、DMA/callback 在何處？ |
| `m3-barriers.json` / `qmp.jsonl` | fault 是否真正 ACK？觀測是否由裝置回覆？ |

所有 Linux gate 都使用雙 UART：UART0 的 `kernel.log` 保存核心診斷，UART1 的 `serial.log`
保存測試 JSON 與雙向 checkpoint。UEFI handoff 的 `kernel.log` 還包含前面的 firmware probe，
因此從該檔追 `PROBE_PASS` → `HANDOFF_START` → `Linux version`；Linux 測試事件再看 `serial.log`。
Probe-only 不啟動 Linux，仍把 firmware 輸出放在 `serial.log`。這項差別由 runner 處理，不需手改 QEMU 命令。
只看 guest 最後的 PASS 不夠：B020 的 guest PASS 曾因 raw JSON 被切斷而被 host 正確拒收。

不同 run 不混用證據。程式修正後開新目錄重跑，原始 FAIL 保留。
aggregate 尚未完成時，result 可能仍是 provisional FAIL、後續 steps NOT_RUN；
程序被中斷後不能把 prefix 的 PASS 自動當成全輪 PASS。

## 10. 常見症狀的定位順序

| 症狀 | 先檢查 | 原因與處理 |
|---|---|---|
| PowerShell 找不到 make / gcc | 所在層、`wsl -d Ubuntu-26.04` | 建置命令應在 WSL；不要另裝 Windows compiler 替代 |
| Git 或 shell script 出現 CRLF 問題 | source checkout、`.gitattributes`、configure log | fetch 固定 LF；不要全域更改 Git 後盲目重試 |
| Kernel EFI stub 編譯失敗 | build argv 的 CC/HOSTCC、D007 | pinned Linux 與 GCC 15 相容性已見失敗；使用 lock 中 GCC 14 |
| EDK2 assembly 失敗 | BaseTools/NASM 版本、D009 原始決策 | 保留工具版本與原始錯誤，套用已記錄 compatibility 處理 |
| guest 找不到已打包的執行檔 | ELF interpreter 與 recursive shared libraries | 路徑存在但 loader 缺失也會像找不到檔案；檢查 initramfs manifest |
| VFIO group 不 viable | group members、driver binding、拓樸 | 不用 ACS override 冒充隔離；修 actual topology/binding |
| 沒 completion | SQ_HEAD、CQ_TAIL、flags、epoch、QEMU DMA log | 先區分沒 fetch、payload fault、CQ fault、IRQ 通知問題 |
| reset 後 ESTALE | 實際 FLR、session/epoch、ready_gen | open/close 可能 FLR；新 queue 必須重新 latch/config/rearm |
| JSON parse failure | kernel.log 與 serial.log 是否混流 | 已採雙 UART；不要刪掉壞行後算 PASS |
| UNMAP 回傳 size 太小 | requested/returned extent、owner maps | 不可 free 尚可能 pinned 的 allocation，先保留到 VFIO fd close |
| 長時間無終端輸出 | 當前 step log、QEMU process、timeout | 輸出可能被收進 artifact；不要啟動第二個大型 run 搶資源 |
| 最新 result 不見了 | 程序是否中斷、各 step JSON 是否存在 | 保存中斷事實，重跑；沒有 final result 就沒有完整驗收 |

這張表包含一般診斷策略，不表示每一項都在本機重現。已重現的 BUG、原始失敗、修正及回歸
另列於 [BUG_LOG.md](BUG_LOG.md)，平台決策另列於 [DECISIONS.md](DECISIONS.md)。

### 一次失敗要保存哪些內容

1. 命令與所在層：是在 PowerShell、WSL 還是 guest；保存真正的 argv 和 exit code。
2. 最早的錯誤：compiler 第一個 error、kernel 第一個 warning、或第一筆不符合契約的 event。
   最後的 `make Error 2` 通常只是上游失敗的結果。
3. 當時的輸入：Git commit/dirty state、source/patch hash、kernel profile、accelerator 與 case。
4. raw logs：保留整個 run 目錄；不要先手工刪行再把清理過的 log 丟給 validator。
5. 修正後的新 run：小範圍重現先通過，再跑受影響 gate；新結果連到原 FAIL，兩者都保留。

記憶體不足時，`cc1: fatal error: Killed signal` 或 exit 137 只是一個線索。
在 WSL 查 `free -h`、`dmesg`／可取得的核心紀錄，再對照 Windows RAM 壓力；確定是 OOM 才調整工作量。
先停掉其他大型工作並保持 `JOBS=2`；必要時用 `JOBS=1` 重建。
不要用關閉 sanitizer、忽略 warning 或加大所有 timeout 來掩蓋原因。

## 11. 離線教材如何重建

讀教材直接開啟 `docs/handbook.html`；圖表和樣式內嵌，無需啟動 web server。
保留 `docs/code/`、其他 Markdown 和原本 artifacts，才能繼續點進原始碼與證據。
若修改教材來源，於 WSL 執行 `make build-handbook`，同時生成與檢查教材。
Renderer 使用 Ubuntu 的 `python3-markdown-it`；重建時所用版本會記錄在
`docs/handbook-manifest.json`。缺少時透過 apt 安裝該套件，再執行 renderer；不會在 build 過程偷偷下載。
HTML 產生成功只代表文件生成成功，與專案 gate PASS 分開記錄。
