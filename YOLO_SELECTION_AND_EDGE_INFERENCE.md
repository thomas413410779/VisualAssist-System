# 第 1 層手機端邊緣動態感知：YOLO 模型選型與推論架構評估

本文件詳細闡述 **VisualAssist System（視障智慧輔助系統）** 在「第 1 層 · 手機邊緣動態感知」中，物件偵測演算法（YOLO）的世代選型依據、尺寸權衡分析、視障場景優先級映射，以及基於 ONNX Runtime Web 的離線推論架構。

---

## 1. 系統定位與核心設計原則

在整體三層式防護架構中，第 1 層扮演的是**「連續動態避障導引」**的中樞神經：

* **第 0 層 (ESP32-S3 胸前穿戴裝置)**：毫秒級極限急煞（硬體超音波測距 < 80cm 立即觸發震動/蜂鳴器，無延遲硬體保護）。
* **第 1 層 (手機本地 YOLO 邊緣推論)**：**15～30 FPS、30～50ms 延遲**。負責即時辨識行人、車輛、障礙物，劃分空間安全錐，並主動以語音警告近距威脅。
* **第 2 層 (雲端 Gemini 認知)**：高解析度單張拍照，深入理解環境細節（招牌文字、紅綠燈倒數、菜單）。

### 邊緣端推論的 3 大不可妥協原則：
1. **完全離線自主 (Zero Internet Dependency)**：街道突發路況（如疾駛單車、逆向機車）容不得網路斷線或蜂窩網路延遲（4G/5G RTT 常在 100~300ms 以上）。
2. **低延遲與高更新率 (15~30 FPS)**：行人快步每秒約前進 1.2 公尺，推論延遲必須壓在 **50ms 以內**，語音警報才能在撞擊前 2~3 秒及時送達。
3. **低功耗與防發熱 (Thermal & Battery Budget)**：手機需隨身連續工作 4~8 小時，模型運算必須受限於中階手機晶片（NPU/GPU/CPU）能常駐運行的功耗區間內。

---

## 2. YOLO 各世代橫向技術評估

針對目前業界主流的 YOLO 系列演算法，進行適用於手機 Web/PWA（React 19 + PWA + ONNX Runtime Web）環境之評估：

| 評估項目 | YOLOv5 (Ultralytics) | YOLOv7 (Chien-Yao Wang) | **YOLOv8 (Ultralytics)** | **YOLO11 (Ultralytics 最新)** |
| :--- | :--- | :--- | :--- | :--- |
| **發布年份** | 2020 | 2022 | **2023** | **2024** |
| **架構特性** | Anchor-based, 經典 CSPNet | 擴展高效層 (E-ELAN) | **Anchor-free, C2f 模組** | **C3k2, 增強注意力與空間特徵** |
| **小目標偵測能力** | 普通 | 良好 | **優異**（對遠處單車/柱子敏感） | **極佳** |
| **邊緣硬體適配度** | 良好 | 一般（模型結構深，算子相容度普通） | **極佳（全球生態鏈支援最完善）** | **極佳** |
| **ONNX Web/WASM 相容性** | 完美相容 | 需自訂算子或複雜後處理 | **完美相容（輸出張量直觀）** | **相容（算子結構接近 v8）** |
| **生態文檔與匯出友善度** | 非常成熟 | 文檔零散 | **最豐富、一鍵轉 ONNX/TFLite** | 最新、支援度快速成長中 |
| **結論** | 備選（精度略遜） | 不推薦（不適合移動端） | 🌟 **生產環境基準首選** | 🚀 **新一代升級候選** |

---

## 3. 模型尺寸（Scale）權衡比較

在相同架構下，不同參數量級對手機邊緣推論的衝擊如下表：

| 模型規模 | 參數量 (Params) | 權重體積 (FP32 / FP16) | 手機 GPU/WASM 延遲 | 記憶體佔用 | 適合場景 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Nano (`n`)** 🏆 | **約 3.2M** | **約 6.3 MB (FP16: 3.2MB)** | **25 ～ 45 ms (22~30 FPS)** | **< 150 MB** | **視障隨身手機 App 首選（低功耗、不發燙）** |
| **Small (`s`)** | 約 11.2M | 約 22.5 MB | 70 ～ 120 ms (8~14 FPS) | 約 350 MB | 旗艦機專用，稍有發熱風險 |
| **Medium (`m`)** | 約 25.9M | 約 52 MB | 180 ～ 300 ms (< 5 FPS) | > 600 MB | 不適合手機即時避障（反應太慢） |
| **Large / XLarge** | > 43M | > 85 MB | > 500 ms | > 1.2 GB | 僅伺服器/桌面端適用 |

### 為什麼強烈選擇 `Nano (n)` 版本？
1. **吞吐量決定安全**：視障者需要連續感知，若幀率低於 10 FPS（延遲 > 100ms），單車突然拐彎時系統將無法追蹤軌跡。Nano 版本可在絕大多數 Android 手機上維持 **20 FPS 以上**。
2. **體積小巧**：整包模型僅 6MB，可直接封裝至 APK 原生 Assets 內，使用者安裝 App 後**開箱即用、完全不需初次下載**。
3. **雙核心平衡**：手機在背景運行 YOLO 的同時，還需處理 UI 渲染、音訊 TTS 合成與胸前遙測數據，Nano 版本能留下充裕的 CPU/GPU 資源。

---

## 4. 視障場景之 COCO 類別優先級劃分

標準 YOLO 模型基於 MS COCO 數據集訓練（共 80 類別）。為了避免無意義的語音資訊干擾使用者，系統在 [edgeDetector.ts](file:///c:/VisualAssist_System/mobile_app/src/services/edgeDetector.ts) 中將類別精準劃分為 3 個威脅階層：

### 🚨 第一級：重大碰撞動態威脅 (Immediate Threat)
* **類別**：`person` (行人)、`car` (車輛)、`truck` (大型卡車)、`bus` (公車)、`motorcycle` (機車)、`bicycle` (自行車)。
* **處理策略**：只要進入中央安全錐，且距離估算 $\le 3.5\text{ m}$，立即觸發方位警告（例如：「正前方 2 公尺有自行車」）。

### ⚠️ 第二級：環境與通行障礙 (Static Hazards)
* **類別**：`traffic light` (交通號誌)、`stop sign` (停等標誌)、`chair` / `bench` (長椅/雜物)、`potted plant` (盆栽)、`dog` (導盲犬或寵物犬)。
* **處理策略**：當距離 $\le 2.0\text{ m}$ 且在中央或近側時發出低急迫提示。

### ⚪ 第三級：過濾忽略類別 (Ignored / Filtered)
* **類別**：`cat`、`bird`、`bottle`、`cup`、`fork`、`book`、`cell phone` 等室內微小物件。
* **處理策略**：邊緣推論直接濾除，防止引發使用者的「語音警報疲勞 (Alert Fatigue)」。

---

## 5. 空間安全錐（Spatial Safe Cone）與威脅評估演算法

[edgeDetector.ts](file:///c:/VisualAssist_System/mobile_app/src/services/edgeDetector.ts) 實作了單目視覺空間幾何評估：

```text
 畫面水平視野劃分 (Normalized X Coordinates):
 [ 0.0 ~ 0.35 ]          [ 0.35 ~ 0.65 ]          [ 0.65 ~ 1.0 ]
   左側 (Left)        中央安全錐 (Center)           右側 (Right)
 (側向預警區域)      (核心行走碰撞危險區)         (側向預警區域)
```

1. **距離預估公式**：
   基於單目相機幾何原理，透過邊界框正規化高度 ($H_{\text{norm}}$) 與典型人體/車輛實體高度比例估算：
   $$\text{Distance (m)} = \text{clamp}\left(0.5,\, 8.0,\, \frac{1.2}{\max(0.08, H_{\text{norm}})}\right)$$
2. **威脅等級判定**：
   * **`critical`（極度緊急）**：中央安全錐且距離 $\le 1.8\text{ m}$（立即語音播報並連動震動）。
   * **`warning`（注意警告）**：中央安全錐且距離 $\le 3.5\text{ m}$，或兩側邊界距離 $\le 1.2\text{ m}$。
   * **`safe`（安全通過）**：目標在安全錐外或距離大於 3.5 公尺。
3. **3 秒防干擾冷卻機制 (Audio Cooldown)**：
   同類別在同方位之警報設有 3 秒防重複冷卻，避免同一個行人走來時語音連續跳針干擾視障者的聽覺空間注意力。

---

## 6. 前端推論架構與實作指引

### 技術棧整合：
* **推論核心**：`onnxruntime-web` (WebAssembly + WebGL/WebGPU 跨平台推論)
* **打包框架**：現代化 PWA (漸進式網頁應用，支援全螢幕安裝與離線快取)
* **模型放置路徑**：`mobile_app/public/models/yolov8n.onnx`

### 模型產生與導出步驟（開發者參考）：
本機若有安裝 Python，執行以下兩行即可取得最佳化後的網頁模型：
```bash
pip install ultralytics onnx onnxslim
yolo export model=yolov8n.pt format=onnx imgsz=640 dynamic=False simplify=True
```
將輸出的 `yolov8n.onnx` 複製至專案：
```bash
cp yolov8n.onnx c:\VisualAssist_System\mobile_app\public\models\yolov8n.onnx
```

### 優雅降級設計 (Graceful Fallback)：
在 `LocalYoloEdgeDetector` 中設計了自動容錯偵測：
* 若找不到實體 `yolov8n.onnx` 檔案，系統自動切換為**高模擬演算模式**，UI 正常渲染邊界框與方位，確保在沒有模型權重的情況下，App 開發與功能測試完全不中斷！
