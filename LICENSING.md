# 授權範圍

本專案自有的原始碼、測試、建置腳本與文件採 **GPL-2.0-only**，即 GNU General Public License 第 2 版，不含「或更新版本」。完整條款見 [LICENSE](LICENSE)。

## 各目錄

| 內容 | 授權處理 |
|---|---|
| `fwcore/`、`device/`、`guest/`、`include/` | 本專案程式碼採 GPL-2.0-only；既有 PF driver 的 GPL-2.0 標示保留 |
| `tools/`、`tests/`、`spec/`、Makefile | 本專案腳本、測試與規格採 GPL-2.0-only |
| `uefi/SriovFwPkg/` | 本專案 EFI application 原始碼採 GPL-2.0-only |
| `patches/linux/`、`patches/qemu/` | 本專案修改採 GPL-2.0-only；補丁上下文仍保留原檔權利 |
| `uefi/patches/0001-nasm3-x64-push.patch` | EDK2 上游修正的 backport，保留 BSD-2-Clause-Patent 與來源 commit，不改列為本專案自有內容 |
| `docs/`、README、HTML／SVG | 本專案撰寫的文字與圖表採 GPL-2.0-only；引用的上游程式片段依各原檔授權 |
| `LICENSES/`、第三方聲明 | 原授權條款與著作權聲明，依原條款保留 |
| `downloads/`、`build/`、`artifacts/` | 不隨公開原始碼包散布；上游、二進位產物及本機紀錄另有各自內容與義務 |

## 第三方內容

教材 `docs/SOURCE_EVIDENCE.md` 引用 QEMU、Linux、EDK2 的 API 與實作片段。
所用版本固定在 `sources.lock.json`；原檔開頭聲明列於
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，包括上游作者資訊。

- QEMU 整體採 GPL 第 2 版，部分原檔另有授權。教材引用的 `hw/net/igbvf.c` 採 LGPL-2.1-or-later，條款見 [LGPL-2.1](LICENSES/LGPL-2.1.txt)。[QEMU 授權說明](https://www.qemu.org/docs/master/about/license.html)
- Linux 整體採 GPL 第 2 版；引用的 UAPI `vfio.h` 保留 [Linux-syscall-note](LICENSES/Linux-syscall-note.txt)。[Linux 授權規則](https://docs.kernel.org/process/license-rules.html)
- EDK2 本次引用的檔案與 NASM 補丁採 [BSD-2-Clause-Patent](LICENSES/BSD-2-Clause-Patent.txt)。[固定版本的 EDK2 授權](https://github.com/tianocore/edk2/blob/edk2-stable202411/License.txt)
- BusyBox 1.36.1 的 `LICENSE` 明定 GPL 第 2 版；其原始碼與執行檔不在本次公開包內。

這份清單核對的是本次散布的專案內容、補丁與教材引用，不是所有遞迴相依套件的完整授權稽核。
若另外發布 QEMU、guest kernel、initramfs 或 OVMF 二進位檔，需要按實際封裝內容整理相應原始碼、建置方法與授權聲明。
