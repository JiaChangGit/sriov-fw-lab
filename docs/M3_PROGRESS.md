# M3 測試索引

M3 在 2026-10-18 完成整套驗收。下表列出同一輪 aggregate 內的結果位置；
原始紀錄保留在本機，公開摘要見 [verification-summary.json](verification-summary.json)。完整版本與建置方式見 [VALIDATION](VALIDATION.md)。

| 案例 | 驗證內容 | 結果 |
|---|---|---|
| T01 | 四 VF 反覆啟停與完整 tenant 生命週期 | PASS（本機紀錄：`artifacts/t01/20261017T180116Z-cnhc14_e/result.json`） |
| T02 | Provision、PCI enable 與 owner 建立失敗的回收 | PASS（本機紀錄：`artifacts/t02/20261017T180705Z-fdi5uz_9/result.json`） |
| T03 | FILL、CRC32 與 ring wrap | PASS（本機紀錄：`artifacts/t03/20261017T181006Z-rav3v2ez/result.json`） |
| T04 | 獨立 VF domain 使用相同 IOVA | PASS（本機紀錄：`artifacts/t04/20261017T181054Z-tory7w20/result.json`） |
| T05 | 未映射位址、guard page 與 DMA 權限 | PASS（本機紀錄：`artifacts/t05/20261017T181144Z-s3smxqb4/result.json`） |
| T06 | VFIO reset、close/open FLR 與重新配置 | PASS（本機紀錄：`artifacts/t06/20261017T181320Z-090av71h/result.json`） |
| T07 | 有未完成工作時 reset，等待新 READY | PASS（本機紀錄：`artifacts/t07/20261017T181339Z-iaw1h_of/result.json`） |
| T08 | 舊 completion callback 在 reset 後執行 | PASS（本機紀錄：`artifacts/t08/20261017T181357Z-d37_5_vk/result.json`） |
| T09 | Stale 與格式錯誤的 descriptor | PASS（本機紀錄：`artifacts/t09/20261017T181415Z-awz6to9y/result.json`） |
| T10 | 有效 lease 阻止 disable 與 resize | PASS（本機紀錄：`artifacts/m3-case/20261017T181432Z-h5yxehkr/result.json`） |

M3 執行前亦核對 M0、stress/KASAN M1 與三條 M2 boot path。
42 項 M3 controller/validator 測試同時檢查正常證據及遭破壞的反例。

- 系統設計與不變條件：[系統分析](SYSTEM_ANALYSIS_ZH.md)
- 操作與結果判讀：[DEMO 手冊](DEMO_ZH.md)
- 失敗原因與回歸：[BUG_LOG](BUG_LOG.md)

單獨案例的 PASS 只代表該案例；整套驗收以 M3 aggregate（本機紀錄：`artifacts/m3/20261017T175140Z-q1s8vqzd/result.json`） 為準。
