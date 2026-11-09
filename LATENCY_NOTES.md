# M6 延遲調查（km_m6a 在節奏遊戲裡「卡卡的」，km_l51 很順）

本檔分四節，每節一個 commit：1 量測計畫、2 原因排名、3 快速路徑設計、4 8 kHz 描述符計畫。
白話前提：**我（寫這份的 session）沒有碰鍵盤**，這裡的數字都是「讀程式碼推出來的預期值」，要靠你在真鍵盤上量到的數字來證實或推翻。

---

## 1. 量測計畫

### 1.1 要回答的問題

按下去到電腦收到按鍵，中間有四段：

| 段 | 內容 | 誰能量 |
|---|---|---|
| A | 實體按下 → 韌體引擎判定「按下了」（掃描 frame、16 點平均、去抖 8 frame） | 看 scope 的 frame 週期（tag 0x0100）；兩個映像用同一套 km_scan / km_engine，理論上一樣 |
| B | 引擎判定 → 報告交給 USB 端點（主迴圈剩下的工作、組報告、km_report_sync） | **新增的探針 tag 0x0600**（只有 km_m6p） |
| C | 端點等主機來 poll（bInterval 4 = 1 ms，最多等 1 ms） | 兩個映像一樣；看第 4 節的 8 kHz 才會變 |
| D | 主機/遊戲自己的處理 | 兩個映像一樣，不是韌體的事 |

M6 比 L51 多的工作都在 B 段與「每個 pass 的長度」，所以探針量 B，再用現有計數器看 pass 長度與 frame 是否被拖長。

### 1.2 韌體端新增的計數器（不改行為，預設不編進去）

* 新純邏輯模組 `firmware/core/include/km/km_latency.h` / `src/km_latency.c`：每個 pass 看引擎的按下清單，新按下的鍵記一個時間戳（cycles）；NKRO 報告送出時，用「最舊的等待中按下」算出 cycles，記錄 最後一次 / 最小 / 最大 / 次數。環形佇列 8 筆，滿了算 `dropped`。主機測試：`firmware/tests/test_latency.c`。
* `m6_main.c` 內以 `KM_M6_LAT_PROBE`（預設 0）包起來的幾行：`pump_reports` 裡呼叫 `km_lat_observe` / `km_lat_sent`，並在每 200 ms 把 tag **0x0600** 發出去（取代那一輪的 flash 診斷 tag 0x0203）。
* 新映像 **`km_m6p`** = km_m6a + 探針（USB 描述符與 km_m6a / km_l51 完全相同，所以不需要新的復原程序；USB 行為沒變）。`km_m6a` 本身逐位元組沒有被動到（flag 預設關）。
* 建置檢查：`km_m6a`、`km_m6p` 都已用 arm-none-eabi 編過、`check_image.py` 通過（沒有燒）。

Tag 0x0600 的 8 個值（微秒，上限 65535）：

| 索引 | 意義 |
|---|---|
| [0] | 樣本數（有按下被報告回應的次數） |
| [1] | 最後一次：引擎看到按下 → 報告交給端點 |
| [2] | 最小 |
| [3] | 最大 |
| [4] | 引擎看到的新按下次數 |
| [5] | 丟掉的（環形滿） |
| [6] | NKRO 報告送出總數（低 16 位） |
| [7] | 目前還在等報告的按下數 |

現有、不用改的計數器（km_m6a 與 km_l51 都有）：

* tag **0x0100**：[2][3] frame 總數、[4] DMA 錯誤、[5] **overruns**、[12] 最近 frame cycles、[13] 最小、[14] **最大**。
* tag **0x0500**（只有 M6）：[0] pump_reports 最近一次 cycles、[1] 最大、[2] **整個 pass 最長 cycles**、[3] **超過 9300 cycles 的 pass 數**。

### 1.3 主機端怎麼讀（不用新工具）

既有的 `tools/magger60_scope.py` 就能讀任何 tag（`--tag` 參數，`--csv` 記錄）：

```bash
python tools/magger60_scope.py --run --tag 0x0600              # 看一次探針
python tools/magger60_scope.py --run --tag 0x0600 --csv lat.csv --seconds 60 --interval 0.05
python tools/magger60_scope.py --run --tag 0x0100              # frame 週期、overruns
python tools/magger60_scope.py --run --tag 0x0500              # pass 長度（只有 M6 映像有）
```

探針 tag 每 200 ms 才更新一次，所以 `--interval 0.05` 讀到的是相同值重複幾次，沒關係；累積的 min/max 才是重點。

### 1.4 要比的兩個映像與量測步驟（由你執行）

燒錄前一律先用 `local/apply_profile.sh` 的流程套你的設定（記憶裡的規則）。兩個映像用**同一份** `backup10/settings_0x08028000.bin` 建。

1. **km_l51**：燒入 → 開 `magger60_scope.py --run --tag 0x0100`，重插後打 30 秒（用你平常的節奏遊戲按法，或連打四個鍵），記下 [5] overruns、[12][13][14] frame cycles。
2. **km_m6p**（M6 + 探針）：同樣打 30 秒，記下 tag 0x0100 同樣欄位，再讀 tag 0x0500 的 [1][2][3]、tag 0x0600 的 [0][1][2][3][5]。
3. 另外在 km_m6p 上**什麼都不按**放 10 秒，再讀一次 0x0500 [2]（燈光/動畫單獨造成的最長 pass）。
4. 最後（可選）把遊戲裡最容易卡的段落錄下來，用高速攝影或外接測試器量 A+B+C 的全程（見 4.5）。

### 1.5 預期數字（讀程式碼推的，等量測證實）

* CPU 240 MHz，1 ms = 240000 cycles；一個掃描 frame ≈ 9.3k–10.4k cycles ≈ 40 µs。
* 探針 [1]/[2]（B 段）：正常應該是 **一個 pass 以內，約 10–60 µs**；最大值偶爾到 100–200 µs 也算正常（燈光 slice）。
* **若 [3] 最大值出現 ≥ 1000 µs（1 ms）**：代表有某個工作把主迴圈卡住毫秒級 → 這才是「卡」的量級（flash 抹除、tap-hold 排隊等，見第 2 節）。
* 0x0500 [3]（超過 9300 cycles 的 pass）：L51 沒有這項；M6 在只按鍵時預期很少，打遊戲 + 燈光動畫時可能每秒數十次。
* 0x0100 [5] overruns：兩個映像都預期 0；M6 若不為 0 就是 pass 太長弄丟 frame。
* 若探針都在幾十 µs、frame 週期兩邊一樣、overruns 皆 0：**M6 的延遲不在韌體主迴圈**，要往「設定被覆寫」（第 2 節 #1）與主機端找。

---

## 2. 原因分析（只讀程式碼；沒有量測，所以是「排名 + 怎麼證實」）

### 2.1 主迴圈 m5_main.c（km_l51）與 m6_main.c（km_m6a）的差別

兩者的 pass 順序**一樣**：USB poll → 掃描 frame（有就吃，跑 engine）→ `pump_reports` → 其餘（燈光等）。
掃描路徑完全共用（同一個 `km_scan` 16 點平均、同一個 `km_engine`、同一個 `adc_scan`），去抖 8 frame 兩邊都在 engine 裡，沒有差別。
所以 M6 的差異只有三類：

1. `pump_reports` 裡多了 `km_pipeline_run`（SOCD → 層 → DKS/手把/燈光動作）以及引擎設定被 pipeline 改寫（`m6_main.c:157-170`）。
2. `pump_reports` 之後多了 `stream_to_lights()`、`anim_service(ms)`（`m6_main.c:950-951`），scope 多了 tag 0x0500。這些**在報告送出之後**才跑，不會擋住同一個 pass 的報告，只會讓「下一個 pass 更晚開始」。
3. 燈光場景（`km_lightscene`）等 M6 新的 flash 儲存路徑。

重要結構事實：`adc_scan.c:298` 一個 frame 完成後計時器**停住**，直到主迴圈呼叫 `adc_scan_release_frame()`（`adc_scan.c:188-199`）才重新開始。
所以掃描節奏 = 掃描時間 + 主迴圈「看到 frame」的等待；pass 越長，frame 越晚被收走，但**不會弄丟 frame**（只會變慢）。
一個 pass 最壞 ≈ 11k cycles ≈ 46 µs（`firmware/core/PIPELINE_NOTES.md` 成本表），所以光是 pass 變長，每個按鍵頂多多 **幾十 µs**，人感覺不到。
**能讓人感覺「卡」的必須是毫秒級的東西。** 下面依這個標準排名。

### 2.2 排名

| # | 原因 | 量級 | 證據 | 只在 M6？ |
|---|---|---|---|---|
| 1 | **引擎設定被 pipeline 覆寫**：profile 裡只要某鍵的層表有 DKS 或手把碼，`km_pipeline_slot_cfg` 就把那個鍵強制成 FIXED（DKS 用 t_act/t_act−10；手把用 0.20/0.10 mm），**原本的 RAPID 設定被丟掉** | 整個行程等級（毫米），遠大於其他項；手感上就是「要壓更深/放更慢才動」 | `km_pipeline.c:89-116`（`memset(out,0)` + `KM_ENGINE_TRIG_FIXED`）、`m6_main.c:166`（apply_profile 對每個鍵都呼叫）；L51 的 apply_profile 直接用 `prof->key[slot]`（`m5_main.c:143`） | 是（L51 沒有 DKS/手把） |
| 2 | **Flash 抹除/寫入卡住 CPU（毫秒到數十毫秒）**：設定儲存、校正儲存、燈光設定/場景儲存。只在「沒有按鍵」時開始，但一旦開始抹除，剛好在這時按下的第一個鍵要等完；`m5_led_persist_urgent()`（已抹除的燈光儲存必須做完）那一支甚至不檢查 idle。M6 UI 第二輪新增「燈光頁存/載場景」，會讓這種儲存變多 | 數 ms～數十 ms，且是「偶發的一下卡」，和「節奏遊戲偶爾吃掉一個音」的描述吻合 | `m6_main.c:171-196`（`g_flash_this_pass`，註解「sector erase stalls the CPU」）、`:954-985`（背景儲存區塊；`:971` urgent 支）、`:958` 閒置 1 s 後 `save_settings()`；探針最大值/0x0500[2]（不計 flash pass，所以要看 0x0600[3]） | 部分（L51 也有 flash 儲存，但 M6 多了場景/燈光儲存） |
| 3 | **tap-hold / Mod-Morph 的排隊**：只要有一個 tap-hold 鍵「尚未決定」，`drain` 會停住，**所有後面的鍵事件都排在它後面**直到決定（可達幾十到幾百 ms 的 hold 時間）。M6 新增 Mod-Morph/Toggle 等，使用到的機會更多 | 數十 ms，若 profile 有這類鍵；遊戲鍵位若剛好在其中就明顯 | `km_layers.c:10`（drain 說明）、`:301 pend_decide`、`:499/:529`；M6 呼叫 `km_layers_update_cycles`（`km_pipeline.c:169`） | 機制 L51 也有，M6 擴充了種類；是否影響取決於你的 profile |
| 4 | **pass 變長 → frame 變慢**（pipeline 約 +0.3k～+3k，最壞 ≈ 11k > frame 9.3k–10.4k；燈光 slice 1.5k–2.5k、stream/動畫/scope 另加） | 每次 ≤ 數十 µs，**不夠解釋「卡」**，但會讓 8 kHz 目標困難（125 µs = 30000 cycles，pass 要穩定遠低於此） | `PIPELINE_NOTES.md`「Cost」表；`m6_main.c:460`（PASS_BUDGET）、`:998`（`g_pass_over`）；`adc_scan.c:188-199` | 是 |
| 5 | **報告同步/USB 端點**：`km_report_sync_pump` 有變化就立刻送（`km_report_sync.c:47-74`），只在 `tx_ready` 為假時等；端點 1 ms 才被 poll 一次，所以平均多 0.5 ms、最多 1 ms（C 段）。兩個映像相同 | ≤1 ms，兩邊一樣 | `km_usb_ids.h:50-53`（bInterval 4） | 否（共同底線，8 kHz 的主要獲利處） |
| 6 | **scope 發佈 / stream / 動畫 tick** | 很小（發佈只複製 ≤25 個值，`km_scope.c:85-100`）；動畫 verify 只在沒按鍵時跑（`m6_main.c` `anim_service` 的 `g_down_now == 0u` 條件） | — | 是，但不太可能是主因 |

### 2.3 沒有找到的東西（排除）

* 手把路徑：`km_m6a` 的 `KM_M6_PAD=0`，`pad_active_now()` 為假，手把報告與端點都不存在（CMakeLists km_m6a 定義）。
* 去抖/16 點平均：兩個映像共用，且 8 frame ≈ 0.33 ms，不是差異來源。
* 報告被燈光/stream/動畫擋住：這些都排在 `pump_reports` **之後**（`m6_main.c:947` vs `:950-952`）。

### 2.4 怎麼用第 1 節的量測把它們分開

* 探針 [3] 最大值 ≈ 毫秒級以上 → #2 或 #3（再用 0x0500[2] 區分：不計 flash 的 pass 很短 → 是 flash）。
* 探針與 pass 都正常（幾十 µs）但手感還是差 → #1：用 `magger60_config.py`（或介面）檢查 profile 有沒有 DKS/手把碼的鍵，並比對 RAPID 設定；最快的驗證是**用一份沒有 DKS/手把/tap-hold/Mod-Morph 的 profile 同時燒 L51 與 M6A 比手感**。
* 0x0100[5] overruns > 0 或 M6 的 frame 週期明顯大於 L51 → #4。

---

## 3. 快速路徑設計（只有設計，還沒寫程式；要等第 1 節的量測確認 B 段/pass 長度真的有問題再做）

目標：**鍵一變，報告立刻出去，其他 M6 功能排在後面、而且不准拉長有按鍵事件的那個 pass。**

### 3.1 現況（為什麼要改）

`pump_reports`（`m6_main.c:513`）每個 pass 都做：複製按下清單 → `km_pipeline_run`（SOCD、層、DKS、手把、燈光動作）→ 組三種報告 → 三次 `km_report_sync_pump`。
這其實已經在 `stream/anim/led` **之前**，所以真正要擋的是兩件事：(a) 有 frame 的那個 pass 裡 pipeline + 組報告的長度；(b) 與按鍵無關的重工作（燈光 slice、stream 複製、動畫 frame 邊界、scope 發佈、flash）落在「剛好有按鍵事件」的 pass。

### 3.2 設計

```
每個 pass:
  1. USB poll（不變，最小）
  2. 若 frame 就緒: 掃描 → engine                           ← 熱路徑，不動
  3. FAST：引擎的按下集合 (bitmask 3 個 u32) 與上次不同?
        是 → 若 profile 標記為「plain」: 直接 slot→usage 表查出 usages，組 NKRO/boot 報告，立刻
             km_report_sync_pump(nkro) 然後 boot（consumer 只在變時送）；記 fast_sent++
           → 若 profile 需要 pipeline: 照舊跑 pipeline，但也是 engine 變了就「當場」送，
             不等到後面的 pass
        否 → 什麼都不送（heartbeat 仍由 sync 處理）
  4. SLOW（本 pass「有按鍵事件」就整段跳過，下個沒事件的 pass 補做）:
        stream_to_lights / anim_service / m5_led_service / scope 發佈 / flash 背景工作 / 手把重算
  5. 其餘（health、watchdog、feed）不變
```

重點規則：

1. **plain 旗標**：`set_profile()` 一次掃描 4×76 表（現在 `km_pipeline` 已經為了 `has_dks/has_pad/has_light` 這樣做），多算一個 `plain` = 每層沒有 tap-hold / Toggle / Mod-Morph / DKS / 手把 / 燈光碼 / SOCD 配對、沒有 one-shot。plain 的 profile 在 FAST 步驟只做一次查表（約 45 cycles × 按下鍵數），不進 pipeline。非 plain 的 profile 行為 100% 和現在相同，只是送出時點不變（本來就同 pass）。
2. **事件 pass 保護**：以「本 pass 有 frame 且按下集合改變」為條件（`key_event_pass`），第 4 步整段延後；延後最多 1 ms，之後強制補做（避免燈光/看門狗餓死；health 的 `KM_PROG_*` 進度回報不能被延後）。`m5_led_service` 的 slice 本來就是每 pass 一個，延後只是少做一個 slice。
3. **flash 不在有鍵時開始**：`m6_main.c:954-985` 的 urgent 支加上 `g_down_now == 0u` 條件，若已抹除的燈光儲存在有鍵時必須完成，改成「在按鍵事件之間的空檔做」（每次只做一個寫入步驟，抹除永遠只在沒鍵時）。（這項修的是第 2 節 #2。）
4. **引擎設定覆寫的透明化**（第 2 節 #1）：不是延遲路徑的事，而是行為：`km_pipeline_slot_cfg` 覆寫 RAPID 時，介面要明示（或保留使用者的 RAPID，只調整 press 點），避免 DKS/手把鍵悄悄變成固定觸發。
5. **送出決策**：`km_report_sync_pump` 已是「有差異就送、`tx_ready` 假才等」，不改；FAST 只是把呼叫提前到 engine 之後立刻做，並且 NKRO 先、boot 次之（Windows 主要吃 NKRO 介面）。

### 3.3 測試計畫（寫程式時一併做）

* 主機：`km_pipeline` 加一個 `km_pipeline_is_plain()`，測試「plain 的 profile 經 FAST 查表的輸出 == 經完整 pipeline 的輸出」（現有 400×1500 隨機測試擴充成對照）；`mutate_pipeline.py` 加突變。
* 主機：事件 pass 延後的排程器（純邏輯）有上限測試（延後 ≤ N pass 必定補做）。
* 硬體：km_m6p 探針 [3] 最大值與 0x0500 [2] 前後比較。

### 3.4 風險

延後燈光 slice 會讓燈光動畫在連打時略不順（可接受）；plain 判斷錯誤會漏掉層/SOCD → 所以預設「不確定就走 pipeline」，plain 只在表掃描證明沒有任何特殊碼時才成立。

---

## 4. 8 kHz 描述符計畫

### 4.1 改什麼（已寫成編譯旗標，預設關閉，所以 km_l51 / km_m6a / km_m6 / km_m6p 的描述符一個位元組都沒變）

`firmware/usb/include/km/km_usb_ids.h`：新旗標 `KM_USB_HS_8KHZ`（預設 0）。開啟時：

| 端點 | 目前 | 8 kHz 映像 | 理由 |
|---|---|---|---|
| IF0 boot 鍵盤 IN（`KM_USB_EP_KBD_IN`） | bInterval 4（1 ms） | **1（125 µs）** | 同一份按鍵狀態也走這裡，兩個鍵盤端點要一致，否則主機用哪一個就決定延遲 |
| IF2 NKRO IN（`KM_USB_EP_NKRO_IN`） | 4 | **1** | Windows 主要用這個 |
| IF1 vendor IN/OUT | 4 | 4（不變） | 只是設定/scope 通道，不需要；也不增加主機負擔 |
| IF3 手把 IN（`KM_USB_EP_PAD_IN`，只有 km_m6） | 4 | 4（不變） | 遊戲讀手把是以畫面更新率輪詢；手把報告每 ms 重算一次（`PIPELINE_NOTES.md`），125 µs 沒有收益，只多主機工作。8 kHz 映像目前也不含手把介面 |

`bcdDevice`：plain **0x0001 → 0x0003**，pad **0x0002 → 0x0004**（0x0002 已被手把變體使用，規則是「不重用」，見 `km_usb_ids.h` 註解）。Windows 會把它當新裝置重新列舉，屬正常。

新映像 **`km_m6h`** = km_m6p（M6 + 探針）+ `KM_USB_HS_8KHZ=1`，名稱 `KMFW-M6H`。已用 arm-none-eabi 建置並通過 `check_image.py`（沒有燒）。

### 4.2 證明描述符值的主機測試（`firmware/tests/test_usb.c`）

* `test_hs_polling_intervals`：從原始設定描述符位元組讀每個端點的 bInterval：旗標關 → 鍵盤/NKRO = 4、其餘 = 4；旗標開 → 鍵盤/NKRO = 1、其餘仍 = 4；並驗算 `125 µs × 2^(n−1)`：1→125 µs、4→1000 µs。
* `test_variant_plain_unchanged`：原本的 FNV 雜湊釘死（0x4A920842）維持不變（證明預設映像沒動）；旗標開時改釘 8 kHz 版雜湊 `0x6543E78A`，bcdDevice 0x0003/0x0004。
* 我已兩種設定都跑過 `python firmware/tools/run_tests.py`：皆 0 failures（旗標開的那次是暫時改標頭、測完還原）。

### 4.3 復原路徑（描述符變了，必寫）

1. 8 kHz 映像只動應用程式描述符；**bootloader、app-ID、boot flag 完全不碰**。bootloader 有自己的 USB 描述符，與應用無關。
2. 若燒入後電腦認不到鍵盤 / 鍵盤無反應：拔掉 USB，**按住 ESC + 左 CTRL 同時插入**，進 bootloader，用 `magger60_flash.py`（或你平常的流程）燒回 **km_l51**（或 km_m6a），再套 `local/apply_profile.sh`。
3. Windows 殘留：若換回 0x0001 後仍怪，到裝置管理員把「Magger60 Open Keyboard」解除安裝（勾選刪除驅動）再重插；因為 bcdDevice 不同，Windows 通常會分開記憶，不太需要。
4. 原廠韌體備份仍在 `backup10/`，最後保底。

### 4.4 硬體階梯（由你執行；我沒有碰鍵盤）

0. **基準**：已知現況鍵盤跑原廠韌體。先照第 1 節燒 **km_l51** 與 **km_m6p**，各量 30 秒，存 CSV。（這一步不涉及描述符。）
1. **只改描述符**：建 `km_m6h`，燒入（先確認 ESC+左CTRL 復原路徑你能做到），重插。檢查：
   * 裝置管理員 / `python tools/magger60_info.py` 的 `bcdDevice` = 0x0003；
   * 鍵盤能打字、`magger60_scope.py --run --tag 0x0600` 正常讀到（vendor 通道不變）；
   * 打 30 秒，看探針 [3] 與 0x0500[3]、0x0100[5] 有沒有比 km_m6p 更差。
   若不穩（掉鍵、卡住、認不到）→ 照 4.3 退回 km_l51，**停在這裡回報**，不進下一步。
2. **確認主機真的以 8 kHz poll**（見 4.5）。
3. 只有 1、2 都穩，才做第 3 節的快速路徑程式（先在 km_m6p 上量到 B 段有問題的證據再做）。
4. 最後把快速路徑 + 8 kHz 合成一個映像，重複 0 的量測，再當日常映像。

### 4.5 怎麼確認「主機真的 8 kHz poll」（這台 PC 能做到什麼）

誠實說明：**Windows 沒有內建顯示 HID 輪詢率的地方**，而且鍵盤在狀態沒變時裝置只回 NAK，主機端看不到「poll 次數」，只看得到「有資料的報告」。可行的方法由準到不準：

1. **USB 協定分析儀**（例如 Total Phase Beagle 480）：直接看每個 micro-frame 的 IN token，是唯一能「證明」的辦法。你若沒有這個，下面兩個只是間接證據。
2. **USBPcap + Wireshark**（免費，可裝在這台 Windows 11）：擷取 `Magger60` 的 NKRO 端點，看一次按壓產生的 interrupt IN 完成事件的時間戳；它是 URB 級（不是 micro-frame 級），所以只能看到「報告多快被主機取走」，不能直接看 125 µs 的 token。
3. **統計法（用現有東西）**：用 hidapi 讀 NKRO 報告，記錄每次按下事件的到達時間；同時韌體探針能告訴你「引擎看到 → 端點收下」(B 段)。1 kHz 時，端點收下到主機取走的等待均勻分布在 0–1 ms（平均 0.5 ms）；8 kHz 時應縮到 0–0.125 ms（平均 ~0.06 ms）。要得到這個，需要一個把「韌體端點收下時間」帶給主機的方式——**目前沒有**，是之後可加的診斷（例如在 km_m6h 的探針 tag 加一個「報告被主機取走」的時間戳，用 USB 控制器的 IN 完成中斷）。這部分我沒有做，標記為待辦。
4. 不要用「滑鼠輪詢率測試網站」：它量的是滑鼠移動事件，對這個鍵盤沒有意義。

### 4.6 風險與未知

* **這顆 USB 核心（N32H48x，高速）在 bInterval 1 下的行為沒有實測過**；`usb_hs.c` 沒有任何和 bInterval 相關的暫存器設定（描述符以外沒有用到 `KM_USB_BINTERVAL_*`），理論上只是主機排程變密，但 FIFO/NAK 行為要硬體階梯第 1 步才知道。
* 主機端：Windows 的 HID 驅動接受 bInterval 1，但遊戲讀鍵盤本身多半是 1 kHz 左右（Raw Input / 訊息迴圈）；8 kHz 能省的主要是 C 段的平均 0.5 ms → ~0.06 ms，**不能解決第 2 節 #1–#3（毫秒級）的問題**。所以順序是：先量，再修 #1–#3，8 kHz 是最後的加分。
* 8 kHz 要求主迴圈 pass 穩定遠小於 125 µs（30000 cycles），目前最壞 pass 約 11k，仍在範圍內，但 flash 抹除那種毫秒級 pass 會讓某些 micro-frame 沒資料可送（只是延遲，不會錯）。

---

## 5. 補充：使用者確認「沒有做額外設定」（預設 profile）

預設 profile（`km_settings_default`，`km_settings.c:79-95`）沒有任何 DKS / 手把 / SOCD / Mod-Morph 碼（M6 的記錄全為 0，表只有預設鍵碼與 FN/媒體碼）。因此：

* 第 2 節 **#1（引擎設定被覆寫）不成立**：`km_pipeline_slot_cfg` 在這種 profile 回傳 false，設定與 L51 相同。
* **#3（tap-hold 排隊）基本不成立**：預設表沒有 tap-hold。
* `km_pipeline` 的 `has_dks/has_pad/has_light` 皆為假，pipeline 在這個 profile 幾乎沒有額外成本。

所以以讀程式碼來說，**預設 profile 下我找不到 M6 專屬的毫秒級延遲來源**。剩下的候選是 #2（flash 抹除，M6 的燈光/場景儲存較多）與 #4（pass 變長，但每次只有幾十 µs）。這代表原因很可能要靠實測才找得到（第 1 節的探針：看 [3] 最大值是否有毫秒級尖峰），或者不是韌體主迴圈而是別處（例如燈光效果在連打時造成的 pass 抖動、主機端行為）。在拿到實測數字前，我不會宣稱已找到原因。
