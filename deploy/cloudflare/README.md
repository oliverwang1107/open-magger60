# 把設定介面放上 Cloudflare

這個資料夾把 `ui/`(Fluxline 設定介面)用 **Cloudflare Workers 的靜態檔案功能**放到網路上。
沒有 Worker 程式、沒有資料庫、沒有金鑰;只有「檔案 + 一份標頭設定」。

為什麼要放上網?WebHID(瀏覽器直接跟鍵盤說話)需要 https 或 localhost。放上 Cloudflare 之後,你在任何一台電腦的
Chrome / Edge 打開網址就能用,不用先 `python serve.py`。**鍵盤的資料仍然只在你的瀏覽器和鍵盤之間走**,
網站只是把網頁檔案送給瀏覽器,看不到任何人的設定。

## 目前的狀態:公開(擁有者的選擇)

`wrangler.jsonc` 的 `"workers_dev": true`:任何人知道網址都能打開頁面,**不需要登入**。這是擁有者明確決定的
(「不需要限制存取」)。這樣做的意義:

- 頁面只是靜態檔案;它**不會**自己連任何人的鍵盤,要使用者在自己的瀏覽器按「連接鍵盤」並選裝置。
- 設定、設定檔、燈光都存在**造訪者自己的瀏覽器**(localStorage)和鍵盤裡,伺服器端什麼都沒有。
- 上傳到 Cloudflare 的只有 `ui/` 裡瀏覽器需要的檔案(`index.html`、`css/`、`js/`、`wasm/`),**不含**測試、README、
  `serve.py`、`local/` 資料夾或個人設定檔。`deploy.mjs` 用白名單複製,不是整個資料夾。
- 網址的子網域是你的 Cloudflare 帳號的 workers.dev 子網域;帳號 id、token 都**不在**任何提交的檔案裡。
- 專案的發布決定(專案名稱、身分、是否公開原始碼)仍照 `PUBLISH_DECISIONS.md`;這個網站公開不代表原始碼公開。

## 檔案說明

| 檔案 | 作用 |
|---|---|
| `wrangler.jsonc` | Worker 設定:名稱、靜態檔案資料夾 `./dist`、`workers_dev`(沒有帳號 id、沒有密碼) |
| `_headers.template` | 安全標頭(CSP、Permissions-Policy、禁止被嵌入等);部署時複製成 `dist/_headers` |
| `deploy.mjs` | 部署腳本:把 `ui/` 裡瀏覽器需要的檔案複製到 `dist/`,再呼叫 wrangler;`--check <網址>` 檢查回應 |
| `package.json` / `package-lock.json` | 只裝 wrangler(Cloudflare 的命令列工具),版本鎖定 |

## 第一次部署(約 5 分鐘)

需要:Node 20 以上、一個 Cloudflare 帳號(免費即可)。在專案根目錄:

```bash
cd deploy/cloudflare
npm ci                      # 安裝 wrangler(版本鎖定)
npx wrangler login          # 瀏覽器會跳出 Cloudflare 授權頁,按「Allow」;已登入可略過(npx wrangler whoami 可確認)
node deploy.mjs --dry-run   # 先演練,不會上傳
node deploy.mjs             # 真的部署;結果會印出網址 https://<名稱>.<你的子網域>.workers.dev
node deploy.mjs --check https://<名稱>.<你的子網域>.workers.dev/    # 預期 200
```

別人要部署自己的一份:先改 `wrangler.jsonc` 的 `name`(Worker 名稱在你的帳號裡要唯一)。

## 日常:更新

改了 `ui/` 之後(wasm 要用 `python tools/build_wasm.py` 重建,見 `firmware/wasm/WASM_NOTES.md`):

```bash
cd deploy/cloudflare
node deploy.mjs
```

瀏覽器每次都會問伺服器檔案有沒有變(`Cache-Control: max-age=0, must-revalidate` + ETag,沒變時只回 304),
所以新版本部署後重新整理就會生效,不會卡在舊的 ES 模組。

## 回滾(上一版有問題時)

```bash
cd deploy/cloudflare
npx wrangler deployments list     # 看部署紀錄
npx wrangler rollback             # 退回上一版;或 npx wrangler rollback <VERSION_ID>
```

也可以直接把 `ui/` 用 git 還原到好的版本再 `node deploy.mjs`。

## 安全標頭(`_headers.template`)

| 標頭 | 目的 |
|---|---|
| `Content-Security-Policy` | 只允許本站的腳本/樣式/圖片(`script-src 'self' 'wasm-unsafe-eval'`:後者是載入 WebAssembly 用),不允許任何外部資源、不允許內嵌腳本,不能被嵌進別人的頁面 |
| `Permissions-Policy: hid=(self)` | 只有本站可用 WebHID;USB、序列埠、相機、麥克風、定位都關掉 |
| `X-Frame-Options: DENY` | 不能被 iframe 嵌入(防點擊劫持) |
| `X-Content-Type-Options: nosniff` | 瀏覽器必須照伺服器給的 MIME 型別處理檔案(`.js`=`text/javascript`、`.css`、`.wasm` 由副檔名自動設定) |
| `Referrer-Policy: no-referrer` | 不外洩你從哪個網址來 |
| `Cross-Origin-Opener-Policy` / `Cross-Origin-Resource-Policy` | 隔離視窗與資源,不被別的網站讀取 |
| `X-Robots-Tag: noindex…` | 不要被搜尋引擎收錄(網站公開但不主動曝光) |

標頭只套在靜態檔案上(Cloudflare 的設計);我們沒有 Worker 程式,所以不需要另外處理。
若之後 UI 要載入外部資源(字型、CDN),必須同時改 `_headers.template` 的 CSP,否則瀏覽器會擋。

## 想再鎖起來(選用):Cloudflare Access

如果之後想改成只有自己能開:

1. 打開 <https://dash.cloudflare.com> → **Workers & Pages** → 點你的 Worker → **Access** 分頁 → **Protect this Worker behind Access**
   (第一次會先要你建立 Zero Trust 組織:取團隊名稱、選 **Free** 方案;可能要求綁付款方式,不會扣款)。
2. 範圍選 **All traffic**;**Authentication policy** 選 **Cloudflare account**(只有你帳號的成員),登入方式用預設的 One-time PIN(信箱收驗證碼)。
3. **Apply Access**。再到 Zero Trust → Access controls → Applications 確認 Policies 裡**只有你**,沒有 `Everyone` / `Bypass`。
4. 驗證:`node deploy.mjs --check <網址>` 應該是 `302`(導向登入),無痕視窗會先看到 Cloudflare 登入頁。

想連網址都關掉:把 `wrangler.jsonc` 的 `workers_dev` 改成 `false` 再 `node deploy.mjs`(沒有任何公開網址,404/1042)。
注意:儀表板手動開關 workers.dev 會在下次部署被 `wrangler.jsonc` 覆蓋,以檔案為準。

## 想整個拆掉

```bash
cd deploy/cloudflare
npx wrangler delete
```

## 疑難排解

| 現象 | 原因 / 處理 |
|---|---|
| `--check` 回 `404` | `workers_dev` 是 `false`(關閉中),或網址打錯 |
| 瀏覽器只顯示空白、主控台出現 `Refused to ...` | CSP 擋了某個資源;看錯誤訊息,必要時改 `_headers.template` |
| 連接鍵盤的按鈕沒有反應 | 要用桌面版 Chrome / Edge;網址必須是 https(workers.dev 是);WebHID 在 Firefox / Safari 沒有 |
| 改了 UI 但網站還是舊的 | 先確認 `node deploy.mjs` 成功;再按 Ctrl+Shift+R 強制重新整理 |
| `wrangler` 說沒登入 | `npx wrangler login` |

## 不要提交的東西

帳號 id、API token、你的 workers.dev 子網域、Access 的 AUD 都**不要**寫進檔案或提交。
`wrangler.jsonc` 裡只有 Worker 名稱(不是機密);`dist/`、`node_modules/`、`.wrangler/` 已被 `.gitignore` 忽略。
