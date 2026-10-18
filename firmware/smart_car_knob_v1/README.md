# ESP32-S3 Smart Car Knob v1

以新版 `smart_car_knob_project_final.md` 為主的可編譯韌體，並合併舊版 XC90 遙控器已實機驗證的 BLE HID 操作習慣。

## 已完成

- ESP32-S3，實機讀回旋轉編碼器 `A=GPIO6` / `B=GPIO5` / `Push=GPIO0`
- SH8601 466x466 QSPI 顯示器與 CST816S 觸控已實作，不再是 headless 模式
- BLE HID 複合式裝置：鍵盤＋滑鼠＋ Consumer/Media Control
- HID 按下與放開各保留 20 ms，避免 Android 車機合併掉報告
- 按住旋鈕並轉動：四頁切換
- 單擊：執行當前頁面焦點
- 500 ms 內雙擊：全域 Back
- 700 ms 長按：全域 Home，並回到導航頁中央焦點
- 開機按住 8 秒：清除 BLE bond 後重啟
- Page 1：上/右/下/左/語音（Consumer Voice Command `0x00CF`）
- Page 2：旋轉送真實 Mouse Wheel，單擊送 Mouse Left Click，觸控拖曳送 Mouse Move
- Page 3：旋轉音量加減，可選上一首/播放暫停/下一首/靜音
- Page 4：Home / Back / Alt+Tab 多工 / Sleep / F10 車輛設定掛點
- BLE 與 UI 分離：NimBLE host 處理通訊，UI task 固定在 Core 1
- UI 狀態已固定為 466x466 觸控座標
- 2026-10-16 已實機燒錄、鏡頭驗證顯示，並通過軟體重開機測試

## 編譯與上傳

Arduino IDE 請選：

- Board: `ESP32S3 Dev Module`
- USB CDC On Boot: `Enabled`（保留開機與面板診斷訊息）
- Flash Size: `8MB`（與原廠 App header 相同）
- PSRAM: `OPI PSRAM`（實機為 8MB embedded PSRAM）
- Partition Scheme: `8M with spiffs (3MB APP/1.5MB SPIFFS)`

需安裝 `NimBLE-Arduino 2.x`。開啟 `smart_car_knob_v1.ino` 後可直接 Verify / Upload。

命令列編譯：

```sh
/Applications/Arduino\ IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli \
  compile --clean \
  --fqbn 'esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc' \
  --build-path smart_car_knob_v1/build-idf51 \
  --export-binaries smart_car_knob_v1
```

已匯出的 App 韌體在：

`build-idf51/smart_car_knob_v1.ino.bin`

燒錄方式：

```sh
# 建議：只寫 App，保留原廠 Bootloader、分割與 NVS
./flash_app.sh /dev/cu.usbmodemXXXX

# 無串列埠時的替代方式：USB-JTAG 只寫 App
./flash_app_via_usb_jtag.sh

# 必要時還原今日讀出的原廠 App
./restore_factory_app.sh /dev/cu.usbmodemXXXX

# 只有真的要替換分割表時才使用，會覆蓋原有 NVS/BLE 配對
./flash_merged.sh /dev/cu.usbmodemXXXX
```

## 466x466 螢幕與觸控實機配置

從原廠韌體執行中的 GPIO matrix 讀回實際接線：

- SH8601 QSPI：`CS=12` / `CLK=10` / `D0=13` / `D1=11` / `D2=14` / `D3=9`
- 顯示器：`RST=8` / `PANEL_EN=17`（GPIO17 必須在 SH8601 指令初始化之前拉高）
- CST816S：`SDA=1` / `SCL=3` / `RST=2` / `INT=4` / I2C `0x15`
- 狀態 LED：`GPIO21`（現階段未由新韌體驅動）

### v12 層次纖維 UI

UI 規格已依 `gemini-code-1791701389511.html` 調整，並從 320x320 網頁邏輯座標等比放大到 466x466：

- 1.2 秒青藍／桃紅雙環開機動畫
- 除觸控板頁外，顯示深灰與藍色雙層 GhostFibers 動畫背景
- 導航頁使用四個無死角三角區域和綠色中央語音鍵
- 觸控板頁使用 26 px 青藍點陣、觸控隆起反饋與右側滾動指示
- 多媒體頁使用 260° CometDial、12 段動態尾跡與百分比音量
- 系統頁使用四象限切割與金色中央菱形
- 換頁動畫節奏 350 ms
- 旋轉音量每格 15%，與 v12 模擬器一致

視覺參數集中在 `UiDesignV12.h`，不需再從 HTML/CSS 逐項轉換。

硬體驅動集中在 `SmartCarKnobBoard.cpp`：

- `boardUiBegin()`：初始化 QSPI 面板、PSRAM framebuffer 與 I2C 觸控
- `boardUiRender()`：繪製開機動畫與四頁 UI
- `boardReadTouch()`：回傳 0..465 的觸控座標

畫面直接用 RGB565 framebuffer 繪製，不需額外安裝 LVGL 或顯示庫。
