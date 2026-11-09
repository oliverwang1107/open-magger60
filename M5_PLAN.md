# M5 計畫：L3「好用」（穩定性、快速觸發、設定儲存、圖層、設定協定）

目標（CUSTOM_FIRMWARE_PLAN.md §3 的 L3，加上 L4 的 FN 層）：K3 已能打字；這一階段讓它成為「可設定、可記住設定、手感可調，而且長時間用也不出事」的鍵盤。

**穩定性是這一階段的第一優先**：它不是最後才補的功能，而是每個任務的驗收條件（§2A），並有自己的任務（T-E）和自己的實機階梯（先於所有新功能）。

**原創規則**同 M4_PLAN.md：新程式全部自己設計；不讀、不抄 `extracted/`、`ghidra/out/`、原廠映像或網頁驅動；可用來源是 USB／HID 規格、晶片事實、`firmware/core/data/` 的自量資料、本 repo 的程式。flash 配置、設定格式、設定協定一律自己設計（不沿用原廠的區塊劃分；`km_flash_map.h` 裡 `KM_CFG_*` 是原廠留下的舊配置，只保留 `KM_CFG_PROFILE` 的 bootloader 旗標，其餘不再使用）。

## 1. 既有事實

- 引擎（`km_engine`）：固定啟動點＋遲滯，每鍵 `km_engine_key_cfg_t { press_mm100, release_mm100, mode }`，`mode = KM_ENGINE_TRIG_FIXED`；深度單位 0.01 mm，行程 340。靜止雜訊約 ±0.15 mm（約 15 個計數），所以任何門檻都要大於約 0.2 mm 才穩。
- 按下去抖動 8 frame（約 0.35 ms），放開 6 frame；每 frame 引擎約 3.4k 週期，frame 預算約 10.4k 週期。
- 報告：NKRO（IF2，id 1，29 位元組位元圖）、boot（IF0，只在主機選 boot 協定時）、consumer（IF2，id 2，已有描述元，尚未送過）。
- 可用 flash：整個 app 區 0x08008000–0x08028000（K3 只用 15 KB）；設定區 0x0802A000–0x0803FFFF（8 KB 一個 sector，共 11 個）。**0x0802C000 sector 的 byte 0 bit 0 是 bootloader 旗標**（`flash_boot.c` 讀出整個 sector、改 bit、擦除、寫回），不能挪走，也不能存別的東西在這個 sector 裡。
- 現有 flash 寫入全部經 `km_flash_guard.h` 的白名單（目前只允許 0x0802C000 sector）。`images/*.bin` 與 `magger60_flash.py` 刷寫時會把整個設定區（到 0x0803FFFF）用備份內容寫回，所以我們的程式必須容忍設定區內是原廠舊內容或 0xFF（有效魔術字＋CRC 才採用，否則用預設值）。
- 已觀察到、尚未解釋的現象（穩定性要處理）：(1) K3 測試時 Windows 端出現一次 50 秒的空白鍵按住，而同時引擎回報沒有鍵被按下（可能是別的鍵盤，也可能是放開報告漏掉）；(2) 引擎偶發最長約 10.7k 週期的更新，約等於整個 frame 預算；(3) 首次 S2 映像曾讓 USB 列舉失敗，真因不明；(4) 一次 flash 寫入錯誤（0x10A20）未重現；(5) 目前餵狗無條件執行（主迴圈只要還在轉就餵），掃描停了也不會被偵測。
- 約定：「第二把鍵盤在手」「每次刷寫要你同意」「刷完備份比對」同 M3／M4。

## 2. 介面約定

**設定儲存（T-D）→ 其他**

```c
/* firmware/core/include/km/km_settings.h */
#define KM_PROFILES 4u
typedef struct {                       /* 一個 profile，全部是自己的格式 */
    km_engine_key_cfg_t key[KM_KEYMAP_SLOTS];   /* 以槽位為索引；沒有鍵的槽位忽略 */
    uint8_t  layer_usage[KM_LAYERS][KM_KEYMAP_SLOTS];  /* T-C 的圖層表，layer 0 = 基底 */
} km_profile_t;
typedef struct { uint8_t version; uint8_t active; km_profile_t profile[KM_PROFILES]; } km_settings_t;
void km_settings_default(km_settings_t *s);
/* 序列化／反序列化：魔術字、版本、長度、CRC32；壞掉的一律回 false，呼叫端用 default */
unsigned km_settings_encode(const km_settings_t *s, uint8_t *buf, unsigned cap);
bool     km_settings_decode(km_settings_t *s, const uint8_t *buf, unsigned len);
/* 儲存層：兩個 sector 輪流寫（序號大者為有效），斷電任何時間點都留有一份完整的；磨損平均 */
bool km_store_load(km_settings_t *s);          /* false = 沒有有效紀錄 */
bool km_store_save(const km_settings_t *s);    /* 擦除／寫入／驗證，期間餵狗、不拿 USB（呼叫端決定時機） */
```

flash 用途（自己的配置）：設定 A = 0x08030000、設定 B = 0x08032000（輪流），校正 = 0x08034000（見 T-F），燈光設定 = 0x08036000（L5 之後的 km_l51，見 `LED_NOTES.md` §15）。0x0802A000、0x0802C000（旗標以外）不再解讀。`km_flash_guard.h` 的白名單新增這四個 sector；bootloader 區、app 區、app-ID block 仍不可寫。

**快速觸發（T-A）→ 引擎**

```c
/* km_engine.h：km_engine_trig_mode_t 加 KM_ENGINE_TRIG_RAPID；key_cfg 加欄位（結構整體版本化，不改既有欄位的意義） */
typedef struct {
    uint16_t press_mm100, release_mm100;     /* FIXED 的啟動點與放開點（既有） */
    uint8_t  mode;                           /* FIXED／RAPID */
    uint16_t rt_down_mm100, rt_up_mm100;     /* RAPID：往下多少算按、往上多少算放（各自可設，0.1 mm 起） */
    uint16_t top_dead_mm100, bottom_dead_mm100;  /* 上方死區（不到這深度不觸發）、下方死區 */
    uint8_t  continuous;                     /* 1 = 連續 RT：在行程中間放開後可再按，直到回到上方死區才重置 */
} km_engine_key_cfg_t;
```

**圖層與 FN（T-C）→ 主程式**

```c
/* km_keymap.h */
#define KM_LAYERS 4u
uint8_t km_keymap_usage(unsigned layer, unsigned slot);          /* 既有，layer 0..3 */
void    km_layers_set_map(const uint8_t map[KM_LAYERS][KM_KEYMAP_SLOTS]);   /* 載入設定；NULL = 預設 */
/* km_layers.h：FN（slot 對應的鍵）按住時目前圖層 = 1；usage 0x01..0xE7 為鍵盤頁；
   0xF0..0xFF 內部功能碼：F0 = 按住切圖層 1，F1..F4 = 切 profile 1..4（放開時生效），F5/F6 = 音量，F7 = 靜音，F8..FA = 媒體 */
void km_layers_update(const uint8_t *down_slots, unsigned n, km_layers_out_t *out);   /* out：鍵盤 usage 清單、consumer usage、profile 切換請求 */
```

consumer 報告用 IF2 id 2（3 位元組，已定義）。

**健康監控與失效保護（T-E）→ 主程式**

```c
/* firmware/core/include/km/km_health.h */
typedef struct {
    uint32_t frames, scan_stalls, dma_errors, scan_restarts;   /* 掃描鏈 */
    uint32_t engine_max_cycles, engine_over_budget;            /* 引擎時間預算 */
    uint32_t usb_resets, usb_suspends, report_retries, report_resyncs;
    uint32_t glitches, calibrations, calibration_refused;
    uint8_t  reset_cause;                                      /* 本次開機的重置原因（RCC 旗標）*/
    uint8_t  fault_flags;                                      /* 目前有效的失效旗標（見下） */
} km_health_t;
/* 失效旗標：SCAN_STALL、DMA_ERROR、CAL_LOST、USB_SUSPENDED、ENGINE_OVERRUN */
/* 進度制餵狗：主迴圈每一圈回報各環節有沒有前進；只有「掃描有新 frame 且 USB 輪詢有跑」才允許餵 */
void km_health_progress(km_health_t *h, unsigned what);        /* KM_PROG_SCAN／USB／ENGINE */
bool km_health_should_feed(km_health_t *h, uint32_t now_cycles);
/* 失效安全：只要有失效旗標，就送一份全放開的報告並維持，直到旗標清除且校正有效 */
bool km_health_keys_allowed(const km_health_t *h);
```

**設定協定（T-B）→ UI／工具**：vendor 通道（IF1，64 位元組）上自己的命令集，與 cmd 3、scope 0xC5 並存；命令 `get_settings`／`set_settings`（分頁讀寫整個 `km_settings_t` 序列化內容，寫入先存 RAM，`commit` 才呼叫 `km_store_save`）、`select_profile`、`recalibrate`、`save_calibration`、`live_depth`（唯讀）。每個會寫 flash 的命令需要在同一請求裡帶「確認字」，且只在 `KMFW` 韌體接受。

## 2A. 穩定性規則（所有任務都要遵守，驗收時逐條檢查）

1. **失效時放開所有鍵**：任何一條失效路徑（掃描停止、DMA 錯誤、校正失效、USB 暫停、引擎超時）都先送全放開報告；寧可停止輸入，也不能卡鍵。恢復後從「全放開」重新開始。
2. **報告對帳**：鍵盤端保存「最後一次成功送出的報告」；每 500 ms 若仍有鍵按下，重送一次目前狀態（主機端忽略相同報告，無害）；放開報告送出失敗時持續重試，直到成功，並計入 `report_retries`。
3. **所有等待都有逾時**：迴圈裡沒有沒有上限的等待；硬體等待（ADC、DMA、flash、USB）逾時後走失效路徑並計數。
4. **時間預算**：主迴圈單圈最壞時間有上限且被量測；SETUP 封包要在數毫秒內被處理；引擎單次更新超過預算要被計數而不是默默拖慢。flash 擦除／寫入只在「沒有鍵按下、USB 端點閒置」時進行，期間餵狗。
5. **進度制餵狗**：IWDG 只在掃描與 USB 都有前進時才餵；餵狗卡住 = 重置，而重置後 bootloader 會很快回到 app，不是磚（bootloader 本身每次開機都重啟 IWDG，已實機驗證）。
6. **每個新功能都要有「故障注入」測試**：丟報告、端點永遠忙、DMA 錯誤、雜訊爆衝、校正時有鍵被按住、設定資料壞掉、flash 寫入中途斷電。測試要證明結果是「安全狀態」而不是「卡鍵」。
7. **可觀測**：所有計數器和失效旗標都能經 scope 通道看到（新 tag），重置原因開機後可讀。

## 3. 子任務

| 任務 | 內容 | 檔案（只動這些） | 需要硬體 |
|---|---|---|---|
| **T-A 快速觸發** | 引擎加 RAPID 模式：往下 `rt_down`、往上 `rt_up`（各自的遲滯），上下死區，連續 RT；測試用合成訊號（斜坡、折返、雜訊）和 `slot_ranges_s2.json` 的真實端點；突變測試；時間預算（每 frame 不超過約 +1k 週期）；設定驗證函式拒絕小於雜訊底限的值；與 FIXED 並存，既有測試不變 | `km_engine.[ch]`、`tests/test_engine*.{c,inc}`、`tools/mutate_engine.py`、`core/ENGINE_NOTES.md` | 否 |
| **T-B 設定協定＋主機工具** | 在 `hid/` 加 `km_config.[ch]`：命令解析、分頁、確認字、回覆格式、長度與範圍檢查（全部對著模擬儲存層和模擬引擎測）；`tools/magger60_config.py`（讀、寫、切 profile，乾跑預設、`--run --confirm`，只送白名單命令）；協定文件 `CONFIG_PROTOCOL.md`；UI 的 `ui/js/device/` 加對應的真實裝置實作（既有 mock 不變） | `hid/`、`tools/magger60_config.py`、`CONFIG_PROTOCOL.md`、`ui/js/device/`、對應測試 | 否 |
| **T-C 圖層／FN／多媒體鍵** | `km_layers`：FN 按住切圖層、profile 切換功能碼、consumer 報告建構；預設 layer 1 = 方向鍵、F1–F12、媒體鍵（自己決定，寫在原始碼與文件）；`km_report` 加 `km_report_build_consumer`；測試含按住中放開 FN、FN 與其他鍵的先後順序（鍵在按下時用當時的圖層，放開時用按下時的那個，避免卡鍵） | `km_keymap.[ch]`、`km_layers.[ch]`、`km_report.[ch]`、`tests/test_layers.c` | 否 |
| **T-D 設定格式＋儲存層** | `km_settings`（編解碼，魔術字／版本／CRC32，往返與壞資料測試，foreign／全 0xFF 內容回 default）、`km_store`（雙 sector 輪流＋序號，在記憶體模擬 flash 上測試，包含每個寫入位元組點斷電的掃描測試）、`port/n32h48x/store_flash.c`（擦除／寫入／驗證，餵狗，所有位址經白名單）、擴充 `km_flash_guard.h` 並加測試、位址稽核通過 | `core/`（新檔）、`port/n32h48x/store_flash.[ch]`、`km_flash_guard.h`、`tests/` | 否（建置） |
| **T-E 穩定性＋失效保護** | `km_health`（計數器、失效旗標、進度制餵狗、`keys_allowed`）、報告對帳與重送邏輯（純邏輯，不含硬體，可主機測試）、掃描鏈的逾時與重啟邏輯（`port/n32h48x/adc_scan.c` 加 `adc_scan_restart()`、DMA 錯誤與 frame 逾時偵測）、USB 暫停／恢復／匯流排重置的狀態處理（`usb_hs.c` 通知、主程式放開所有鍵）、重置原因讀取、故障注入測試（`tests/test_health.c`、`tests/sim/` 擴充）、引擎時間預算量測（找出 10.7k 週期的原因並記錄）、scope 新 tag（健康計數器 `0x0300`、按下鍵的位元遮罩 `0x0301`，供對帳工具用）；**主機端浸泡測試工具** `tools/magger60_soak.py`：長時間記錄「Windows 端看到的鍵（用 Raw Input 依裝置過濾，只看 19F5 的鍵盤）」對照「韌體回報的按下鍵」，超過 200 ms 的不一致即記錄事件，附自我測試與模擬器 | `core/`（新檔 `km_health.[ch]`）、`port/n32h48x/adc_scan.[ch]`、`usb_hs.[ch]`、`tests/`、`tools/magger60_soak.py` | 否（建置） |
| 整合（主流程） | `m5_main.c`：K3＋設定載入／儲存＋圖層＋RT＋設定協定；實機階梯 | — | 是 |

並行安全：T-A 只動引擎；T-E 只動新檔 `km_health`、`adc_scan`、`usb_hs` 與 soak 工具（不碰引擎與 keymap）；T-B 只動 hid／tools／ui；T-C 只動 keymap／layers／report；T-D 只動新檔和 flash guard。共用的 `tests/km_test.h`、`test_main.c`、`CMakeLists.txt`、`tools/run_tests.py` 由整合時合併（同 M4）。`km_settings_t` 與 `km_engine_key_cfg_t` 的欄位以本文件為準：T-D 引用 T-A 的 key_cfg 與 T-C 的 `KM_LAYERS`，在各自分支先放一份最小標頭，整合時刪掉。

## 4. 實機階梯（主流程做，每級都要你同意才刷）

| 級 | 內容 | 驗收 |
|---|---|---|
| **L3-0（先做）** | K3＋T-E：健康監控、進度制餵狗、失效保護、報告對帳；**不加任何新功能**。這是之後所有功能的穩定底座 | 浸泡測試（見下）通過：8 小時混合使用（打字、閒置、長按）無卡鍵、無非預期重置、`magger60_soak.py` 無不一致事件；熱插拔 50 次、電腦睡眠／喚醒 10 次，每次恢復後可正常打字；另用一個刻意讓掃描停止的測試版本，確認鍵盤送出全放開並在約 2 秒內由看門狗重置，重置後恢復正常 |
| L3-1 | L3-0＋快速觸發（設定寫死在程式裡，不碰 flash） | 打字照常；用 scope 與 UI 看 RT 的上下點；手感試打 |
| L3-2 | **第一次寫自己的設定 sector**（`km_store_save` 一次，寫 0x08030000）；重插電後讀回 | 讀回與寫入逐位元組相同；備份比對：除了我們的 sector 外全部相同；bootloader 與 app-ID 不變 |
| L3-3 | 圖層／FN、profile 切換、設定協定存取 | FN 組合行為正確；斷電重插後 profile 仍是上次的 |
| L3-4 | 校正持久化（T-F，見下）與日常使用一天 | 沒有卡鍵、沒有漏打；浸泡測試再跑一次（含設定儲存期間打字） |

每一級加入新功能後，都要重跑 L3-0 的浸泡與熱插拔項目（縮短版 2 小時、熱插拔 20 次）；有任何一項退步就退回上一級。

所有階梯保留「USB 命令進更新模式」與 ESC＋左 CTRL 救援；每次刷完做備份，比對 bootloader 與 app-ID 沒變。

## 5. 後續（M5 之外）

- **T-F 校正持久化**：已實作（主機測試全過，**尚未上真機**，設計與數據見 `firmware/core/CALIBRATION_NOTES.md`）。每鍵學到的 bottom 以「附加式紀錄」存在 0x08034000（一個 sector 放 24 筆，滿了才擦除一次；斷電在每個擦除／寫入步驟都掃描測過）；只在 bottom 變深至少 16 個計數、靜止 20 秒、距上次存檔至少 10 分鐘時才寫；寫入是一圈一個 flash 操作，有鍵按下就暫停；開機時有有效紀錄就用一個 frame 取代 64 frame 的校正等待（與現場感測值差超過 150 計數就丟掉紀錄、照常校正）；`save_calibration`（SCAL）指令已在 `km_l33` 接上。映像只有 `km_l33` 有這個功能，`km_l31` 不變。實機驗證步驟在 T-F 的交接報告裡，之後併入 M5_TEST_GUIDE.md。
- **深度模型驗證**：預設形狀參數 `a=2` 是依物理推估的；用厚薄規量幾個行程點（`tools/magger60_depth_check.py`）來擬合；需要你操作量測。
- **L4 進階鍵**（DKS、MT、TGL、巨集）、**L5 燈**：各自有獨立的任務，等 L3 穩了再開。
- **已知待查**（已納入 T-E）：引擎偶發最長約 10.7k 週期的更新；K3 期間觀察到一次來源不明的 Windows 端空白鍵 50 秒。T-E 的對帳工具與 scope 按鍵遮罩就是為了在下次出現時能當場分辨「別的鍵盤」或「放開報告漏掉」。
