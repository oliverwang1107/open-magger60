# USB：主機燈號（P5a）與手把介面（P5b）

> M6 的 USB 部分。白話版：鍵盤現在有兩種「自我介紹」（描述元變體），其中一種多了一支手把；手把介面不通時會自動退回原本的鍵盤；
> 另外鍵盤第一次能「聽到」電腦的 Caps Lock／Num Lock／Scroll Lock 狀態。
> 全部是原創設計，依據 USB 2.0、HID 1.11 規格與 HID Usage Tables；沒有讀、也沒有用任何原廠檔案。
> 證據等級：**H** = 主機測試證明；**U** = 沒有在實機驗證。**這一輪沒有碰硬體，所以所有「實機會怎樣」都是 U。**

## 1. P5a：主機燈號

| 項目 | 內容 |
|---|---|
| 以前 | EP0 的 SET_REPORT（主機送 LED 位元）會被接受，但資料階段的內容被丟掉 |
| 現在 | 驅動把資料階段的每個封包交給 `km_usb_ctrl_out_push()`，階段結束時呼叫 `km_usb_ctrl_out_end()`（都在 `km_usb_ctrl.c`，純邏輯）。**只有「剛好收滿 wLength 位元組、IF2 的報告 ID 正確」的資料才會生效**；太短、太長、被下一個 SETUP 打斷、重複結束、報告 ID 錯、vendor 通道，一律不改狀態 |
| 結果 | `host_leds`：bit0 Num、bit1 Caps、bit2 Scroll（其餘位元丟掉）。`km_usbhs_host_leds()` 讀它，`km_usbhs_led_report_count()` 回報「收到幾次有效報告」（診斷用） |
| 清除時機 | 匯流排重置（USB reset）清為 0；`SET_CONFIGURATION` **不清**（主機換設定時不會重送 LED 狀態）。0 = 「全關或還沒被告知」 |
| 驅動 | `usb_hs.c` 只改兩處：EP0 資料階段把 FIFO 的資料讀出來交給上面兩個函式；EP0 OUT 完成時先呼叫 `_end()`，再送狀態階段 |

證據：**H** `tests/test_usb.c` 的 `test_host_leds()`（256 種 LED 位元組 × 兩個介面、分段送、短/長/錯 ID/錯介面/錯方向/被打斷/重置、64 位元組緩衝溢位），控制請求 fuzz 也會餵隨機資料階段。**U** Windows 實際會在什麼時機送、有沒有送給 IF0 與 IF2 兩個；`led_report_count` 就是用來看這件事。

## 2. P5b：描述元變體

兩種變體，`km_usb_variant_t`：

| | PLAIN（預設） | PAD |
|---|---|---|
| 介面 | IF0 boot 鍵盤、IF1 vendor、IF2 NKRO＋媒體 | 同左，**加 IF3 手把** |
| 端點 | EP1、EP2/EP10、EP3 | 同左，**加 EP4 IN**（16 位元組、1 ms） |
| `bcdDevice` | `0x0001`（沒變） | `0x0002`（Windows 才不會沿用舊快取） |
| 設定描述元長度 | 91（沒變） | 116 |

* **PLAIN 與 M6 之前一個位元組都不差**：`test_variant_plain_unchanged()` 把裝置、設定、qualifier、三個報告描述元的雜湊釘住（`0x4A920842`，是在動手前的程式上算出來的）。
* 變體在開機時由映像決定：`km_usbhs_init_variant(KM_USB_VARIANT_PAD)`；`km_usbhs_init()` 還是 PLAIN。**沒有先 `km_usbhs_set_clock()` 就要 PAD，驅動會悄悄改成 PLAIN**（沒有計時器就沒有自動退回，寧可不給手把）。
* 手把報告 15 位元組，沒有報告 ID，跟 P4（`km_pad`，commit 008e5ed）的 `KM_PAD_REPORT_LEN` 與每個位元組的順序**逐位元相同**：按鈕 2 位元組、hat（0 = 中立、1..8）、LX LY RX RY（有號 16 位元，-32767..32767）、左右扳機（0..32767）。
  報告長度只定義在 `km_usb_reports.h` 的 `KM_USB_PAD_REPORT_LEN`（由按鈕數與軸數算出），描述元用同一組常數產生；改了版面，測試會同時喊。
  `km_usb_pad_pack()` 是 USB 層自己的打包函式（數值超出範圍就拒絕、不寫任何位元組）。P4 的 `km_pad_report_pack()` 對超出範圍的 hat 是「打包成 0」，兩者對合法值結果相同；P10 整合時加一個測試比對兩邊 15 位元組即可。
* 報告描述元和 P4 的 `magger60_pad_map.py descriptor` 一樣是 89 位元組、欄位完全相同，只差三個細節：我在 hat 後面把 Physical 最小／最大與 Unit **都歸零**（P4 的版本只歸零 Unit，Physical 最大值 315 會一路「漏」到軸上，主機可能把搖桿的實體範圍當成 0..315）；hat 後面的補齊用 `81 01`（P4 是 `81 03`，兩者都是常數）；扳機不重複 Report Size。我的版本用獨立的逐項解析器逐位元組檢查（任何一個位元組加一或減一都會被抓到）。
* `GET_REPORT`（IF3）回最後排進 EP4 的報告；IF3 沒有 idle、protocol、輸出報告，這些請求一律 STALL（跟 IF1 vendor 一樣的做法，那是在實機上驗證過的）。

### FIFO 與端點（`km_usb_fifo_plan()`）

字為單位：接收 0x80、EP0 0x20、EP1 0x10、EP2 0x20、EP3 0x20（用到 0xF0，**跟以前完全一樣**）；PAD 變體再加 EP4 0x10 → 剛好 0x100，是 bootloader 用的範圍。驅動直接用這份計畫寫暫存器，測試證明 PLAIN 的數字沒變、各 FIFO 不重疊、每個 FIFO 放得下自己的封包。
**U**：晶片實際有幾個 IN 端點、FIFO RAM 夠不夠，要實機讀。

### 能力診斷（先於任何描述元改動）

`km_usbhs_read_hwcfg()` 讀 `GHWCFG1..4`（偏移 0x44..0x50，唯讀）。`km_usb_hwcfg_decode()`／`km_usb_hwcfg_check()`（純邏輯，測過）把它們翻成「有幾個 IN 端點、有沒有每端點專用 FIFO、FIFO RAM 多大、每個端點方向」並回答 PAD 變體放不放得下。
**欄位位置是照常見的 DWC2 核心寫的，這顆晶片是否相同 = U**，所以診斷映像一定先印原始的四個字，再印解碼結果；請對照參考手冊確認。
**如果診斷說 EP4 放不下**（`verdict` 的 TOO_FEW_IN／IN_DIR／FIFO_SPACE 位元非 0）：**不要上 u5b**；改走計畫書 §10 問題 1 的選項 A（在 IF2 的報告描述元加第二個 collection，report id 3，同一個 EP3；鍵盤報告優先、手把降速）。我沒有先寫選項 A：沒有數字就沒有理由多做一份描述元，而且選項 A 要動 IF2 的描述元（影響鍵盤本身），風險比選項 B 大。診斷結果出來後再做，工作量約是：IF2 描述元一個 collection ＋ `km_usb_in_send_nkro` 多一種報告 ID 3 ＋ 對應的測試與突變。

### 自動退回（`km_usb_guard_t`）

* 計時從**主機第一次匯流排重置**開始（主機不在場就不會有重置，也就不會誤判）；主機重試不會重新計時；收到 `SET_CONFIGURATION(1)` 就永久停止。
* 6000 ms 內沒設定成功 → `km_usb_guard_poll()` 回 true **一次**；驅動 `fall_back_to_plain()`：放掉 D+ 上拉（`DCTL.SDIS`）、關端點、等 300 ms（餵看門狗，主機通常要 100 ms 以上才認定拔除）、改成 PLAIN、重新連上。之後這次開機不再嘗試；下次上電會再試一次 PAD。
* 毫秒時鐘用無號減法，跨 32 位元回捲也正確（測過）。
* 救得了：主機拒絕描述元／不肯設定。救不了：韌體本身當掉（用 ESC＋左 CTRL）；主機有設定成功但 IF3 的驅動壞掉（鍵盤 IF0/IF2 照常，裝置管理員會有黃色驚嘆號，低風險）。
* 演練用：`km_usbhs_rehearsal_refuse_configuration(true)`，PAD 變體時對 `SET_CONFIGURATION(1)` 回 STALL（只對 PAD 變體、只對設定 1），讓主機「永遠設定不起來」。**真正的映像不可以呼叫它。**

### 手把端點沒人輪詢

EP4 有自己的 busy 旗標與 FIFO。主機從不取走報告時：EP4 一直 busy，`km_usb_in_send_pad()` 一直回 false，**鍵盤與 NKRO 照常收送、閒置重送不受影響、不會重送手把報告**（`test_pad_in_endpoint()` 跑了 2000 輪）。`km_usbhs_pad_recover()` 是 EP4 版的卡住復原（和 T-E 的 `km_usbhs_in_recover()` 同一個做法，沒有改後者）；呼叫後下一次送的是**當下**狀態。**U**：DWC2 的 NAK/disable 序列和 T-E 一樣是照程式手冊寫的，沒有實機驗證。

## 3. 診斷映像 `usb_diag`（給 P10 登記）

新檔 `firmware/port/n32h48x/usb_diag_main.c`，一份原始碼三個映像：

| 映像 | `KM_USBDIAG_MODE` | 做什麼 |
|---|---|---|
| `km_u5a` | 0 | PLAIN 描述元（跟日常鍵盤一樣）＋讀 GHWCFG ＋顯示主機 LED 狀態／報告次數 |
| `km_u5b` | 1 | PAD 描述元；EP4 送合成的手把掃描（每個軸一個三角波、每秒一個按鈕與一個 hat 方向）；6 秒退回已啟用 |
| `km_u5c` | 2 | 退回演練：PAD 描述元但拒絕 SET_CONFIGURATION，6 秒後必須變回 PLAIN |

三個都保留 K1 的 vendor 命令 3、scope 0xC5、「從主機進 bootloader」。scope tag `0x0510`，18 個值，版面寫在檔頭，`tools/magger60_padtest.py --hwcfg` 會解碼。
CMake 沒有動（P10 的檔案）。要登記請貼在 `firmware/CMakeLists.txt` 第二個 `foreach(V l30 l30s l30d)`（設定連結選項那一段）**之前**；我在本機貼過，三個映像都編得過、`check_image.py` 通過、未提交：

```cmake
    # M6 P5b: USB interface work of the gamepad on its own (usb_diag_main.c): u5a = capability readout (plain descriptor),
    # u5b = pad descriptor + sweep, u5c = fall-back drill
    foreach(M a=0 b=1 c=2)
        string(REPLACE "=" ";" MP ${M})
        list(GET MP 0 UL)
        list(GET MP 1 UM)
        add_executable(km_u5${UL}.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/usb_diag_main.c)
        target_link_libraries(km_u5${UL}.elf PRIVATE km_usb km_hid)
        target_include_directories(km_u5${UL}.elf PRIVATE core/include)
        target_compile_definitions(km_u5${UL}.elf PRIVATE KM_USBDIAG_MODE=${UM})
        target_compile_options(km_u5${UL}.elf PRIVATE -Wall -Wextra -Werror -Os)
        target_link_options(km_u5${UL}.elf PRIVATE
            -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs
            -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_u5${UL}.map)
        set_target_properties(km_u5${UL}.elf PROPERTIES LINK_DEPENDS ${KM_LD})
        add_custom_command(TARGET km_u5${UL}.elf POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O binary km_u5${UL}.elf km_u5${UL}.bin)
    endforeach()
```

另外給 P10：`m6_main.c` 的 `pad_mode` 開機時選變體（`km_usbhs_set_clock()` 要在 `km_usbhs_init_variant()` 之前）；`km_usbhs_fell_back()`／`km_usbhs_variant()` 給 `DEVICE_STATUS`；手把報告用 `km_usbhs_pad_ready()`／`km_usbhs_send_pad()`（規則跟鍵盤報告一樣：忙就下一圈送當下狀態）。`tests/test_usb.c`、`tools/mutate_usb.py` 已含全部 USB 測試；`mutate_usb.py` 現在預設只編 USB 函式庫與 `test_usb.c`（每個突變約 3 秒），`--full` 才跑整套。

## 4. 主機工具 `tools/magger60_padtest.py`

* `--winmm`：用 Windows 內建的 `joyGetPosEx`（ctypes，不用裝套件）依 USB 識別碼 19F5:FB49 找到手把，顯示六個軸、按鈕、hat。這就是「老遊戲／DirectInput 包裝」看到的東西。**沒有手把時會明講原因與去哪看（裝置管理員、joy.cpl）。** 在這台電腦上實跑過：沒有手把時正確回報找不到。
* `--winmm --identify`：配合 `u5b` 的掃描，量每個 WinMM 軸的週期，自動說出「X 軸 = LX、Y = LY…」（Windows 的軸順序是**量出來的，不是假設**）。
* `--raw`：用 hidapi（要另外裝）讀 IF3 的原始 15 位元組。
* `--hwcfg`：讀 `u5a` 發佈的 GHWCFG 與診斷值（只送 cmd 3 與唯讀的 scope 請求）。
* `--sim`：模擬的手把，任何系統都能看畫面；`--self-test`：沒有硬體的自我檢查（報告版面與韌體標頭一致、週期偵測、WinMM 結構大小、唯讀保證）。

## 5. 實機階梯（每一步都要你同意才刷；第二把鍵盤在手；刷完做備份比對）

救援：**插電時按住 ESC＋左 CTRL** 進更新模式（所有映像都適用）；映像還在跑時也可以用 vendor 命令進 bootloader（`tools/magger60_enterboot.py`）。

| 步驟 | 刷什麼 | 你要看到 | 看到別的怎麼辦 |
|---|---|---|---|
| 0 | 先在電腦上 `python firmware/tools/run_tests.py`、`python tools/magger60_padtest.py --self-test` | 0 failures／self-test ok | 不要刷 |
| 1 (L6-5a) | `km_u5a`（PLAIN 描述元，列舉方式跟平常一樣） | 打字、`tools/magger60_info.py` 正常。`magger60_padtest.py --hwcfg` 印出四個原始字與解碼；`verdict = 0` 表示 PAD 放得下。按 Caps Lock，`LED reports accepted` 加 1、`host LEDs` 變 2（Num Lock = 1、Scroll Lock = 4）；各按幾次、再拔插一次，看 Windows 是否在重新列舉後又送一次 | 鍵盤不能打字／列舉失敗：P5a 的 EP0 改動有問題 → ESC＋左 CTRL，刷回 `km_l51`，回報（這是最需要知道的結果）。`verdict ≠ 0`：停，別往下，改走選項 A。報告次數永遠是 0：主機沒送 LED 報告，要看是哪個介面（回報給我） |
| 2 (L6-5c) | `km_u5c`（退回演練） | 插上後約 6 秒內裝置管理員先出現失敗的項目，接著鍵盤變回正常（bcdDevice 0x0001），可以打字；`--hwcfg` 的 `fell back = 1` | 10 秒後仍不能打字：退回沒有發生 → 拔掉、ESC＋左 CTRL、刷回 `km_l51`，回報。**這一步不通過就不要做步驟 3** |
| 3 (L6-5b) | `km_u5b`（PAD 描述元＋掃描） | 鍵盤照常打字；`joy.cpl`（執行 → `joy.cpl`）出現 Magger60，內容頁看到六個軸在動、按鈕與 hat 輪流亮；`magger60_padtest.py --winmm --identify` 印出軸的對應表；`--hwcfg` 的 `fell back = 0`、`pad reports sent` 一直增加 | 裝置管理員黃色驚嘆號但能打字：低風險，把 Windows 的錯誤碼寫下來；打不了字：等 6 秒應自動退回，沒有就 ESC＋左 CTRL |
| 4 | 把 `km_u5b` 換成 `km_u5a` 再換回 `km_l51`；到裝置管理員「顯示隱藏的裝置」把舊項目移除 | 回到日常狀態，備份比對 bootloader 與 app-ID 不變 | — |

整合進正式映像（P10 的 `km_m63`）時，`pad_mode` 預設關，行為與 `km_l51` 完全相同。

## 6. 風險清單

| 風險 | 嚴重性 | 說明／對策 |
|---|---|---|
| EP0 資料階段改動讓列舉失敗 | 中 | 只在 SET_REPORT 資料階段與 `_end()` 呼叫處動到驅動；狀態階段順序沒變。步驟 1 專門驗；ESC＋左 CTRL 救援 |
| 晶片沒有第 5 個 IN 端點／FIFO 不夠 | 中 | 步驟 1 先讀；不夠就不上 PAD，改選項 A（尚未實作，見 §2） |
| `GHWCFG` 欄位位置與常見 DWC2 不同 | 低 | 一定先印原始字；解碼只是輔助。對照手冊確認 |
| PAD 描述元被主機拒絕 | 高 | 6 秒自動退回（主機測試完整演練）；`pad_mode` 預設關；步驟 2 先驗退回 |
| 退回機制本身在實機失效（D+ 上拉切換、300 ms 間隔不夠主機認定拔除） | 中 | 步驟 2 專門驗；失敗就 ESC＋左 CTRL。**U**：我沒有實機證據表示 SDIS 切換後主機一定重新列舉 |
| 退回把「慢速主機」誤判（BIOS／睡眠後） | 低 | 計時從第一次重置開始；一次開機只退回一次；PLAIN 永遠可用。代價只是那次開機沒有手把 |
| 主機不輪詢 EP4 | 低 | 不影響鍵盤（測過）；`km_usbhs_pad_recover()` 可用，**U** |
| Windows 對 `bcdDevice` 變更的快取 | 低 | 兩種變體各用自己的值；看到舊項目就在裝置管理員移除 |
| 軸的順序在不同遊戲不同 | 低（已知） | 依 usage 編號；`--identify` 實測；SDL 對應字串在 P4 的工具 |
| 只吃 XInput 的遊戲看不到 | 已知 | 用 pad_mode = 2（§8） |

## 7. 沒做／沒驗證的

* 選項 A（IF2 加 collection）沒有實作，等步驟 1 的數字。
* XInput（P11）見 §8（已寫完，**完全沒有實機驗證**）。
* `usb_hs.c` 的所有新增部分只通過 ARM 編譯（所有映像都編得過、`check_image.py` 通過），**沒有任何一行在實機上跑過**；可由主機測試證明的邏輯都已搬進 `usb/src/`。
* `km_usbhs_pad_recover()`、`fall_back_to_plain()` 的暫存器順序是照 DWC2 程式手冊寫的（U）。

## 8. P11：XInput 風格手把（`pad_mode = 2`）

白話：給「只吃 XInput（Xbox 手把）」的遊戲。設定裡把手把類型選「Xbox 相容」→ 儲存 → 拔插 USB，鍵盤就用另一組描述元（`bcdDevice 0x0003`）：IF3 不再是 HID 手把，而是 Xbox 360 手把那種「廠商自訂類別」的介面。**和 HID 手把互斥**（同一個 IF3、同一個 EP4，不會同時出現兩支）。其他三個介面（打字、設定通道、NKRO）一個位元組都沒變。
全部依據公開知識（Linux `xpad` 驅動的文件、各種開源 XInput 複製品觀察到的格式、USB 2.0）；**沒有讀、也沒有用任何原廠或 Microsoft 的檔案**。證據等級同上：**H** = 主機測試，**U** = 沒有實機驗證。**這一輪沒有碰硬體，所有「Windows 會怎樣」都是 U。**

### 8.1 描述元（`km_usb_desc.c`）

| 項目 | 內容 |
|---|---|
| 裝置 | 同 PLAIN，只有 `bcdDevice = 0x0003`（H） |
| 設定描述元 | 131 位元組（PLAIN 91 + IF3 的 40）；IF0..IF2 與 PLAIN 逐位元相同（H） |
| IF3 | class `0xFF`、subclass `0x5D`、protocol `0x01`、2 個端點；接著一個 17 位元組的「類別專屬描述元」（type `0x21`）：`00 01 01 25 <IN 位址> 14 00 00 00 00 13 <OUT 位址> 08 00 00`；然後 EP4 IN（`0x84`）與 EP4 OUT（`0x04`），都是 interrupt、32 位元組、1 ms |
| 沒有的東西 | HID 描述元、報告描述元、HID 類別請求：對 IF3 的這些請求一律 STALL（H）。廠商請求（例如 `0xC1 0x01`）也一律 STALL——我不知道正確答案，不亂編 |
| 為什麼是這組 class 三元組 | Windows 的 XInput 驅動（`xusb`）依公開資訊是靠「相容 ID `USB\Class_FF&SubClass_5D&Prot_01`」綁定。複合裝置時 `usbccgp` 會替 IF3 產生一個子裝置，這個相容 ID 也會出現在子裝置上（**U**） |

類別專屬描述元的內容是照「真正的有線 Xbox 360 手把會送出什麼」的公開觀察寫的：版本 1.00、`0x25`＋IN 端點＋20（報告長度）、`0x13`＋OUT 端點＋8（訊息長度）。端點位址換成我們的（`0x84`／`0x04`）；**驅動會不會看這幾個位元組、看了之後端點位址不同會不會不開心 = U**。

### 8.2 報告（`km_usb_reports.h`、`km_usb_in.c` 的 `km_usb_xinput_pack()`）

20 位元組輸入報告：`[0]=0x00 [1]=0x14`；`[2]` 方向鍵上／下／左／右＋Start＋Back＋L3＋R3；`[3]` LB／RB／Guide／（空）／A／B／X／Y；`[4][5]` 左右扳機 0..255；`[6..13]` LX LY RX RY（有號 16 位元，**+Y 是往上**）；其餘 0。
從現有手把設定轉換（同一份 `km_pad` 狀態，所以按鍵綁定、曲線、死區全都沿用）：

* 按鈕 1..11 → A B X Y LB RB Back Start L3 R3 Guide（UI 的鍵帽文字本來就是這些名字）。**按鈕 12..16 在 XInput 沒有對應，丟掉**（UI 會提醒）。
* 十字方向：hat 的 8 個方向 → 上下左右四個位元（斜向設兩個）。
* 搖桿：HID 版 LY／RY 是「負 = 上」，XInput 是「正 = 上」，所以 LY／RY 取負號；範圍仍是 −32767..32767。
* 扳機：0..32767 → 0..255，四捨五入，單調遞增。
* 輸入不合法（hat > 8、軸 −32768、扳機 > 32767）→ 拒絕，不寫任何位元組。
證據：**H** `test_usb.c` 的 `test_xinput_*`：每個按鈕、每個 hat 方向、扳機 0..32767 全掃、2 萬組隨機狀態與獨立解碼比對、描述元用獨立走訪器逐層檢查、20 萬個隨機 SETUP 的 fuzz、三個突變（LY 符號、Start／Back 對調…）都被抓到。

### 8.3 主機送來的震動／燈號（EP4 OUT）

主機會送 8 位元組震動訊息（`00 08 00 大馬達 小馬達 00 00 00`）和 3 位元組燈環訊息（`01 03 圖樣`）。我們**只記錄**（`km_usb_xinput_out()`，`km_usbhs_xinput_rumble()` 可讀），鍵盤沒有馬達；格式不對的一律丟掉（H）。`usb_hs.c` 新增：第 4 號 OUT 端點的啟用／接收／重新武裝（**U**，只過 ARM 編譯）。

### 8.4 選擇與安全網

* `device.pad_mode = 2`（設定協定本來就接受這個值）；`m6_main.c` 開機時讀一次：`2` → `KM_USB_VARIANT_XINPUT`。映像仍是 `km_m6`（`KM_M6_PAD=1`），**沒有新增映像**；`km_m6a` 永遠沒有手把介面，不受影響。`DEVICE_STATUS[2]`（實際生效的模式）現在可能是 2。
* 6 秒自動退回與 HID 版是同一個機制（`km_usb_guard_t`、`fall_back_to_plain()`，H 測過）：**主機不肯設定（SET_CONFIGURATION）才會退回**。如果主機設定成功但 Windows 沒有替 IF3 找到驅動（裝置管理員黃色驚嘆號），鍵盤的 IF0／IF2 仍然正常，但不會自動退回；到設定頁改回「標準手把」、儲存、拔插即可，或 ESC＋左 CTRL 進更新模式。
* 能力檢查：XINPUT 變體另外要求 EP4 兩個方向都能用（`KM_USB_HW_OUT_DIR`，H）。

### 8.5 不確定、沒驗證的清單（誠實版）

1. **Windows 會不會替複合裝置的 IF3 載入 XInput 驅動**（U）。公開資訊說相容 ID 是 `USB\Class_FF&SubClass_5D&Prot_01`；但我沒有 Microsoft 的文件可以核對，也沒有硬體。若不成功，下一步是加 Microsoft OS 1.0 描述元（字串索引 `0xEE`＋延伸相容 ID 請求，在 IF3 宣告相容 ID `XUSB10`）——這是公開文件的機制，但要動 EP0 的字串／廠商請求處理，所以先不做，等實機結果。
2. 真手把還有第二個介面（class `0xFF`／subclass `0xFD`／protocol `0x13`，Xbox 的「安全」介面）和 Windows 的簽章驗證握手。公開的 XInput 複製品多半只有單一介面就能在 Windows 上被遊戲讀到，但**我沒辦法保證這顆 Windows 版本接受**（U）。若某些遊戲或 Game Bar 不認，這是首要嫌疑。
3. 類別專屬描述元的內容與端點位址（§8.1）（U）。
4. EP4 OUT 的 DWC2 暫存器順序（U）；晶片 EP4 是否真的雙向（診斷映像 `km_u5a` 會印 GHWCFG，`hwcfg_check` 會判斷）。
5. 回報率：1 ms（`bInterval 4` 在高速下 = 1 ms）。XInput 本身沒有更高的輪詢要求。
6. 搖桿死區：XInput 遊戲通常自帶死區（約 7849／8689 的公開常數）；鍵盤這邊的死區設定與它疊加，所以「最小可感」的搖桿值可能比 HID 版大。
7. 這顆鍵盤沒有 Xbox 的 rumble 馬達，遊戲震動只會被記錄。

### 8.6 實機測試步驟（每一步要使用者同意才刷；有第二把鍵盤）

0. 電腦上：`python firmware/tools/run_tests.py`（0 failures）、`cd ui && npm test`、`python tools/magger60_xinputtest.py --self-test`。
1. 刷 `km_m6`（日常映像）。**不要用 `km_m6a`**（它永遠沒有手把介面）。刷完照 `local/apply_profile.sh`。
2. 設定頁 → 手把：先確認 HID 模式沒壞（類型選「標準手把」、開啟、儲存、拔插；`joy.cpl` 看得到；`tools/magger60_padtest.py --winmm`）。這一步是 P5b 的老驗證，沒過就不要做後面。
3. 類型改成「Xbox 相容」→ 儲存到鍵盤 → 拔 USB 再插。預期：打字照常；6 秒內裝置管理員多一個裝置（可能叫「Xbox 360 Controller for Windows」或「未知裝置」）。
4. `python tools/magger60_xinputtest.py`：按綁定到 A／搖桿的鍵，應看到 `A`、`LX`…變化；按深一點搖桿值變大；扳機 0..255。`--rumble` 可確認 Windows 的震動呼叫不會出錯。也可以用瀏覽器的 Gamepad 測試網頁（Chrome 會顯示「Xbox 360 Controller (STANDARD GAMEPAD)」）。
5. 看不到手把、或裝置管理員有黃色驚嘆號：記下「錯誤碼」與「硬體 ID／相容 ID」（裝置管理員 → 內容 → 詳細資料 → 硬體識別碼、相容識別碼）。這兩串字就是決定下一步（§8.5 第 1、2 點）的關鍵資料。
6. 結束：設定改回「標準手把」或「關閉」、儲存、拔插；裝置管理員「顯示隱藏的裝置」把舊項目移除。
