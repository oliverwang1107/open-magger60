# M3 計畫：L1「看得見」（感測掃描）

目標（CUSTOM_FIRMWARE_PLAN.md §3 的 L1）：自製韌體重現 mux＋ADC＋DMA 掃描，把 76 個槽位的 16 點滑動和用 vendor 通道回報給主機；WASD 四個 injected 通道正常。**不打字、不送按鍵**，所以不影響你用原廠韌體打字。

規格來源：`FIRMWARE_Magger60.md` §3–§5（周邊、腳位、槽位表）、`SENSING_BRINGUP_NOTES.md` §1（資料路徑）。bootloader 每次開機都會用 ADC 讀開機鍵，它在實機上跑過，是最可靠的對照。

## 1. 安全規則（沿用 M2，所有子任務都要遵守）

- 不實作任何新的 flash 寫入。唯一的 flash 寫入是 D4 的 `flash_boot.c`（只寫 `0x0802C000` 設定扇區）。
- 類比腳位只當類比輸入；唯一新增的輸出腳是 mux 位址線 PB3／PB4。**PF5**（疑似感測器電源，原廠開機拉低、進 bootloader 前拉高）不得亂動：要先從反編譯與 bootloader 找出證據再決定。
- 每個主迴圈都要餵 IWDG（`km_iwdg_feed()`），從第一行指令開始。
- 暫存器流程一律從原廠／bootloader 反編譯抄，標出處（位址）。反編譯沒有的欄位值，才查 SVD；都沒有就標「未確認」，不要猜。
- 映像（`images/*.bin`）只能用 `tools/magger60_image.py` 與 `tools/magger60_flash.py` 產生與刷入；**子任務只建置，不刷**，刷寫由主流程在使用者同意後做。

## 2. 介面約定（子任務各自照這個寫，整合時才接起來）

槽位數 `KM_SLOTS = 76`；槽位 s = 2×字索引 + 半字（0 = 低半 = ADC1／ADC3，1 = 高半 = ADC2／ADC4），見 `SENSING_BRINGUP_NOTES.md` §1.2。

**驅動 → 掃描核心**（驅動是 `port/n32h48x/`，核心是 `core/`，兩邊都不含對方的程式）：

```c
/* firmware/core/include/km/km_scan.h */
typedef struct km_scan km_scan_t;                       /* 不透明大小由實作決定，提供 KM_SCAN_STATE_SIZE 或公開 struct */
void km_scan_init(km_scan_t *s);
/* 一個 mux 相位（0..3）的 9 個 DMA 字：words[0..5] = ADC1+2（高 16 = ADC2、低 16 = ADC1），words[6..8] = ADC3+4 */
void km_scan_push_phase(km_scan_t *s, unsigned phase, const uint32_t words[9]);
/* injected：w36 = ADC2<<16 | ADC1（槽位 73 A、72 S），w37 = ADC4<<16 | ADC3（槽位 75 W、74 D） */
void km_scan_push_injected(km_scan_t *s, uint32_t w36, uint32_t w37);
/* 76 個 16 點和；打包相加，12 位元 × 16 不會進位 */
const uint16_t *km_scan_sums(const km_scan_t *s);
/* 槽位表（來自 tools/magger60_slotmap.py）：該槽位的相位、ADC、rank、通道、鍵名 */
const km_slot_info_t *km_scan_slot_info(unsigned slot);
```

**任何程式 → 示波器通道**（vendor 偵錯命令，只有自製韌體回應）：

```c
/* firmware/hid/include/km/km_scope.h；在主迴圈呼叫，不在中斷裡 */
void km_scope_publish(uint16_t tag, const uint16_t *values, uint16_t count);   /* count <= 128；tag 標示資料種類 */
```

tag：`0x0001` = S1 單點讀值、`0x0002` = S2 輪詢掃描的 76 個原始值、`0x0003` = S3 的 76 個 16 點和、`0x0100`+ = 診斷（時序、DMA 計數）。

**驅動給核心的資料**：`uint32_t g_phase_words[4][9]`（DMA 目標）與 `uint32_t g_inj[2]`。每個相位 DMA 兩條都完成時，驅動呼叫 `km_scan_push_phase`；相位 3 完成後再呼叫 `km_scan_push_injected`。

## 3. 實機階梯（主流程做，每一級都要你同意才刷）

| 級 | 內容 | 驗收 |
|---|---|---|
| S1 | 輪詢單點：開 ADC1（ch3）與 ADC2（ch17），設 mux 相位 1／0，讀 ESC 與左 CTRL 的單次轉換值，用 `km_scope_publish` 回報 | 按住 ESC／左 CTRL 時數值明顯變小（< 0x999，與 bootloader 的判斷一致）；沒按時在合理範圍 |
| S2 | 輪詢掃描：軟體跑 4 個相位 × 全部通道，一次回報 76 個原始值（無 DMA、無計時器） | 逐鍵按壓時，只有對應槽位變動；槽位表（`FIRMWARE_Magger60.md` §5）得到實機驗證 |
| S3 | 完整掃描：GTIM1 觸發 ADC＋DMA＋injected，`km_scan` 的 16 點和 | 76 個槽位和（含 WASD）隨按壓變化；量測整體掃描率 |
| S4 | 驗收與記錄：每鍵放開／按到底的數值範圍存成黃金資料，記掃描率 | 寫進本文件 |

## 4. 子任務分工

| 任務 | 內容 | 檔案（盡量只動自己的） | 需要硬體 |
|---|---|---|---|
| **T1 掃描核心** | `km_scan`：槽位表、環形 16 點和、injected、相位推進；合成訊號的主機測試與刻意破壞測試 | `core/include/km/km_scan.h`、`core/src/km_scan.c`、`tests/test_scan.c` | 否 |
| **T2 示波器通道＋主機工具** | `km_scope`：vendor 偵錯命令（唯讀、只有自製韌體回應、不與原廠命令衝突）、分頁讀取；主機端 `tools/magger60_scope.py`（原始值表／即時熱圖） | `hid/*/km_scope.*`、`km_vendor.c` 的新 case、`tests/test_scope.c`、`tools/magger60_scope.py` | 否 |
| **T3 感測驅動** | 研究原廠 `sensing_init` 與 bootloader 的 ADC／DMA／GTIM／GPIO 初始化，寫成 `SENSING_BRINGUP_NOTES.md`；寫 `port/n32h48x/` 驅動與 S1／S2／S3 三個 main，並建置映像（不刷） | `port/n32h48x/sense_*`、`adc_scan.*`、`s1_main.c`、`s2_main.c`、`s3_main.c`、`SENSING_BRINGUP_NOTES.md` | 否（只編譯） |
| 整合（主流程） | 合併三個分支、接起來、S1→S4 實機驗證 | — | 是 |

T3 在 T1／T2 的分支還沒合進來時，用 `port/n32h48x/m3_stubs/` 放暫時替身（原型與第 2 節一字不差），整合時刪掉。

## 5. 狀態（2026-10-27）

- T1、T2、T3 都已合進 `main`（暫時替身已刪除，S1–S3 連結的是真正的 `km_scan`、`km_scope`）。主機測試 715,464 項全過；S1–S3 與 D4 的 ARM 建置通過、沒有警告。
- 映像（不進 git）：`images/s1.bin`（sha256 `5e21a7652fd73d52`）、`s2.bin`（`09fec9363b055e7d`）、`s3.bin`（`e63a8223ddeee995`），設定區取自 `backup7`；刷寫工具乾跑都通過。
- 位址稽核：三個映像對 flash 控制器（`0x40022000…`）的存取都只出現在 D4 的 `km_flash_request_bootloader`／`wait_ready`；感測程式碼沒有。
- **S1 尚未刷**，等使用者同意。第一次刷最可能的問題見 `SENSING_BRINGUP_NOTES.md` §2（ADC 重置脈衝的位元對應是推論）與 §5。

### S1、S2 實機結果（2026-10-28）

- **S1 通過**：ESC（ADC1 ch3、mux 相位 1）閒置約 3228、按住 2120；左 CTRL（ADC2 ch17、相位 0）閒置 3314、按住 2037；彼此不干擾，逾時 0。ADC 重置脈衝沒有弄壞 USB。
- **S2 第一版失敗**：刷入後 USB 裝置狀態 Error、無 HID 介面；另外第一次刷寫曾在映像偏移 `0x10A20` 報寫入錯誤（重試成功，原因不明；之後的刷寫都沒再發生，包含進更新模式後等 5 秒再刷）。救援（ESC＋左 CTRL）成功。
- **S2 第二版（T3 修正：計時逾時、等待迴圈內輪詢 USB）通過**：四顆 ADC 全部可用、沒有逾時、USB 兩次輪詢最長 51 µs、一圈掃描約 2.2 ms（約 450 圈／秒）。第一版失敗的真正原因**未證實**（四顆 ADC 這次都正常，原先「ADC3／4 無回應」的推論不成立）。
- S2 的 `tag 0x0002` 改為每 20 ms 發佈一次：每圈發佈（約 450 次／秒）時，主機分四頁讀取的快照幾乎都被判為「讀到一半被改」。這是掃描發佈端要遵守的規則：快照要停留得比一次完整讀取久。
- **逐鍵量測（R1）**：`tools/magger60_keymap_measure.py` 記錄使用者每個鍵按一次（第三排與最下排由右往左按）。61 次按壓對應 61 個不同槽位，按下時下降約 1000–1300。其餘 15 個槽位無鍵：8 個是 ADC4 同一通道重複取樣（15、17、33、35、51、53、69、71），7 個是沒接感測器的浮接輸入（20、24、40、41、46、50、58，閒置約 1360）。量測表存在 `firmware/core/data/slotmap_measured.json`。
- 本機交叉檢查：量測表與原廠預設對照表 61 個鍵全部吻合（對照結果不進 repo）。
- 下一步：S3（完整 DMA 掃描）；R1 收尾＝把 `km_scan` 的槽位表改由量測表產生，不再依賴 `gen_slotmap.py`／原廠映像。

### S3、S4 實機結果與 M3 收尾（2026-10-28）

- **S3 通過（第一次就成功）**：DMA 錯誤 0、重疊 0；一個 frame（4 個 mux 相位）約 9,300 個 CPU 週期（每相位約 2,200–2,230）＝ 240 MHz 下約 39 µs，**硬體掃描率約 25.8 kHz**；主迴圈實際推進約 **23,100 frame／秒**（S2 約 450 圈／秒）。16 點和除以 16 與 S2 的原始值在 76 個槽位上相符；injected 中斷持續觸發。
- **按鍵驗證**：W、A、S、D（injected 通道，槽位 75、73、72、74）、ESC（18）、左 CTRL（5）按下時各自只有對應槽位下降約 1100–1300，沒有串擾，與量測表一致。（先前文件曾把左 CTRL 寫成槽位 68 是筆誤：68 是右 CTRL。）
- **S4 範圍資料**：`firmware/core/data/slot_ranges_s2.json`（由 `tools/magger60_slot_ranges.py` 從逐鍵記錄產生）。61 個鍵放開約 3102–3451、按到底約 1794–2398，放開到按到底的幅度 998–1365 個計數（中位數 1139）。這是深度模型（R2）擬合的端點資料。
- **收尾**：原廠 1.04 已刷回，backup9 與 backup8 的 bootloader 與設定區逐位元組相同（含 S2 失敗與救援的整段過程）。
- **M3 結論**：L1「看得見」達成。未解的事：S2 第一版失敗的真正原因；第一次 S2 刷寫的 `0x10A20` 寫入錯誤（之後 5 次刷寫都沒再發生）。
- 下一步：R1 收尾（`km_scan` 槽位表改由量測表產生）、UI 分支合併、M4（L2 打得出字）。
