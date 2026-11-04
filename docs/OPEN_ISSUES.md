# 已知問題與驗證範圍

2026-11-05 從空 build 完成的 M0–M3／T01–T10 驗證通過；受測輸入與結果見 [VALIDATION](VALIDATION.md)。
該次測試不涵蓋以下列出的範圍，也不能證明不存在其他問題。
B017–B020 已修正並完成回歸；受測版本、原始結果及雜湊核對見 [VALIDATION](VALIDATION.md)。

適用環境為 WSL2 Ubuntu 26.04、QEMU TCG、x86_64 guest 與指定 kernel patches。
M4/M5、KCSAN、KVM、實體板卡及 firmware update 不列入本版功能，範圍見 [REQUIREMENTS](REQUIREMENTS.md)。

以下是結果的適用條件：

- 2026-10-18 的完整 PASS 使用增量建置；2026-11-05 另有一輪從空 build 完成的完整結果。
- T05 WRITE-only read rejection 依賴 toy-only guest kernel patch，不能視為 stock Linux 的保證。
- Native 32-bit UAPI layout 檢查不代表 32-bit guest runtime；KASAN 也不能替代未執行的 race/hot-remove 全矩陣。

修改來源、patch 或工具鏈後，須重新執行受影響測試。若發現新問題，依 [BUG_LOG](BUG_LOG.md)
保存重現步驟、原始失敗、根因和回歸結果。
