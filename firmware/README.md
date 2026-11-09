# Magger60 自製韌體

所有程式都是我們自己設計、自己實測的：感測與深度模型來自實機量測（`core/data/*.json`），HID 與 USB 規格來自公開文件與我們自己錄到的封包。
規格與計畫見 [`CUSTOM_FIRMWARE_PLAN.md`](../CUSTOM_FIRMWARE_PLAN.md)、[`M3_PLAN.md`](../M3_PLAN.md)、[`M4_PLAN.md`](../M4_PLAN.md)。

## 結構

| 路徑 | 內容 |
|---|---|
| `core/` | 純 C11、無硬體依賴的核心：掃描（`km_scan`，76 個槽位的 16 點滑動和）、深度模型（`km_depth_model`）、按鍵引擎（`km_engine`）、預設鍵盤配置（`km_keymap`）、HID 報告組裝（`km_report`）、flash 配置與寫入防護（`km_flash_map`、`km_flash_guard`） |
| `core/gen/km_slot_data.c` | 槽位表，由 `tools/gen_slotmap.py` 從我們實測的 `core/data/slotmap_measured.json` 產生 |
| `core/data/` | 實機量測資料（各槽位讀值範圍、槽位 → 按鍵對照） |
| `hid/` | 廠商類 HID 通道：封包組裝／解析（`'U'`／`'_'`、XOR、checksum）、指令表、bootloader 更新封包、`cmd 3` 與 scope 通道（`0xC5`）的裝置端處理 |
| `usb/` | 自己的 USB 描述元、控制請求處理與 IN 報告送出 |
| `tests/` | 主機端單元測試。預期的封包位元組都是依規格手算出來的，測試不讀任何原廠檔案 |
| `port/n32h48x/` | 連結腳本（app 在 `0x08008000`、上限 `0x08027FFF`）、啟動檔、暫存器層驅動，以及各階段的映像（`d*`、`s*`、`k*`） |
| `tools/` | `run_tests.py`、`arm_smoke.py`、`build_crashtest.py`、`check_image.py`、`gen_slotmap.py`、`gen_depth_cal.py`、突變測試腳本 |

## 怎麼跑

```bash
python firmware/tools/run_tests.py            # 主機端單元測試（需要 gcc；Windows 會自動用 MSYS2 ucrt64）
python firmware/tools/arm_smoke.py            # 交叉編譯 Cortex-M4F 煙霧映像並檢查記憶體配置（需要 arm-none-eabi-gcc、cmake、ninja）
python firmware/tools/build_crashtest.py --target km_k1   # 編某個階段的映像並檢查記憶體配置
```

也可以直接用 CMake：`cmake -S firmware -B build -G Ninja && cmake --build build && ctest --test-dir build`；
交叉編譯加 `-DCMAKE_TOOLCHAIN_FILE=firmware/cmake/arm-none-eabi.cmake`。

Windows 備註：Git Bash 的 PATH 會把不相容的 `/mingw64/bin` 放在前面，導致 gcc 無聲失敗（exit 1、沒有訊息）。
腳本已經把 `C:\msys64\ucrt64\bin` 放到 PATH 最前面；手動呼叫時請自己這樣做。

## 安全規則（寫進程式裡）

- 備份工具只能組出白名單內的唯讀指令；`0xE1`（可寫 bootloader）、`0x06`（可設進 bootloader 旗標）、所有寫入／重置／切換狀態的指令都在黑名單。
- `km_hid_parse` 對 `'_'` 封包也限制長度；`km_hid_build_flash_read` 拒絕超過 512 KB 的讀取。
- 自製韌體**不實作**任意位址寫入；`km_flash_guard` 拒絕任何落在 bootloader 範圍（`0x08000000`–`0x08007FFF`）的寫入。

## 已移除的 M0 程式

M0 時期曾經照著原廠韌體的靜態分析寫過一批核心（濾波、深度查找表、方向確認、觸發狀態機、基準線學習、DKS／MT／TGL）以及與原廠映像逐位元組比對的測試資料。
這些在 M4 被我們自己的設計（`km_depth_model`、`km_engine`、`km_keymap`、`km_report`）取代，已從樹中刪除（PUBLISH_PLAN.md 4A / R5）；需要看舊版請查 git 歷史。
