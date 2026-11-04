# Historical unimplemented M5 design

Not part of the M0-M3 release.

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

payloadCRC 與 headerCRC 只是非惡意完整性檢查；hash 可供發布 manifest 用 SHA256，但 manifesthash 不等於裝置驗簽。拒絕較舊 fw_version 是模型 policy，若 host 可改 storage 並無可信 antirollbackroot。M5 安全限制必須在 README 與系統說明一致。

### 持久化位址與 metadata

| 範圍 | 內容 |
| --- | --- |
| 0x0000..0x0fff / 0x1000..0x1fff | Journal copy 0 / 1，各 4KiB |
| 0x2000..0x9fff / 0xa000..0x11fff | Slot A / B，各 32KiB，capsule header 包含在 slot 內 |
| 0x12000..0x1ffff | Reserved；固定檔案總長度 0x20000（128KiB） |
| Journal record bytes 0..63 | magic[4]=TJNL、format u16=1、header_len u16=64、generation u64、active_slot u8、last_good u8、pending_slot u8（0xff=none）、state u8、reason u32、slotA_len u32、slotA_crc u32、slotB_len u32、slotB_crc u32、record_crc u32、reserved[20]；剩餘 block 零。record_crc 欄位計算時置 0，計算完整 4KiB。 |
| State / reason | state 0 FACTORY、1 CONFIRMED、2 PENDING、3 ROLLED_BACK；reason 0 NONE、1 BAD_CRC、2 BAD_ABI、3 HEALTH_FAIL、4 UNCONFIRMED_REBOOT、5 TORN_WRITE。enum 未知拒絕 recover。 |

Capsule header byte offsets：magic 0x00、header_len 0x04、format 0x06、image_len 0x08、fw_version 0x0c、min_abi 0x10、max_abi 0x12、flags 0x14、payload_crc32 0x18、header_crc32 0x1c、reserved 0x20..0x3f。header_crc32 計算完整 64 bytes 時只將 header_crc32 本身置零，payload_crc32 保留；payload_crc32 單獨對 payload 計算。這條精確規則優先於「CRC 欄位置0」的簡稱，測試必須提供正負向固定 capsule 向量。
