# 8 kHz USB 回報：韌體準備與硬體測試步驟

承接 `LATENCY_NOTES.md`（第 3 節快速路徑、第 4 節 8 kHz 計畫）。**沒有燒過鍵盤**，下面的數字都要靠你（或主 session）在真鍵盤上量。

## 1. 做了什麼

| 項目 | 結果 |
|---|---|
| 描述符 | km_m6h = km_m6p + `KM_USB_HS_8KHZ`：鍵盤 boot 與 NKRO 端點 bInterval 1（125 µs），wMaxPacketSize 不變，bcdDevice 0x0003。原本 `test_usb.c` 的描述符測試（端點位址、封包大小、HS interrupt interval 1..16、FNV 雜湊）在旗標開／關兩種設定都 0 failures（開的那次是暫時改標頭，已還原）。 |
| 快速路徑 | **「按鍵變化後同一個 pass 內就送進端點」本來就成立**：`pump_reports` 在掃描 frame 之後、燈光/stream/動畫之前就呼叫 `km_report_sync_pump`（見 LATENCY_NOTES 2.1）。真正缺的是三件事，全部做在 `KM_M6_FASTPATH=1`（只有 km_m6h）：(a) 慢工作（stream、動畫、燈光）在「剛送出/還欠一個鍵盤報告」的 pass 跳過，最多延後 1 ms，之後強制執行（`km_defer_slow_ok`，不會餓死）；(b) 端點當時忙碌時，報告不用等到下一個 pass：在每個慢階段之間再 poll 一次 USB 並重送（`retry_keys`）；(c) 已抹除的燈光儲存（`m5_led_persist_urgent`）不再在按著鍵時動 flash，除非鍵一直按住超過 2 秒沒有空檔。 |
| 端點重新上膛 | 驅動是輪詢式（沒有 ISR）：傳輸完成要等主迴圈的 `km_usbhs_poll()` 看到 XFRC 才釋放端點。一個端點同時只能有一包在飛（DWC2 的 DIEPTSIZ 單封包），所以 8 kHz 下「上一包被主機取走 → 下一包能進端點」的延遲＝主迴圈 pass 長度（約 40–46 µs，遠小於 125 µs）。報告本身是「永遠送目前狀態」（不排佇列），所以不會送出過期資料；代價是：125 µs 內按下又放開，中間狀態會被合併（計入 `coalesced`）。 |
| 計數器 | `km_report_sync` 新增 `deferred / coalesced / late / wait_last / wait_max`（8 kHz 以外的映像也有，只是多幾個計數）。新純邏輯模組 `km_usbfast`：`km_defer`（延後慢工作）、`km_ustat`（端點忙碌時間分桶）。 |
| 測試 | `tests/test_usbfast.c`（延後的上限與 wrap、分桶邊界、sync 計數器各情境）。`python firmware/tools/run_tests.py`：188624885 checks, **0 failures**。 |
| ARM 建置 | `check_image.py` 全過：km_m6a 58480 B（44.6%）、km_m6p 59248 B、**km_m6h 59824 B（45.6%）**。km_m6a 的主迴圈程式碼沒動（`KM_M6_FASTPATH` 預設 0）；只因為共用的 report_sync 多了計數欄位，位元組不同於合併前。 |

## 2. 新的 scope tag（km_m6p 與 km_m6h 都有；以 400 ms 輪流發佈）

以 `python tools/magger60_scope.py --run --tag 0x0601` 讀取。

**tag 0x0601：報告佔用 NKRO 端點的時間**（送進端點 → 主迴圈看到端點又空了）

| 索引 | 意義 |
|---|---|
| [0] | 量測次數（低 16 位） |
| [1]…[6] | 分桶次數：≤62 µs、≤125、≤250、≤500、≤1000、>1000 µs |
| [7] | 最大值（µs） |

**tag 0x0602：送達計數器**

| 索引 | 意義 |
|---|---|
| [0] | late：報告從「該送」到「被端點收下」超過 125 µs 的次數 |
| [1] | deferred：該送時端點還被上一包佔著的次數 |
| [2] | coalesced：等端點時狀態又變了，中間狀態沒送出的次數（快按快放） |
| [3] | NKRO 最大等待（µs） |
| [4] | boot 鍵盤最大等待（µs） |
| [5] | 慢工作被延後的 pass 數（km_m6p 恆為 0） |
| [6] | 在慢階段之間重試而送出的報告數（km_m6p 恆為 0） |
| [7] | 超過 9300 cycles 的 pass 數（同 0x0500[3]） |

tag 0x0600 照舊（引擎看到按下 → 報告進端點的 µs，[3] 為最大值）。

## 3. 硬體測試步驟（由主 session 燒錄，使用者同意後）

燒錄前後都要套 `local/apply_profile.sh`。**復原**：拔 USB，按住 ESC + 左 CTRL 插入進 bootloader，燒回 km_l51 或 km_m6a。

1. **基準（1 kHz）**：燒 km_m6p（已知能用）。打 30 秒（連打四個鍵），讀 tag 0x0601、0x0602、0x0600、0x0100，存下來。
2. **8 kHz**：用 `python firmware/tools/build_crashtest.py --target km_m6h --build-dir <dir>` 建出映像，燒入。先確認：能打字；`magger60_info.py` 的 bcdDevice = 0x0003；scope 讀得到。只要不穩（掉鍵、卡住、認不到）就照復原步驟退回並**停止回報**。
3. 同樣打 30 秒，讀同樣的 tag。

**成功的數字（km_m6h 對 km_m6p）**
* 0x0601：km_m6p 的桶子會散布到 ≤250 / ≤500 / ≤1000；km_m6h 應該集中在前兩桶（≤125 µs），>250 µs 的桶接近 0，最大值 [7] 約 ≤ 250。這是「8 kHz 真的在跑」的最直接證據（量的是主迴圈看到的時間，下限約是一個 pass，40–46 µs）。如果 km_m6h 的分佈跟 km_m6p 一樣散到 1 ms，代表主機沒有以 8 kHz poll（或控制器沒照 bInterval 排程）。
* 0x0602：late、coalesced 應為 0 或極少；[3] 最大等待 < 250 µs；[7] 與 km_m6p 相比不增加。
* 0x0100[5] overruns = 0，[14] frame 週期仍約 9.4k cycles。
* 0x0600[3]：km_m6h 不應比 km_m6p 差。

**失敗訊號**：late 持續增加、0x0601 有大量 >1000 µs（端點長時間沒被主機取走，可能是 Windows/控制器不支援該間隔或 FIFO 問題）、coalesced 很多（正常打字不該有）、打字掉鍵。

## 4. 風險與限制

* **N32H48x USB 核心在 bInterval 1 下沒有實測**；`usb_hs.c` 沒有 interval 相關暫存器。`in_start` 先 EPENA 再寫 FIFO，在 125 µs 輪詢下主機可能在兩者之間來問（回 NAK，之後再取），預期無害，但這是第一個要在硬體看的點。
* 輪詢式驅動：pass 若因 flash 抹除變成毫秒級，那段時間端點無法釋放；這是已知現象，非本次引入。
* HID idle 重送用「每次 poll 看到 SOF 就 +1」數 micro-frame，8 kHz 下若某個 pass >125 µs 會少算，只讓 idle 重送略慢，不影響按鍵。
* `retry_keys` 讓 pass 內多 poll 1–3 次 USB（只在有報告欠著時）；pass 變長量要看 0x0500 與 0x0602[7]。
* Windows 遊戲多半以約 1 kHz 讀取輸入，8 kHz 主要縮短平均「端點等主機」那段（約 0.5 ms → 約 0.06 ms）；主機實際收到的時間無法由韌體證明，需要 USBPcap 或 USB 分析儀（LATENCY_NOTES 4.5）。
* 延後慢工作會讓燈光在連打時略不順（上限 1 ms 的延後，通常看不出來）。

---

# 第二輪：為什麼主機還是 1 kHz poll？(硬體結果之後)

## A. 實測回報（主 session）
km_m6p（bInterval 4）與 km_m6h（bInterval 1）的 0x0601 分佈幾乎一樣，都散到 1 ms → 主機以約 1 kHz 在取報告。0x0600 最大值 km_m6h 546 µs 對 km_m6p 31 µs。

## B. 讀程式碼的結論（沒有量測）
1. **描述符**：`GET_DESCRIPTOR(config)` 只有一份設定（`km_usb_ctrl.c`），不分速度；沒有 other-speed 描述符（請求會 STALL，對只有高速的裝置是允許的），device qualifier 有（bcdUSB 0x0200）。所以**沒有「另一份 FS 描述符」可以搶走**；bInterval 1 的位元組就是主機讀到的（主機測試 `km_usb_cfg_binterval` 從實際供應的位元組讀回）。
2. **速度**：驅動 `DCFG` 設高速，但**從來沒有讀過協商結果**。若鍵盤其實以全速列舉，bInterval 在全速的單位是 ms：bInterval 4 = 4 ms、1 = 1 ms。0x0601 的結果（km_m6p 約 1 ms）說明 km_m6p 在高速（否則會是 4 ms），所以更可能是「高速，但主機給 1 ms」。這點要用新的 0x0604 直接確認。
3. **裝置端不限制**：DWC2 核心對週期性 IN 端點不會自己用 bInterval 擋 IN token（它只回 NAK 或資料），端點一武裝，下一個 IN token 就拿到。所以如果主機 125 µs 來問，裝置會在 ≤125 µs 內交出。
4. **0x0600 最大 546 µs 的解釋**：0x0600 量到的是「引擎看到按下 → 報告被端點收下」，若上一包（例如前一個鍵的放開）還佔著端點，要等主機取走才能送；1 kHz 輪詢時這個等待可到 ~1 ms。它跟慢工作延後無關，是同一個 1 kHz 問題；km_m6p 的 31 µs 只是樣本沒碰到。延後本身沒有任何收益（0x0602 重試＝0），所以**已關掉**：`KM_M6_DEFER`、`KM_M6_RETRY` 預設 0，km_m6h 只保留「按著鍵不動 flash」的規則與計數器。

## C. Windows / 主機端可能的上限（公開知識，未在這台機器驗證）
* 高速裝置的 interrupt 端點 bInterval 1 = 125 µs，Windows 的 xHCI 驅動（usbxhci）與 EHCI 驅動理論上都支援；市面上 8 kHz 滑鼠/鍵盤（走高速）在 Windows 上就是這樣運作。
* 可能的上限：(1) 裝置實際是全速列舉（單位變 ms）；(2) 裝置接在**全速／USB 1.1 的 hub 之後**，或 hub 的 Transaction Translator 把它降成 1 kHz；(3) 主機控制器是 xHCI，且裝置經由 USB3 hub 的 USB2 路徑，排程仍應是 125 µs，但某些 hub/主機板韌體會量化；(4) Windows 對**已快取過的裝置實例**不重讀描述符（bcdDevice 已改為 0x0003，所以應重讀，但可用裝置管理員解除安裝後重插確認）；(5) 鍵盤類別驅動（kbdhid/hidusb）只決定怎麼讀資料，不改匯流排排程。
* 用 Windows 看實際值：USB Tree View（Uwe Sieber，免安裝）會顯示每個端點的 bInterval、裝置速度（High-speed / Full-speed）、接在哪個 hub／控制器。**請把鍵盤直接插主機板後面的 USB 埠（不經 hub、也不要插 USB-C 轉接座）**再量一次。

## D. 新的診斷（km_m6p 與 km_m6h 都有，只讀不改行為）
讀法：`python tools/magger60_scope.py --run --tag 0x0603`、`--tag 0x0604`。六個 tag 輪流發佈，每個每 600 ms 更新一次。

**tag 0x0604（連線狀態）**
| 索引 | 意義 |
|---|---|
| [0] | 列舉速度 DSTS.ENUMSPD：**0 = 高速**，1 或 3 = 全速，0xFF = 尚未 |
| [1][2] | 韌體數到的 SOF 次數（低 16 位、高 16 位） |
| [3][4] | 同一時刻的 CPU 週期計數器（低、高 16 位） |
| [5] | 實際供應的 boot 鍵盤端點 bInterval（m6h 應為 1，m6p 為 4） |
| [6] | 實際供應的 NKRO 端點 bInterval |
| [7] | DSTS 原始值低 16 位 |

SOF 速率：讀兩次，Δ SOF ÷ Δ 週期 × 240,000,000。**高速約 8000 /s，全速約 1000 /s。**（pass 超過 125 µs 時會少數，所以略低於 8000 屬正常；遠低於 7000 要懷疑。）

**tag 0x0603（微幀直方圖）**：報告進端點 → 主迴圈看到端點變空，經過幾個微幀。[0]…[5] = 0、1、2、3–4、5–7、8 個以上微幀的次數；[6] 總數；[7] 最大微幀數。
* 主機 125 µs 輪詢：幾乎全在 [0][1]（0～1 微幀），[3]以後接近 0。
* 主機 1 ms 輪詢：均勻散在 0～7 微幀，[5]（8+）接近 0。
* 這個量測的解析度是一個 pass（約 40 µs），比一個微幀細，所以足以分辨。

## E. 硬體步驟（不需要改映像也能先做 1–2）
1. **先不燒**：在 Windows 開 USB Tree View，看鍵盤現在（km_m6h）的速度、兩個鍵盤端點的 bInterval、它接在哪個 hub／控制器。確認 bInterval 欄位真的是 1。
2. 把鍵盤直接接主機板後方 USB 埠（避開 hub），裝置管理員解除安裝 Magger60 的所有項目並重插。
3. 燒新的 km_m6h（本分支重建），連打 30 秒，讀 0x0604（速度、SOF 速率、bInterval）與 0x0603。
4. 判讀：
   * 速度≠0 → 全速列舉，是 PHY／線材／埠的問題，先解決這個。
   * 速度=0、SOF 約 8000/s、bInterval=1，但 0x0603 仍均勻散開 → 主機（控制器或 hub）把它排成 1 ms，錯不在韌體；換埠／不同控制器（例如主機板 vs. 擴充卡）再試。
   * 0x0603 集中在 0～1 微幀 → 8 kHz 成功，再看 0x0602 的 late。
5. 還需要確認的話：USBPcap + Wireshark 可看主機端 URB 完成的時間戳。

## F. 變更
* `KM_M6_DEFER`／`KM_M6_RETRY` 預設關閉（km_m6h 現在不延後慢工作、不在 pass 內重試）。
* km_m6h 保留：描述符、flash 規則、全部計數器與新 tag。

---

# 第三輪：根因 — 8 kHz 描述符從沒進過映像

硬體回報（0x0604：bInterval 4/4）證實：`KM_USB_HS_8KHZ=1` 只加在 `km_m6h.elf` 的編譯參數，但描述符在**共用的靜態函式庫 `km_usb`**（`km_usb_desc.c`）裡編譯，函式庫從沒看到這個 define，所以前兩輪燒的 km_m6h 其實是 bInterval 4、bcdDevice 1。主機測試之所以過，是因為測試全域定義了旗標。**所以「主機 1 ms poll」不是主機的問題，是我們沒送出 bInterval 1。** 第二輪的「Windows 上限」推測作廢（0x0604 的速度＝高速、SOF 約 8000/s 仍是有用的事實）。

修正：
* `CMakeLists.txt`：新增函式庫 `km_usb_8k`（同樣三個來源檔，`KM_USB_HS_8KHZ=1` 設為 PUBLIC，所以連結它的映像的 `m6_main.c`/`usb_hs.c` 也看到同一個值）；km_m6h 改連 `km_usb_8k`。其他映像仍連 `km_usb`。km_core／km_hid 不含 km_usb_ids.h，不受影響；沒有任何物件檔會同時看到兩個值。
* `tools/check_image.py` 新增 `--usb-8khz` / `--usb-1khz`：從建好的 .bin 讀出裝置描述符的 bcdDevice 與 EP 0x81/0x83/0x82 的 bInterval，不符就 FAIL；`build_crashtest.py` 對 km_l51/m6a/m6/m6p 自動用 `--usb-1khz`，對 km_m6h 用 `--usb-8khz`。我已反向驗證（m6p 用 --usb-8khz、m6h 用 --usb-1khz 都會 FAIL）。
* 建置結果（皆通過）：km_l51 / km_m6a / km_m6 / km_m6p：bcdDevice 0x0001(0x0002)、bInterval 4/4/4；**km_m6h：bcdDevice 0x0003(0x0004)、bInterval kbd 1、nkro 1、vendor 4**。位元組直接在 .bin 內看到：EP 0x81 `07058103 08 00 01`、EP 0x83 `07058303 40 00 01`。
* 大小：km_m6a 58484 B、km_m6p 59632 B、km_m6h 59760 B。

燒入後預期：0x0604[5][6] = 1、1；裝置 bcdDevice 0x0003（Windows 會當新裝置）；0x0603 若主機真的 125 µs poll 會集中在 0～1 微幀。復原步驟同前。
