# M4 計畫：L2「打得出字」（自己的深度模型、按鍵引擎、USB 描述元）

目標（CUSTOM_FIRMWARE_PLAN.md §3 的 L2）：自製韌體第一次用自己的 USB 鍵盤報告送出按鍵，全鍵可打字、無鬼鍵、無連發。

**原創規則（PUBLISH_PLAN.md §4A，這個階段的新程式全部適用）**：
- 不讀、不抄 `extracted/`、`ghidra/out/`、原廠映像、原廠 JS、原廠查找表、原廠黃金資料。
- 不以現有 M0 核心（`km_depth.c`、`km_trigger.c`、`km_filter.c`、`km_dir.c` 等，它們是照原廠反編譯寫的）為藍本：新的深度模型、按鍵引擎、USB 描述元由我們自己設計。舊檔案只在避免名稱衝突時看一眼，之後會被新設計取代。
- 可以用的來源：USB／HID 規格、標準 HID Usage Table（公開）、晶片事實、`firmware/core/data/` 底下我們自己量出來的資料、本 repo 的 M3 驅動。
- 刷寫、備份、救援規則同 M2／M3（`M3_PLAN.md` §1）：子任務只建置與主機測試，不碰鍵盤；刷寫由主流程在使用者同意後做。

## 1. 已有的自己的資料

- `firmware/core/data/slotmap_measured.json`：槽位→實體鍵（61 鍵；15 個槽位無鍵）。
- `firmware/core/data/slot_ranges_s2.json`：每個鍵放開（約 3102–3451）與按到底（約 1794–2398）的讀值，行程幅度 998–1365 個計數；讀值愈小＝愈按下去。
- `km_scan_sums()`：每槽位 16 點和（÷16 ＝ 原始計數）；一個 frame 約 39 µs，主迴圈約 23,000 frame／秒。
- 開關行程：名義 3.4 mm（公開規格）。

## 2. 介面約定

**深度模型（T-A）→ 引擎（T-B）**

```c
/* firmware/core/include/km/km_depth_model.h */
typedef struct { uint16_t released; uint16_t bottom; } km_depth_cal_t;   /* ADC 計數（已 ÷16）；released > bottom */
#define KM_TRAVEL_MM100 340u
/* 讀值 -> 深度，單位 0.01 mm，夾在 0..KM_TRAVEL_MM100；單調不增（讀值愈小深度愈大） */
uint16_t km_depth_model_mm100(const km_depth_cal_t *cal, uint16_t value);
```

**引擎（T-B）→ 主程式**

```c
/* firmware/core/include/km/km_engine.h */
void km_engine_init(km_engine_t *e);
/* 開機：用靜止時的一段 frame 取每槽位的 released 基準；沒有按鍵時才有效；不合理的槽位（過低／過高）停用 */
void km_engine_calibrate_idle(km_engine_t *e, const uint16_t sums_div16[76]);
/* 每個 frame 呼叫一次；內部做濾波、深度、固定啟動點與遲滯 */
void km_engine_update(km_engine_t *e, const uint16_t sums_div16[76]);
bool km_engine_slot_down(const km_engine_t *e, unsigned slot);
/* 診斷：每槽位目前深度、基準、停用旗標 */
uint16_t km_engine_depth(const km_engine_t *e, unsigned slot);
```

**鍵盤報告（T-B）**：`km_keymap.h`：槽位→HID usage（預設 ANSI 60%，標準 HID Usage Table，資料寫在原始碼，來源是公開規格與 `slotmap_measured.json` 的鍵名）；`km_report.h`：`km_report_build_boot(down_slots, report8)`（最多 6 鍵＋修飾鍵）與 `km_report_build_nkro(down_slots, report)`（位元圖，格式由 T-C 的描述元決定）；溢位時 boot 報告填 `0x01`（ErrorRollOver）。

**USB（T-C）**：`km_usb.h` 的描述元與 `port/n32h48x/usb_hs.c` 的 IN 端點送出函式：

```c
bool km_usbhs_kbd_ready(void);                        /* 上一包已送完 */
bool km_usbhs_send_kbd(const uint8_t report[8]);      /* IF0，boot 鍵盤格式 */
bool km_usbhs_send_nkro(const uint8_t *report, unsigned len);   /* IF2 */
```

## 3. 子任務

| 任務 | 內容 | 檔案 | 需要硬體 |
|---|---|---|---|
| **T-A 深度模型（R2）** | 自己的讀值→深度模型：以 released／bottom 兩個端點加物理模型（磁場隨距離衰減）擬合，參數與推導寫在原始碼註解；端點誤差、單調性、對雜訊的靈敏度測試；用 `slot_ranges_s2.json` 的 61 組端點做測試；留出「之後用厚薄規量幾個點驗證」的資料格式 | `core/include/km/km_depth_model.h`、`core/src/km_depth_model.c`、`tests/test_depth_model.c`、`tools/` 的擬合／檢視腳本 | 否 |
| **T-B 引擎＋鍵位＋報告** | 自己設計的按鍵引擎（濾波、開機基準與不合理槽位停用、固定啟動點加遲滯、去抖動、多鍵串擾保護）、預設鍵位、boot／NKRO 報告建構；合成訊號的主機測試與突變測試；用 `slotmap_measured.json`、`slot_ranges_s2.json` 當真實資料測試 | `core/include/km/km_engine.h`、`km_keymap.h`、`km_report.h` 與對應 `.c`、`tests/test_engine.c` | 否 |
| **T-C USB 描述元與報告送出（R3）** | 自己設計的 HID 描述元（IF0 boot 鍵盤；IF1 vendor 通道 64 B 沿用目前的 scope／cmd 0xC5 協定；IF2 NKRO 位元圖＋多媒體），字串與序號自己寫；VID／PID 先沿用 `19F5:FB49`，集中成一個設定標頭方便日後改；把 `km_usb_desc.c` 換成自己的；`usb_hs.c` 加 IN 端點送出；描述元的主機端結構檢查（長度、端點、報告描述元以小型 HID 解析器驗證，不使用任何原廠位元組）；舊的原廠描述元黃金測試改成新設計的檢查 | `usb/`、`port/n32h48x/usb_hs.[ch]`、`tests/test_usb.c` | 否（建置） |
| 整合（主流程） | `m4_main.c`：S3 掃描＋引擎＋報告＋進 bootloader 命令；實機階梯 K1–K3 | — | 是 |

## 4. 實機階梯（主流程做，每級都要你同意才刷）

| 級 | 內容 | 驗收 |
|---|---|---|
| K1 | 只換 USB 描述元（無掃描、不送鍵）：用 scope 通道回報 | Windows 列舉成 `19F5:FB49`，裝置管理員看得到鍵盤、`magger60_info.py` 讀得到版本 |
| K2 | 掃描＋引擎，**只有 ESC 會送出按鍵**（安全閘）；其他鍵只在 scope 顯示 | 備用鍵盤在手，按 ESC 在記事本看到 Esc 動作；沒有卡鍵 |
| K3 | 全鍵可打字 | 逐鍵打字測試、無鬼鍵、無連發、熱插拔後正常 |

所有階梯都保留「USB 命令進更新模式」與 ESC＋左 CTRL 救援。
