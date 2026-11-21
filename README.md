<div align="center">

# 穿戴式視覺輔助系統 (DEWANS)

**結合胸前反射急煞、手機邊緣動態視覺與雲端多模態認知之三層協同輔助架構**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](./INTEGRATION_GUIDE.md)
[![Platform: ESP32--S3](https://img.shields.io/badge/Platform-ESP32--S3-red.svg)](https://www.espressif.com/)
[![Framework: React 19 + PWA](https://img.shields.io/badge/Mobile-React_19_+_PWA-61dafb.svg)](https://react.dev/)
[![Inference: ONNX Runtime](https://img.shields.io/badge/Inference-ONNX_Runtime_Web-blueviolet.svg)](https://onnxruntime.ai/)

</div>

---

## 專案定位

本專案（Dual-path Edge Wearable Assistive Navigation System，簡稱 DEWANS）為針對視障自主導引設計之穿戴式研究原型。系統以「胸前感測單元」搭配「隨身智慧型手機」進行雙端協同運算，擺脫傳統導盲手杖之感測視角限制。

為解決傳統雲端視覺模型（VLM）數秒推論延遲、行進動態模糊、連續通報造成聽覺疲勞，以及單一手杖擺角失真等問題，本系統提出「三層分級混合防護架構」，將即時保命反射完全留在邊緣端，並將深度語義理解作為按需觸發的大腦。

> 注意：本專案為研究驗證原型與開源工程骨架，非醫療級自主導盲設備。戶外實測時必須由專人陪同，並搭配傳統白手杖使用。

---

## 三層分級防護架構

```text
[環境動態] 
   |
   +---> [第 0 層: 胸前硬體反射] < 30ms 延遲
   |        ESP32-S3 + JSN-SR04T 超音波 (< 80cm 觸發硬體蜂鳴/震動急煞)
   |
   +---> [第 1 層: 手機邊緣動態感知] 30~50ms 延遲 (15~30 FPS)
   |        MJPEG 串流 -> ONNX Runtime Web (YOLOv8n) -> 中央安全錐過濾 -> 離線語音警報
   |
   +---> [第 2 層: 雲端認知大腦] 1~3 秒延遲 (按需手動觸發)
            單張 JPEG 擷取 -> Gemini 多模態 API -> 招牌文字、環境深度解讀
```

1. **第 0 層 · 胸前硬體反射神經（延遲 < 30ms，零 AI 算力）**
   胸前超音波感測器以 25ms 週期取樣前方距離。一旦進入 80cm 近迫危險區，微控制器（ESP32-S3）直接透過硬體電路驅動蜂鳴器或震動馬達，提供零軟體排隊延遲的即時急煞警示。
2. **第 1 層 · 手機邊緣動態感知（15~30 FPS，延遲 30~50ms）**
   手機接收相機即時串流，利用 ONNX Runtime Web 於本地執行輕量化 YOLOv8n 物件偵測。配合「中央安全錐 (Center Safe Cone)」演算法，僅針對前進動線上具備實質碰撞風險之目標發出空間方位語音，避免無效資訊轟炸。
3. **第 2 層 · 雲端多模態認知大腦（1~3 秒，按需主動觸發）**
   嚴格獨立於即時避障路徑之外。僅在使用者主動按鈕發問時，擷取單張靜態相片送交 Gemini API 進行店家招牌解析、整體環境描述或尋物判讀。

---

## 技術方案對比與設計權衡

| 評估維度 | 傳統純雲端 VLM 方案 | 傳統手杖單點超音波 | 本專案三層協同架構 (DEWANS) |
|---|---|---|---|
| 即時避障延遲 | 1,000 ~ 3,000 ms (存在碰撞空窗) | < 50 ms | **< 30 ms (硬體反射) + 40 ms (邊緣 YOLO)** |
| 網路斷線生存性 | 完全失效 | 完全離線 | **即時避障 100% 離線可用，僅深度問答需連網** |
| 移動動態穩定度 | 車載視角易模糊、頻寬耗損大 | 手杖左右揮動導致測距基準漂移 | **胸前水平穩定視角，雙眼級動態空間追蹤** |
| 聽覺認知負荷 | 連續廣播畫面所有物體，極易疲勞 | 單純逼逼聲，無法判斷障礙物類別 | **中央安全錐過濾 + 3 階威脅冷卻語音播報** |
| 功耗與發熱 | 手機全時上傳影像，耗電發燙 | 極低 | **分級喚醒，常態運算維持中低負載** |

---

## 模組權責與檔案架構

```text
VisualAssist_System/
├── chest_vision/              # 胸前穿戴韌體 (ESP32-S3)
│   └── chest_vision.ino       # 相機驅動、MJPEG 串流、超音波取樣、急煞中斷
├── mobile_app/                # 手機端邊緣運算 App (React 19 + PWA 離線應用)
│   ├── src/
│   │   ├── services/
│   │   │   ├── cameraService.ts   # 相機串流、遙測狀態與控制協定
│   │   │   ├── edgeDetector.ts    # YOLO 邊緣推論、安全錐空間分區演算法
│   │   │   ├── geminiService.ts   # 雲端多模態認知 API 整合
│   │   │   └── ttsService.ts      # 離線語音合成與警示音調控制
│   │   └── App.tsx                # 主介面狀態管理與音訊事件迴圈
├── scripts/                   # 自動化啟動與輔助工具
│   ├── launch.ps1             # 啟動主流程腳本
│   └── flash_esp32.ps1        # 韌體燒錄輔助腳本
├── yolo_test/                 # 模型驗證與本機測試套裝
│   ├── run_test.py            # 推論延遲與邊界測試
│   └── export_onnx.bat        # ONNX 模型量化與匯出工具
├── platformio.ini             # PlatformIO 硬體編譯組態
├── start.bat                  # 一鍵自動化啟動進入點
└── INTEGRATION_GUIDE.md       # 硬體清單、電路接線與進階設定手冊
```

---

## 硬體規格與接線安全規範

下表為核心硬體與對應接線限制，詳細採購型號請見 [INTEGRATION_GUIDE.md](./INTEGRATION_GUIDE.md)：

| 硬體組件 | 建議型號與規格 | 介面 / GPIO | 安全防護要求 |
|---|---|---|---|
| 胸前主控板 | ESP32-S3-EYE (8MB Flash + 8MB OPI PSRAM) | 板載相機 + USB CDC | 確保供電電流達 5V / 1.5A 以上 |
| 前方急煞測距 | JSN-SR04T 防水超音波模組 (或 HC-SR04) | TRIG: GPIO44 / ECHO: GPIO43 | **Gravity 接口 (TX:44, RX:43)；若為 5V ECHO 請分壓至 3.3V** |
| 急煞回饋輸出 | 3V 有源蜂鳴器 / 震動馬達 / 板載 LED | GPIO3 (急煞信號) | **嚴禁直接以 GPIO 供電高負載，應透過電晶體隔離驅動** |
| 供電電源 | 5V 輕量行動電源 | Type-C USB 埠 | 獨立電路供電，避免大電流負載干擾相機運作 |

---

## 快速開始

### 方式 A：全新 Windows 電腦一鍵啟動（推薦）

專案內建全自動環境檢測腳本。在已連接 ESP32-S3 開發板之電腦上，直接雙擊執行：
- `start.bat` 或 `一鍵啟動_視覺輔助系統.bat`

執行時自動處理以下流程：
1. 自動偵測 Node.js 環境（若無則免安裝下載可攜版 Node.js LTS）。
2. 自動校驗並補齊依賴套件。
3. 自動掃描已連接之 ESP32 序列埠並注入連線參數。
4. 啟動 Vite 本機開發伺服器，自動喚起瀏覽器並顯示區域網路測試位址。

### 方式 B：手動建置與啟動

#### 1. 燒錄胸前穿戴端 (ESP32-S3)

可使用 PlatformIO 或 Arduino IDE 進行編譯與上傳：

```bash
# 使用 PlatformIO 編譯並燒錄韌體
pio run -t upload
```

啟動後請至序列埠確認取得之 IP 位址（預設 mDNS 主機名稱為 `http://visualassist.local`；若未配置 Wi-Fi 則自動建立 `BlindAssist-Camera` AP）。

#### 2. 啟動手機 App 端

```bash
# 1. 進入手機 App 目錄
cd mobile_app

# 2. 安裝依賴
pnpm install

# 3. 複製環境設定並填入 Gemini API 金鑰
cp .env.example .env.local

# 4. 啟動本機開發伺服器
pnpm run dev
```

如需建置 Android 原生 APK：

```bash
pnpm run build
pnpm exec cap add android
pnpm run cap:sync
pnpm exec cap open android
```

---

## 實測與驗證流程

建議遵循以下四階段階梯式驗證，確保系統各層級獨立穩定：

1. **超音波反射測試**：於序列埠監控器驗證 80cm 內即時觸發 GPIO44 急煞輸出，確認延遲小於 30ms。
2. **影像串流端點測試**：使用瀏覽器確認 `http://visualassist.local/` (MJPEG) 與 `/capture` (JPEG) 存取無誤。
3. **邊緣空間追蹤測試**：於 App 畫面確認 YOLOv8n 能辨識前方障礙，且僅在中央安全錐判定威脅時觸發 TTS 方位警報。
4. **雲端認知整合測試**：按下相機按鍵或 App 畫面觸發 Gemini 多模態對話，驗證中文語音說明之準確度。

---

## 相關文件樞紐

- 系統整合與硬體接線手冊：[INTEGRATION_GUIDE.md](./INTEGRATION_GUIDE.md)
- YOLO 模型選型與邊緣推論評估：[YOLO_SELECTION_AND_EDGE_INFERENCE.md](./YOLO_SELECTION_AND_EDGE_INFERENCE.md)
- 採購規格與命名稱謂整理：[PROCUREMENT_LINKS_AND_PROJECT_NAMING.md](./PROCUREMENT_LINKS_AND_PROJECT_NAMING.md)
- 本機 YOLO 測試與 ONNX 匯出工具：[yolo_test/README.md](./yolo_test/README.md)

---

## 授權條款

本專案採 [MIT 授權條款](./INTEGRATION_GUIDE.md)。各項第三方依賴與開源參考項目之聲明請詳閱 `INTEGRATION_GUIDE.md`。
