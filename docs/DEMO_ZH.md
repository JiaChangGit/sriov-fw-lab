# DEMO 操作手冊：隔離、reset 與測試證據

這份手冊用三個展示回答三件事：四個 VF 是否真的各有 VFIO domain、reset 是否影響其他 VF，
以及相同 IOVA 是否仍能保持資料隔離。所有命令都在 **WSL 的 repository 根目錄**執行。
正常使用不需要手動登入 guest，也不需要在 WSL 載入 driver。

## 1. 展示前準備

先完成 [建置教學](BUILD_TUTORIAL_ZH.md) 的完整驗收。首次建立 kernel 可能耗時較久，
執行展示前先完成建置。

PowerShell：

```powershell
wsl -d Ubuntu-26.04
```

進入 WSL 後：

```sh
cd "$HOME/src/sriov-fw-lab"
make env-check
```

確認目前已有完整結果：

```sh
python3 - <<'PY'
import json
from pathlib import Path

pointer = json.loads(Path('artifacts/project-latest.json').read_text())
report = Path(pointer['path']) / 'result.json'
result = json.loads(report.read_text())
print('完整驗收：', report)
print('狀態：', result['status'])
print('範圍：', result['scope'])
print('空 build 起跑：', result['build_started_empty'])
for step in result['steps']:
    print(step['id'], step['status'])
assert pointer['status'] == result['status'] == 'PASS'
assert all(step['status'] == 'PASS' for step in result['steps'])
PY
```

若指標不存在，回建置教學執行 `make verify-project JOBS=2`。
若最近改過 source，先重建並重驗；這段只是讀報告，沒有替新版本重新跑測試。

準備兩個 WSL terminal：A 執行測試，B 讀程式和結果。兩個 terminal 都先 cd 到 repository，
不要在 B 另外啟動一台 VM。接上電源並保留足夠 RAM；正常 guest 使用 1536 MiB，KASAN 使用 2048 MiB。

## 2. DEMO A：四個 VF 與完整回收

### 執行

在 terminal A：

```sh
make demo
```

這個命令直接啟動 stress guest，執行 M1：
檢查 PF/VF capabilities、四個獨立 group/container、真實 MAP/UNMAP，
先 warm-up，再做 100 次 enable/disable。輸出會收進 artifacts，終端暫時安靜是正常的。

### 讀取這一次的結果

等命令返回後，在同一 terminal：

```sh
python3 - <<'PY'
import json
from pathlib import Path

pointer = json.loads(Path('artifacts/m1-latest.json').read_text())
directory = Path(pointer['path'])
result = json.loads((directory / 'result.json').read_text())
print('本次目錄：', directory)
print('整體：', result['status'])
for case in result['cases']:
    print(case['id'], json.dumps(case, ensure_ascii=False))
print('Host 檢查：', result['host_checkpoints'])
print('核心異常：', result['unexpected_kernel_reports'])
print('錯誤：', result.get('error', '無'))
assert pointer['status'] == result['status'] == 'PASS'
PY
```

| 欄位 | 成功時要看什麼 | 如何解釋 |
|---|---|---|
| `M1_CAPS` | 四個 VF、各自 requester、PF/VF capabilities 完整 | PCI function 存在只是第一步 |
| `M1_GROUPS` | 四個 group，各只有對應 VF，PF 不混在其中 | 檢查實際 group 成員 |
| `M1_TYPE1` | 四個同時存在的 container、exact UNMAP、剩餘 fd/maps 為零 | 真正走過 VFIO 操作 |
| M1 報告的 `T01` | 100 cycles、VF/fd/map/ledger 回零 | 這是 M1 enumeration loop；M3 的 T01 另外涵蓋完整 tenant lifecycle |
| `host_checkpoints` | warm-up 後 initial/final inventory hash 一致 | 排除首次 requester cache 後，檢查是否持續成長 |
| `unexpected_kernel_reports` | 空陣列 | 已檢查該次 guest 核心診斷 |

簡短講法：「我不只看 lspci 出現四個 VF；我會打開四個 VFIO container，實際映射、解除映射，
再比對多次 enable/disable 後的裝置與程序資源。」

## 3. DEMO B：有未完成工作的 VF reset

### 執行

```sh
make demo-reset
```

這個入口執行 T07。它先檢查目前 build 對應的 M1/M2 證據，驗 native logical deadlines，
再啟動 KASAN guest。案例讓 VF A 保留一筆 delayed work，執行 reset/reconfigure/rearm，
並確認 VF B 仍持續處理工作。

### 找 aggregate 與真正的 VM 目錄

```sh
python3 - <<'PY'
import json
from pathlib import Path

pointer = json.loads(Path('artifacts/t07-latest.json').read_text())
aggregate_dir = Path(pointer['path'])
aggregate = json.loads((aggregate_dir / 'result.json').read_text())
print('T07 aggregate：', aggregate_dir)
print('狀態：', aggregate['status'])
print('錯誤：', aggregate.get('error', '無'))
assert pointer['status'] == aggregate['status'] == 'PASS'

vm_dir = Path(aggregate['path'])
vm = json.loads((vm_dir / 'result.json').read_text())
print('VM 原始紀錄：', vm_dir)
print('Case：', vm['cases'])
print('核心異常：', vm['unexpected_kernel_reports'])
for filename in ('reset-generations.json', 'healthy-progress.json'):
    print('\n' + filename)
    for row in json.loads((vm_dir / filename).read_text()):
        print(json.dumps(row, ensure_ascii=False))
PY
```

按照 [reset 時序圖](SYSTEM_ANALYSIS_ZH.md#控制時序有未完成請求的-reset) 解釋四個觀察點：

1. **Reset 前**：`pending_at_reset=1`，證明當時真的有未完成工作。
2. **Reset 後、READY 前**：VF A 留在 RECOVERING/fenced；沒有因 timeout 尚未到就提早放行 DMA。
3. **重新配置後**：epoch/ready_gen 符合新 queue，REARM 後才回到可工作狀態。
4. **另一個 VF**：healthy progress 的 completed 增加，session/epoch/lease 維持該 owner 的一致性。

`guard-digests.json` 保存舊、新 SQ/CQ/payload 的檢查；`delayed-callbacks.json` 保存相關 counters。
數字會依執行時序變化，不應背誦某次 PID 或 completed 值。真正的成功條件由 validator 檢查跨紀錄的關係。

報告的 `m3_status=NOT_RUN` 表示單獨 T07 不替整套 M3 驗收，並不是 T07 失敗。
本次 T07 的結果應讀自己的 `status`；完整結果仍在 `project-latest.json`。

## 4. DEMO C：兩個 VF 使用相同 IOVA

時間足夠時執行：

```sh
make verify-t04
```

它在 N=16、32、256 三種 ring size 啟動獨立 VM，各驗兩個 domain。
用以下程式取得每個 variant 的實際目錄：

```sh
python3 - <<'PY'
import json
from pathlib import Path

pointer = json.loads(Path('artifacts/t04-latest.json').read_text())
directory = Path(pointer['path'])
result = json.loads((directory / 'result.json').read_text())
print('T04：', directory, result['status'])
assert pointer['status'] == result['status'] == 'PASS'
for variant in result['variants']:
    print('N =', variant['entries'], 'VM =', variant['path'])
    print(json.dumps(variant['validation'], ensure_ascii=False, indent=2))
PY
```

先在白板畫 A/B 各自的 container/domain，再畫兩條同名的 IOVA `0x400000`。
相同數字透過不同 requester/domain 轉譯，指向不同 owner 的 pages。
報告要同時看到兩邊的實際工作、完整 buffer 比對與 requester 證據。

若要查看「A 存取未映射位址會怎樣」，用 T05 的已保存結果接續說明；
`make verify-t05` 會跑六種 fault，包含 WRITE-only 實驗所需的 toy-only kernel patch。
不要把 T04 的正常隔離測試誤說成所有錯誤權限測試。

## 5. 十分鐘展示安排

| 時間 | 操作 | 要說清楚的問題 |
|---|---|---|
| 0:00–1:30 | [系統指南](SYSTEM_GUIDE_ZH.md) | 為何用簡單 FILL/CRC32 workload 研究隔離與生命週期 |
| 1:30–3:00 | 開架構與 I/O 時序圖 | Windows、WSL/QEMU、guest kernel/userspace 各在哪裡 |
| 3:00–5:00 | 執行 DEMO B，查看結果 | 先 fence/cancel，再撤映射；其他 VF 如何繼續工作 |
| 5:00–7:00 | 點 `owner_workload` 和 `toy_complete` 源碼 | SQ/CQ 索引、generation guard、CQ phase 發布 |
| 7:00–9:00 | 說明 B018 或 B020 | 症狀、錯誤假設、修正、回歸證據 |
| 9:00–10:00 | 展示驗證表與限制 | 測到哪些路徑，哪些結論不能外推到實體硬體 |

先在自己的筆電練一次，記下實際 DEMO 耗時。表中的時間是講解安排，不是 TCG 執行時間保證。
如果時間有限，只跑 T07；其餘展示已保存且版本可核對的結果。

## 6. 現場出錯時的處理

| 情況 | 下一步 |
|---|---|
| `Current-binary M1 prerequisite unavailable` | 檢查 build 是否改過；回到完整驗收入口，不修改 result 讓它通過 |
| 找不到 kernel/initramfs | 先依建置章完成 build；demo 不會自動建立整套依賴 |
| 等很久但終端沒輸出 | 在 B 查看最新 case 的 log 與程序；不要重複啟動 VM |
| 出現 FAIL | 先讀 aggregate 的 error，再追失敗子項目，不只看 make 的 Error 2 |
| QEMU 結束碼是 1，但報告 PASS | 本專案用 isa-debug-exit，guest 成功值 0 會轉為 QEMU exit 1；以 controller 的完整判斷為準 |
| kernel.log 出現 warning/KASAN | 保存該次紀錄，說明中止原因；不能只展示 guest 的 PASS 字串 |
| 中途 Ctrl+C | 保存中斷紀錄，等程序停止後再重跑；沒有完整結果就不算通過 |

測試的正常結束路徑會清理 guest 內的 owner、fd、mapping 和 VF，QEMU 隨後退出。
正常 demo 結束不必執行 clean-build；那會刪除下次展示要用的建置產物。
