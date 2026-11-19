# 穿戴式視覺輔助系統整合指南

本文件說明目前專案框架整合了哪些開源專案、每段程式碼對應哪些硬體、如何接線與燒錄、手機 App 如何連線，以及目前已完成與尚未完成的功能邊界。整體設計採用分級架構：胸前超音波急煞硬體與手機端邊緣 YOLO 組成**即時避障通道**；胸前相機高解析拍照、Gemini 描述與語音播報組成**認知大腦通道**。

> **重要定位**：目前內容是可編譯的研究原型與整合骨架，不是醫療器材，也不是可以獨立取代白手杖、陪同者或專業導盲訓練的安全系統。所有戶外測試都應在熟悉場地、由陪同人員監護下進行。

## 1. 目前實際整合的開源專案

目前程式碼沒有直接複製任何 GitHub repository 的整個原始碼，而是採用官方文件、開源專案的硬體設定與架構概念，再由本專題建立較小的獨立原型。這樣做可以避免把不必要的功能整包帶入，也較容易在專題報告中清楚說明修改內容與授權來源。

| 開源專案或套件 | 在本專案中的角色 | 目前是否直接作為依賴 | 相容性與注意事項 |
| :--- | :--- | :---: | :--- |
| [`espressif/esp-who`](https://github.com/espressif/esp-who) | 參考 ESP32-S3-EYE 的官方板級支援、相機與影像串流架構 | 否 | 官方範例較偏 ESP-IDF。若要使用其完整 `object_detect`，建議另開 ESP-IDF 分支，不要與目前 Arduino sketch 混用。 |
| [`espressif/esp-bsp`](https://github.com/espressif/esp-bsp) | 取得 ESP32-S3-EYE v2.1/v2.2 的相機 GPIO 與 BSP 設定 | 否 | 本專案的胸前相機程式使用官方 BSP 標頭檔公布的相機腳位；官方 S3-EYE 的 GPIO 已被相機、LCD、音訊與按鍵大量佔用。 |
| [`arkhipenko/esp32-cam-mjpeg`](https://github.com/arkhipenko/esp32-cam-mjpeg) | 參考 MJPEG multipart HTTP 串流模式 | 否 | 本專案自行實作 `/`、`/capture`、`/status`，沒有將該 repository 直接放進專案。 |
| [`ultralytics/ultralytics`](https://github.com/ultralytics/ultralytics) | 參考 YOLOv8n / YOLO11n 輕量化物件偵測與邊緣部署規範 | 否 | 模型於手機端由 ONNX Runtime Web 進行推論。 |
| [`microsoft/onnxruntime`](https://github.com/microsoft/onnxruntime) | 手機 App 內建之 WebAssembly 與 WebGPU 硬體加速邊緣推論引擎 | 是 | 透過 `onnxruntime-web` 在手機本機執行離線 YOLO 物件偵測。 |

## 2. 專案資料夾與功能狀態

```text
VisualAssist_System/
├── README.md
├── INTEGRATION_GUIDE.md
├── platformio.ini
├── chest_vision/
│   └── chest_vision.ino
└── mobile_app/
    ├── package.json
    ├── pnpm-lock.yaml
    ├── capacitor.config.ts
    ├── vite.config.ts
    ├── tsconfig*.json
    ├── index.html
    ├── .env.example
    └── src/
        ├── App.tsx
        ├── main.tsx
        ├── styles.css
        ├── types.ts
        └── services/
            ├── cameraService.ts
            ├── edgeDetector.ts
            ├── geminiService.ts
            └── ttsService.ts
```

目前功能狀態如下：胸前端已包含官方 ESP32-S3-EYE 相機腳位、Wi-Fi station 連線、fallback AP、MJPEG 串流、單張 JPEG 擷取、第 0 層超音波急煞取樣與 `/status` 遙測端點。手機 App 包含即時畫面顯示、即時邊緣 YOLO 動態追蹤（含中央安全錐與左中右方位過濾）、第 0 層急煞聯動監控、第 2 層按需 Gemini 深度情境描述與完全離線 TTS 語音。

手機端第 1 層邊緣推論採用 `edgeDetector.ts`，已實作空間方位切割（`left` / `center` / `right`）、碰撞急迫度估算（`threatLevel`）與 3 秒語音冷卻機制。打包為 Android 原生 APK 時，模型權重（ONNX / TFLite）可直接放置於靜態資源目錄或原生 assets 中，完全無需依賴外網，隨裝即用。

## 3. 硬體採購清單與程式碼對應

| 目的 | 建議材料 | 數量 | 估計單價 | 對應程式碼 | 採購搜尋關鍵字 |
| :--- | :--- | ---: | ---: | :--- | :--- |
| 胸前主機 | Espressif ESP32-S3-EYE v2.1/v2.2 | 1 | NT$1,000–1,800 | `chest_vision/chest_vision.ino` | `ESP32-S3-EYE v2.2` |
| 前方急煞測距 | JSN-SR04T 防水超音波模組 (或 HC-SR04) | 1 | NT$180–400 | `readUltrasonicCm()` | `JSN-SR04T-2.0 防水超音波` |
| 急煞回饋輸出 | 3V 蜂鳴器或扁平震動馬達模組（含 MOSFET 驅動） | 1 組 | NT$30–150 | `EMERGENCY_BRAKE_PIN` | `3V 有源蜂鳴器 MOSFET driver` |
| 連接與擴充 | 麵包板、端子台、杜邦線、電阻分壓 | 1 組 | NT$100–300 | 硬體接線 | `ESP32 杜邦線 端子擴展板` |
| 隨身供電 | 5V 小型行動電源 | 1 | NT$250–600 | USB 供電 | `輕量 5V 行動電源` |

## 4. 胸前 ESP32-S3-EYE 接線與網路

官方 ESP32-S3-EYE v2.1/v2.2 的相機已經接在板上，因此本專案不要求您接 OV2640 排線。`chest_vision.ino` 使用官方 BSP 公開的主要相機 GPIO：XCLK 15、PCLK 13、VSYNC 6、HREF 7、D0–D7 對應 11、9、8、10、12、18、17、16，SCCB SDA/SCL 為 GPIO4/GPIO5。官方板的 GPIO0 是 BOOT 鍵，按住可進行語音提問，短按拍照。

### 胸前第 0 層硬體急煞擴充腳位

為實現 `< 30ms` 零 AI 算力的硬體保命急煞，胸前節點擴充了超音波與急煞告警：

| ESP32-S3 (DFR1154) 腳位 | 功能說明 | 連接模組腳位 | 安全與電壓規範 |
| :---: | :--- | :--- | :--- |
| **GPIO44 (Gravity TX)** | 超音波觸發 (TRIG) | JSN-SR04T / HC-SR04 TRIG | 3.3V 輸出脈衝 |
| **GPIO43 (Gravity RX)** | 超音波回波 (ECHO) | JSN-SR04T / HC-SR04 ECHO | **若感測器為 5V 模組，ECHO 需分壓至 3.3V** |
| **GPIO3** | 急煞警報輸出 / 板載警報 LED | 蜂鳴器 / 震動馬達 / 指示燈 | 驅動 MOSFET/電晶體（嚴禁直接帶大負載） |

- **第 0 層急煞運作機制**：ESP32-S3 核心每 25ms 取樣一次。當測量距離小於 **80cm** 時，硬體直接輸出高頻方波脈衝驅動 GPIO3 (板載指示燈/警報端) 進行急煞告警，**完全不經 Wi-Fi 與手機，保證 30ms 內反射反應**。
- **`/status` 遙測格式**：同時將狀態透過 JSON 端點輸出：`{"triggerCounter":1,"ip":"192.168.4.1","ultrasonicCm":68.5,"emergencyBrake":true}`，供手機 App 進行視覺聯動。

正常模式下，將 `WIFI_SSID` 與 `WIFI_PASSWORD` 改為手機可連線的區域網路，開機後從序列埠監控器讀取相機 IP，例如 `http://192.168.1.123`。在 App 中輸入不含結尾斜線的位址，畫面會使用 `/` 串流，按下拍照會呼叫 `/capture`。

如果 Wi-Fi station 在 15 秒內連不上，韌體會建立 `BlindAssist-Camera` 無線基地台，密碼為 `blindassist`，預設 IP 通常是 `http://192.168.4.1`。手機先連上這個 AP，再把 App 的相機位址設為 `http://192.168.4.1`。斷網時第 0 層硬體急煞與第 1 層手機邊緣 YOLO 均可完全離線正常工作。

## 5. 手機 App 建置與串接

### App 建置指令

```bash
cd mobile_app
pnpm install
cp .env.example .env.local
# 編輯 .env.local，填入原型測試用 API Key
pnpm run dev
```

產生 Android 原生專案的流程如下：

```bash
cd mobile_app
pnpm run build
pnpm exec cap add android
pnpm run cap:sync
pnpm exec cap open android
```

手機 App 不應在 UI 端永久保存影像；本原型收到 Gemini 文字後也不會將照片寫入檔案。

## 6. Gemini 與隱私注意事項

`geminiService.ts` 是原型版本，直接由瀏覽器／App 呼叫 Gemini REST API，因此 API Key 會進入前端建置環境。這只適合在專題開發與短期展示使用，不適合公開發佈。正式版本應增加自己的後端代理，例如由 Node.js 伺服器保存 API Key，手機只上傳一次性 JPEG 並取得文字結果；後端也應設定請求大小限制、速率限制與日誌去識別化。

影像處理上，建議將原始照片僅保留於記憶體，API 回應後立即釋放 Blob 引用，不寫入相簿、SD 卡或雲端硬碟。

## 7. 驗收測試表

| 測試項目 | 通過條件 | 測試方式 |
| :--- | :--- | :--- |
| 胸前超音波急煞 | 距離 < 80cm 立即觸發急煞蜂鳴，無卡頓 | 以 50、100 cm 障礙物進逼測試，記錄 30 筆 |
| 相機串流 | `/` 能穩定顯示 MJPEG 串流，`/capture` 回傳 JPEG | 手機瀏覽器與 App 各測一次 |
| 手機邊緣 YOLO | 偵測速度 15~30 FPS，中央安全錐精準分類方位 | 手機前向移動實測不同角度障礙物 |
| 斷網快通道 | 關閉手機網路時，第 0 層急煞與第 1 層 YOLO 依然能自主運作 | 關閉 Wi-Fi 與行動網路後驗證 |
| Gemini 慢通道 | 按鍵或語音提問觸發拍照，中文解說流暢 | 使用 10 張招牌與路況照片測試延遲與回答品質 |
