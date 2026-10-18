# XC90 智慧車用旋鈕

以 ESP32-S3 為核心的車用藍牙控制旋鈕，搭配 1.5 吋、466 × 466 圓形 IPS 觸控螢幕。裝置透過 BLE HID 模擬鍵盤、滑鼠與多媒體控制器，不需在 Android 車機安裝 APK。

## 設計概念

- 實體旋鈕與觸控螢幕各自獨立操作，避免畫面焦點與實體輸入互相干擾。
- 按住旋鈕並轉動可切換四個頁面；短按為確認、雙擊為返回、長按為首頁。
- 介面以純黑背景搭配青藍、桃紅、綠色及金色高對比元素，適合車內與夜間使用。
- 60 秒未操作時只關閉螢幕，BLE 維持連線；旋轉、按壓或觸控即可喚醒。
- 外殼依 Volvo XC90 中央區域的長條配置設計，3MF 內含三個模型零件。

## 四個操作頁面

1. **導航**：順時針送出鍵盤向下鍵，逆時針送出向上鍵；觸控區直接送出方向鍵或語音指令。
2. **觸控板**：旋鈕控制滑鼠滾輪，觸控螢幕模擬滑鼠移動，短按為滑鼠左鍵。
3. **多媒體**：旋鈕控制音量，觸控按鈕提供上一首、播放／暫停、下一首與靜音。
4. **系統快捷**：旋鈕維持鍵盤上下選擇，觸控按鈕提供首頁、返回、多工、休眠及車輛設定快捷鍵。

## 專案內容

- [`firmware/smart_car_knob_v1/`](firmware/smart_car_knob_v1/)：ESP32-S3 Arduino 韌體與燒錄腳本。
- [`models/xc90-phone-remote-v13.3mf`](models/xc90-phone-remote-v13.3mf)：三件式外殼模型。
- [`docs/model-preview.png`](docs/model-preview.png)：3MF 內嵌預覽圖。
- [`HARDWARE.md`](HARDWARE.md)：硬體、緊固件及已確認接腳。

## 快速開始

Arduino IDE 選擇 `ESP32S3 Dev Module`，設定如下：

- Flash Size：8 MB
- PSRAM：OPI PSRAM
- USB CDC On Boot：Enabled
- Partition Scheme：8M with SPIFFS
- 額外函式庫：NimBLE-Arduino 2.x

開啟 `firmware/smart_car_knob_v1/smart_car_knob_v1.ino` 後即可編譯。建議只燒錄 App 分區，以保留原廠 bootloader、partition table 與 NVS：

```sh
cd firmware/smart_car_knob_v1
./flash_app.sh /dev/cu.usbmodemXXXX
```

詳細編譯、燒錄及接腳資訊請參閱 [`firmware/smart_car_knob_v1/README.md`](firmware/smart_car_knob_v1/README.md)。

## 3D 列印與組裝

3MF 使用毫米為單位，包含三個模型物件：

- 兩個長形外殼零件，最大平面範圍約 40.81 × 188.24 mm。
- 一個圓形零件，最大平面範圍約 40.99 × 40.99 mm。

切片前請依實際材料與印表機檢查方向、支撐、熱熔螺母孔及螺絲長度。模型與硬體裝配以實物試裝結果為準。

## 專案狀態

韌體已在 ESP32-S3 實機完成顯示、觸控、旋鈕、休眠喚醒與燒錄測試。BLE HID 的最終相容性仍會受 Android 車機的藍牙堆疊與按鍵映射影響，建議在目標車機完成配對驗證。
