# 圖表與文件維護

GitHub 閱讀入口是 [README](../README.md)與[閱讀路線](LEARNING_PATH_ZH.md)。
章節中的 `mermaid` fenced blocks 可由 GitHub 直接繪製；基本語法與支援範圍見
[GitHub 官方說明](https://docs.github.com/en/get-started/writing-on-github/working-with-advanced-formatting/creating-diagrams)。
流程圖、資料流、狀態圖和時序圖描述的關係不同；圖後的文字須交代入口、狀態管理者、
箭頭的意義與省略範圍，尤其不能把 MMIO、DMA 或非同步通知畫成普通 C 函式呼叫。

## 一般建置

在 WSL 的 repository 根目錄執行：

```sh
make build-handbook
```

工具將各章與已提交的 SVG 組成 `docs/handbook.html`，更新可點擊的離線原始碼與 hash manifest，
再檢查 HTML 與 Markdown 的相對連結、anchor、原始碼行號及圖數。
一般建置不下載 Mermaid、不啟動瀏覽器，也不需 Node.js。

## 修改 Mermaid 圖之後

圖的來源是各章 Markdown 的 `mermaid` block；`diagrams/mermaid-*.svg` 是離線版本的快取。
名稱包含來源內容的 SHA256 前 16 碼，SVG 內也記錄完整 hash。內容改動後缺少對應 SVG，
`build_handbook.py` 會停止，避免顯示舊圖。

產圖工具只在維護圖表時使用，需要 Python 3 與本機的 Chromium、Chrome 或 Edge。
它以固定的 Mermaid 11.4.1 產生靜態 SVG，並核對下載檔 SHA256；
圖表 API 參考 [Mermaid 官方文件](https://mermaid.js.org/config/usage.html)。
套件下載保存在不提交的 `artifacts/documentation-renderer/`，不把第三方 JavaScript 打包進教材。

Windows PowerShell，位於 repository 根目錄：

```powershell
python tools/render_doc_diagrams.py
```

工具會尋找常見瀏覽器安裝路徑。需要明確指定時：

```powershell
python tools/render_doc_diagrams.py --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
```

若在已安裝 Chromium 的 Linux 上維護，則於相同 repository 根目錄執行：

```sh
python3 tools/render_doc_diagrams.py --browser /usr/bin/chromium
make build-handbook
```

Windows 完成產圖後，切回 WSL 執行 `make build-handbook`。提交變更時同時包含 Markdown、
新 SVG、生成的 handbook 與 manifest；以 `git status --short` 確認實際清單。
圖的文字、函式名稱與 guard 必須先對照原始碼，語法解析成功不能證明技術內容正確。

## 內容核對

每段操作需說明執行層、前置條件、命令、輸出位置、成功判斷與失敗定位入口。
解釋狀態變化時，指出執行者、檢查條件與可見副作用；這個寫法參考
[Google 對明確主詞與精確動詞的說明](https://developers.google.com/tech-writing/one/clear-sentences)。
自測題應先要求讀者預測，再提供程式或結果作為核對依據。

瀏覽器檢查至少包含寬螢幕與窄視窗：文字不能被圖框裁切、箭頭方向必須清楚、表格能水平捲動，
程式片段與來源連結仍可使用。文件檢查與視覺檢查只驗證教材，不改寫專案測試的執行狀態。
