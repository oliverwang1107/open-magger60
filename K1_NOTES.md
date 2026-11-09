# K1 筆記：自己的 USB 描述元與按鍵報告送出（M4 任務 T-C）

K1 = 只換 USB 描述元、不掃描、不送鍵的映像（`firmware/port/n32h48x/k1_main.c`，目標 `km_k1`）。這份筆記說明描述元怎麼設計、怎麼檢查，以及第一次實機刷寫前要知道的風險。**尚未刷進鍵盤**。

## 1. 設計（全部由 USB 2.0、HID 1.11、HID Usage Tables 自行寫成）

一個複合裝置，三個 HID 介面，端點位址與大小沿用已在實機驗證過的驅動設定（FIFO 配置沒改）：

| 介面 | 用途 | 端點 | 報告 |
|---|---|---|---|
| IF0 | boot 鍵盤（子類別 1、協定 1） | EP1 IN，8 B | 輸入 8 B（修飾鍵、保留、6 鍵）；輸出 1 B（LED） |
| IF1 | 廠商通道（cmd 3、scope 0xC5、進 bootloader） | EP2 IN + EP10 OUT，各 64 B | 無 report ID，64 B 進／64 B 出 |
| IF2 | NKRO 位元圖＋多媒體鍵 | EP3 IN，≤64 B | ID 1：鍵盤，30 B（ID＋29 B 位元圖）；LED 輸出 2 B。ID 2：consumer，3 B（ID＋16 位元 usage） |

- 位元圖：第 n 位元 ＝ 鍵盤頁 usage n，n＝0x00..0xE7（232 位元＝29 B；位元 0..3 不是按鍵，在描述元裡是常數填充、永遠為 0）。**這是暫定格式**，T-B 的 `km_report.h` 如果不同，整合時只要改 IF2 的描述元（格式文件在 `km_usb_reports.h`）。
- 識別碼與字串都集中在 `firmware/usb/include/km/km_usb_ids.h`：VID/PID `19F5:FB49`（沿用，是否可用於公開版是待決問題，標頭裡有寫）、`bcdDevice 0.01`、廠商字串、產品字串、序號。
- 序號：`KM60` ＋ 晶片 UID 三個字（24 個大寫十六進位數字）。跟原廠的序號格式不同。
- 介面 1 的最上層 collection 仍宣告為「Generic Desktop／usage 0」，因為備份、info、scope 工具和 UI（`ui/js/device/ids.js`）都用這一對數字找通道；資料欄位本身宣告在廠商頁 `0xFF4D`。若日後要改成純廠商頁，描述元和那三處要一起改。
- 傳輸間隔：全部 1 ms（`bInterval 4`，高速）；NKRO 之後可改成 1（125 µs）。

## 2. 控制請求（`km_usb_ctrl.c`）

- GET_REPORT：回最後送出的報告（IF0：8 B；IF2：ID 1 為 30 B、ID 2 為 3 B；IF1 回 64 個 0）；只支援 input 類型。
- SET_REPORT：只收 output（LED），長度與 ID 要合理，收下後忽略。
- SET_IDLE／GET_IDLE：IF0、IF2；IF1 回 STALL。SET_PROTOCOL／GET_PROTOCOL：只有 IF0（boot 介面）。
- 其他請求一律 STALL。

## 3. 按鍵報告送出（`usb_hs.[ch]`、`km_usb_in.c`）

`km_usbhs_kbd_ready()`／`send_kbd(report[8])`／`nkro_ready()`／`send_nkro(report, len)`／`kbd_boot_protocol()`。非阻塞；上一包還沒被主機取走（或尚未 configured）就回 `false`，呼叫端下一圈用**最新**狀態再送。完成事件在 `km_usbhs_poll()` 內偵測。主機設了 HID idle 時，驅動以 SOF 計數（約每毫秒）重送最後一包；IF2 只重送鍵盤報告，不重送多媒體鍵。BIOS 若用 boot 協定，`km_usbhs_kbd_boot_protocol()` 會是 true，此時用 IF0。

整合時注意：IF0 與 IF2 都是鍵盤，**只要選一個送**（兩個都送會讓 OS 看到兩把鍵盤同時按同一鍵）。

## 4. 檢查

- 主機測試（`tests/test_usb.c`）：HID 報告描述元解析器（逐項規格，包含頁、usage 範圍、邏輯範圍、旗標、報告長度）、設定描述元走訪器、字串編碼、序號格式、EP0 政策、IN 端點與 idle 重送；走訪器自己也對壞掉的複本做測試（每個報告描述元的每個位元組 ±1 都必須被拒絕）。
- 突變測試：`python firmware/tools/mutate_usb.py`（在暫存複本改一個長度／位址／報告大小／比較，要求測試失敗）。
- 位址稽核：`audit_addrs.py` 對 `km_k1.elf` 與 `km_d4.elf` 的輸出完全相同（沒有新的絕對位址；SOF 只是 `GINTSTS` 的另一個位元）。

## 5. 第一次實機刷寫（K1）的風險

1. **Windows 快取**：Windows 會依 VID/PID/序號記住裝置。這次序號格式與 `bcdDevice` 都變了，所以通常會被當成新裝置、重新讀描述元；但若看到舊的鍵盤／HID 項目或行為怪異，做這些：裝置管理員 → 檢視 → 顯示隱藏的裝置 → 把「鍵盤」「人體學輸入裝置」「通用序列匯流排裝置」底下所有 `19F5` 相關（含灰色）項目解除安裝（勾選刪除驅動程式），拔掉鍵盤再插回。必要時也可在登錄檔 `HKLM\SYSTEM\CurrentControlSet\Enum\USB\VID_19F5&PID_FB49` 底下刪除舊的子項目（需系統管理員）。
2. **救援路徑不變**：K1 保留「USB 命令進更新模式」（D4 的那條）；bootloader 本身（PID `AF1F`）是另一個裝置，不受描述元影響。按住 ESC＋左 CTRL 上電的救援也不變。若新描述元讓 Windows 完全不認，bootloader 路徑仍可用，用 `magger60_flash.py` 把 D4 或原廠映像刷回去。
3. **工具相容**：`magger60_info.py`、`magger60_scope.py` 靠「usage page 1／usage 0」找通道，已保留；但若 hidapi 對新的介面字串或序號有意外，先用 `tools/hid_survey.py` 看列舉結果。
4. **boot 協定與 BIOS**：IF0 的 boot 描述元遵循 HID 1.11 附錄 B.1，K1 不送鍵所以無法在這步驗證打字；IF0／IF2 的 IN 路徑要到 K2 才第一次真的送報告。
5. **idle 重送與 SOF 計數**：驅動假設 `GINTSTS` 的 SOF 位元即使未開啟中斷也會被硬體設定；若不是，idle 重送不會發生（預設 idle 0 時不需要，Windows 也設 0）。
6. **ZLP 路徑**：所有 EP0 描述元的長度都不是 64 的倍數（設定描述元 91 B，報告描述元 63／27／69 B），所以零長度封包的路徑在 K1 完全不會用到。
7. **測試鉤子**：`k1_main.c` 的 `KM_K1_TEST_USAGE`（預設 0）才會在 configured 後約 5 秒送出一次指定按鍵（先按後放），預設編譯完全不送鍵；要用必須另外同意。
8. **時間**：K1 的「秒數」只是迴圈次數估計，不是時鐘。
