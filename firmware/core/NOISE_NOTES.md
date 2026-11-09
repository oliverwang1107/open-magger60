# 感測雜訊量測（M6 P12a）：診斷映像 `km_noise` ＋ `tools/magger60_noise.py`

目的：量出 **Hall 感測器的真實雜訊**，才知道濾波要往哪裡優化（`M6_PLAN.md` §6.3）。現在引擎用的雜訊數字（±15 計數、σ 8.7）是假設與早期量測，
而且是「平均過」之後的值；原始每個 frame（約 40 µs）的雜訊、有沒有低頻漂移、有沒有 LED／USB 的週期成分、鍵與鍵之間有沒有共同的雜訊，
**都沒有資料**。這份文件說明怎麼量，量完要回答什麼問題。

**這是診斷用映像，不是日常映像。** 它行為跟 L3-0（`km_l30`）一樣（能打字），只是多了「把指定槽位的原始取樣記到 RAM、再讀回 PC」的功能。
沒有燈、沒有設定儲存、沒有校正儲存。唯一的 flash 寫入是 enter-bootloader 的開機旗標（所有映像從 D4 起都有，工具用它來刷回去）。

## 1. 組成

| 檔案 | 作用 |
|---|---|
| `core/include/km/km_capture.h`、`core/src/km_capture.c` | 擷取緩衝（32 KB RAM）。每個 frame 只**讀** DMA 字、寫自己的緩衝，不碰掃描鏈、暫存器、flash、按鍵路徑 |
| `hid/include/km/km_scope.h`、`hid/src/km_scope.c` | scope 指令新增擷取 op（`want_tag` 0xC000..0xCFFF）。沒有註冊擷取（所有日常映像）時，這些 tag 的回應與以前完全相同；以前合法的每個封包回應不變（`tests/test_capture.c` 用舊的 golden 全部重驗） |
| `hid/include/km/km_capture_scope.h` | 把 `km_capture` 接到 scope 的標準膠水（純標頭檔） |
| `port/n32h48x/noise_main.c` | 診斷映像（`m5_stab_main.c` 的副本＋擷取）。build string 帶 `KMFW-NOISE` |
| `tools/magger60_noise.py` | 主機工具：下指令、讀回、存檔、統計、報告；`--self-test`、`--sim` |
| `tests/golden/capture_vectors.h` | 擷取 op 的 golden 封包，由 `firmware/tools/gen_capture_vectors.py` 用 Python 模型產生，C 測試與工具各自驗證 |

### 擷取的行為

- 選 1..4 個槽位（槽位編號同 `km_scan`，鍵名對照見 `core/data/slotmap_measured.json`）；緩衝共 16384 個 u16：
  4 槽位 → 4096 frame（約 0.17 s），1 槽位 → 16384 frame（約 0.7 s）。
- 存的值：bit 0..11 = 原始 12 位元取樣，bit 15 = 條件旗標（這個 frame 之前有 USB 封包被處理：任何 vendor 請求，或送出了按鍵報告）。
- 模式：one-shot（記滿自動停）；ring（一直覆寫最舊的，直到停止，讀出從最舊開始）。
- 正在記錄時不能讀（回 0 個值）、不能改選擇；每次 arm 的 run 計數器 +1，讀到一半被重新 arm 工具會發現並報錯。
- 取樣率由「第一個與最後一個 frame 的週期計數器」算出，工具存進檔案，所以頻譜的 Hz 軸是實測的。
- 讀回：每頁 25 個值，16384 個值約 656 次請求，通常 1–2 秒。

### 檔案格式（KMTR v1；`filter_lab`／`tests/filter_lab.c` 也讀它）

小端。`"KMTR"`、u16 版本=1、u16 欄數、u32 frame 數、f32 取樣率 Hz、u16 旗標(0)、u16 保留(0)、欄數個 u8 槽位號（補零到 4 的倍數），
然後 frame 數 × 欄數 個 u16，frame 優先。每個 `.kmtr` 旁有一個 `.json`（情境、按鍵名、時間、取樣率、條件）。慢速記錄是 `.csv`。
**只放我們自己的鍵盤量到的資料，不放廠商資料。**

## 2. 建置與上板（由主導者做，我沒有碰硬體）

建置（任何有 ARM 工具鏈的機器；MSYS2 `C:\msys64\ucrt64\bin` 在 PATH 最前面）：

```
python firmware/tools/build_crashtest.py --target km_noise --build-dir build_noise
```

做出 `build_noise/km_noise.bin`（約 22 KB）。上板跟其他診斷映像一樣：先 `tools/magger60_image.py build --app build_noise/km_noise.bin --settings <備份的 settings_0x08028000.bin> --out images/noise.bin`，
`tools/magger60_flash.py` 先 dry run 再 `--run --confirm`（流程與備份要求見該工具的說明與 `BEFORE_KEYBOARD_ARRIVES.md`）。刷完 `tools/magger60_info.py`
應該看到 build string 以 `KMFW-NOISE` 結尾。量完刷回日常映像（`km_l51`）。救援方式不變（插電時按住 ESC＋左 CTRL）。

CMake 目標已經（為了能本地建置）加在 `firmware/CMakeLists.txt` 的 Generic 分支最後；整合者可以搬走，內容如下：

```cmake
add_executable(km_noise.elf port/n32h48x/app_startup.c port/n32h48x/usb_hs.c port/n32h48x/flash_boot.c port/n32h48x/reset_cause.c port/n32h48x/noise_main.c)
target_link_libraries(km_noise.elf PRIVATE km_sense km_core km_usb km_hid)
target_include_directories(km_noise.elf PRIVATE core/include port/n32h48x)
target_compile_options(km_noise.elf PRIVATE -Wall -Wextra -Werror -Os)
target_compile_definitions(km_noise.elf PRIVATE "KM_M5_NAME=\"KMFW-NOISE\"")
target_link_options(km_noise.elf PRIVATE -T${KM_LD} -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/km_noise.map)
set_target_properties(km_noise.elf PROPERTIES LINK_DEPENDS ${KM_LD})
add_custom_command(TARGET km_noise.elf POST_BUILD COMMAND ${CMAKE_OBJCOPY} -O binary km_noise.elf km_noise.bin)
```

`firmware/tools/build_crashtest.py` 只多了一個 `--target` 選項值 `km_noise`。

## 3. 量測前的準備

1. 鍵盤直接插主機（不要接 hub），Windows 其他程式不要搶這個 HID 介面（`magger60_scope.py --run`、soak 工具先關掉）。
2. `python tools/magger60_noise.py --self-test` 應該 OK（確認工具本身沒壞）。
3. `python tools/magger60_noise.py --list` 看情境。每個情境先用 `--sim` 跑一遍熟悉流程：`python tools/magger60_noise.py --sim --scenario A --slow-seconds 2`。
4. 實機指令都是 `python tools/magger60_noise.py --run --scenario X --out noise_data`。工具只送 cmd 3 查詢與 scope 請求；build string 沒有 `KMFW-NOISE` 它**不會**送任何擷取指令。
5. 紀錄環境：室溫大致、鍵帽有沒有裝、USB 孔（主機板後面 vs 前面板）、筆電有沒有接電源。把這些寫在 `noise_data/README.txt`。

## 4. 情境清單（照順序做；每個情境結束工具會印報告，檔案在 `--out` 下的子目錄）

| 代號 | 情境 | 指令 | 人要做的事 | 約時間 |
|---|---|---|---|---|
| **A** | 全部按鍵靜止 | `--scenario A` | 手離開鍵盤。先 60 秒慢速記錄（76 槽位約 10 Hz），再逐組 4 顆快速擷取全部 61 顆鍵（16 組） | 約 2 分鐘 |
| **B** | 單鍵壓在不同深度 | `--scenario B --key G` | 照提示把 G 壓到 0.5／1.0／1.5／2.0／2.5／3.0 mm 和到底，每個深度保持 30 秒不動。**最好用墊片當硬擋**（在鍵帽裙下塞一疊紙卡，手指穩穩壓住），否則手抖會算進雜訊；沒有墊片也能做，報告仍會列「平均計數 vs σ」 | 約 5 分鐘 |
| **C** | USB 忙碌 | `--scenario C --keys ESC,G,SPACE,ENTER` | 手離開鍵盤。先 10 次「擷取時主機完全不發封包」，再 10 次「擷取時主機連續發請求」 | 約 1 分鐘 |
| **D** | 敲別的鍵 | `--scenario D` | D0 手離開（參考）；D1 在鍵盤另一側亂打字，不碰 ESC/ENTER/SPACE；D2 只猛敲 G（約每秒 5 次、敲到底），量它的鄰居 F/H/T/B；D3 停手（鄰居參考） | 約 3 分鐘 |
| **E** | 暖機 | `--scenario E --warm-minutes 30` | 鍵盤要在剛插上的狀態（之前拔掉至少 1 小時最好）。手離開，30 分鐘不要碰；第 0、5、10、20、30 分鐘各一次快速擷取 | 30 分鐘 |

沒有做的（等 `fw-led-persist` 合併、燈光存在之後再加）：LED 靜態／動畫／串流條件下的雜訊。`km_noise` 沒有燈，這組之後在有燈的診斷映像上補。
邊角鍵 vs 中央鍵的 σ 分布（計畫的 (i)）已含在 A 裡（逐鍵報告＋列出 σ 最大／最小／中位數）。

每個情境結束後請把整個 `noise_data` 資料夾留著（不要刪），P12b 的評估工具會讀這些 `.kmtr`。

## 5. 報告怎麼看

每鍵一行：`mean`（平均計數）、`sigma`（原始取樣標準差）、`p-p99.9`（99.95 與 0.05 百分位的差，約等於峰對峰）、`sigma/16`（引擎實際看到的 16 點平均後的 σ，現行假設是 8.7）、
`color`（顏色係數）、`spk/s`（孤立突波每秒幾次）、方差在四個頻帶（<100 Hz／<1 kHz／<5 kHz／>5 kHz）的占比。

**顏色係數** = `σ_16點平均` ÷ `(σ_原始 / 4)`。白雜訊平均 16 點會剛好 ÷4，係數 ≈ 1；係數明顯 > 1（例如 2–3）表示雜訊有**低頻成分**，平均濾不掉。
這是最重要的一個數字：它決定「再加濾波」有沒有用，還是要靠別的辦法（共模扣除、漂移追蹤）。

其他輸出：「窄線」（>8 倍鄰近頻譜的譜線：50 Hz 的倍數是 LED frame，1 kHz 是 USB frame；出現了就是週期干擾）；
`by condition`（情境 C/D 的各條件比較：σ、峰對峰、突波率、共模占比）；慢速記錄的 σ 與每分鐘漂移。

要回答的問題與看哪裡：

| 問題 | 看哪裡 |
|---|---|
| 雜訊是白的嗎？ | 顏色係數、頻帶占比 |
| 有沒有 LED／USB 的週期成分？ | 窄線；情境 C 的 quiet vs busy |
| 鍵與鍵之間相關嗎（共模占多少）？ | 情境 D 的 `common`（1＝完全一樣、約 0.25＝四顆互相獨立） |
| 各鍵 σ 差多少？ | A 的 `sigma over keys`（worst / median） |
| 暖機漂移多快？ | E 的慢速 σ 與每分鐘漂移 |
| 雜訊與深度的關係？ | B 的各深度 σ |

## 6. 已知限制（不要把它們當成結論）

- 擷取是 0.17–0.7 秒的片段，**看不到比約 6 Hz 更慢的變化**；慢的部分靠慢速記錄（sum/16、約 10 Hz）。長時間統計是「很多段片段拼起來」，段與段之間有空隙。
- 條件旗標只說「那個 frame 前有 USB 封包被處理」，不分是哪種封包。
- 每次最多 4 顆鍵，共模／串擾只能在這 4 顆內看（情境 D 選了鄰居）。
- 工具的頻譜與統計是純 Python 實作（`--self-test` 對已知訊號驗過），跑 61 鍵報告約十幾秒。
- 沒有溫度感測器資料；暖機只能看計數漂移，不能分辨是溫度還是電路。
