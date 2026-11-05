# YOLO 本地測試工具

> **用途**：在本機快速驗證 YOLOv8/YOLO11 的畫框效果，並以和 `edgeDetector.ts` 完全一致的威脅等級邏輯標注每個偵測框。

---

## 📁 資料夾結構

```
yolo_test/
├── images/          ← 把測試圖片丟這裡（jpg / png / bmp / webp）
├── output/          ← 標注結果自動存到這裡
├── run_test.py      ← 主測試腳本
├── install_deps.bat ← 一鍵安裝依賴
├── export_onnx.bat  ← 匯出 ONNX 並複製到 mobile_app
└── README.md
```

---

## 🚀 快速開始

### 步驟一：安裝依賴（只需一次）

```bat
install_deps.bat
```

或手動：

```bash
python -m pip install ultralytics opencv-python onnx onnxslim
```

### 步驟二：放入圖片

把任意 `.jpg` / `.png` 圖片拖進 `images/` 資料夾。

### 步驟三：執行測試

```bash
python run_test.py
```

結果圖片輸出到 `output/`，終端機顯示每張的推論時間。

---

## ⚙️ 進階選項

| 指令 | 說明 |
|------|------|
| `python run_test.py --model yolov8s` | 換成較精準的 small 版本 |
| `python run_test.py --model yolo11n` | 測試最新 YOLO11 nano |
| `python run_test.py --conf 0.35` | 提高信心閾值（減少假陽性）|
| `python run_test.py --img path/to/x.jpg` | 只測單張 |
| `python run_test.py --show` | 測完彈視窗預覽 |

---

## 🎨 標注框顏色說明

| 顏色 | 威脅等級 | 觸發條件 |
|------|----------|----------|
| 🔴 紅 | `critical` 極度緊急 | 中央安全錐內且距離 ≤ 1.8m |
| 🟠 橘 | `warning` 注意警告 | 中央安全錐內 ≤ 3.5m，或側邊 ≤ 1.2m |
| 🟢 綠 | `safe` 安全 | 在錐外或距離夠遠 |
| ⚪ 灰 | `ignored` 過濾 | cat / bird / bottle 等室內雜物 |

> 黃色虛線為 **空間安全錐**（水平 35%～65%），對應手機畫面中央的核心行走區域。

---

## 📦 匯出 ONNX 供 App 使用

測試滿意後，執行：

```bat
export_onnx.bat
```

會自動將 `yolov8n.onnx` 複製到 `mobile_app/public/models/`，讓 App 直接使用。

---

## 📐 威脅等級邏輯（與 edgeDetector.ts 完全一致）

距離估算公式：

$$\text{Distance (m)} = \text{clamp}\left(0.5,\, 8.0,\, \frac{1.2}{\max(0.08,\, H_{\text{norm}})}\right)$$

| 類別群 | 成員 |
|--------|------|
| 重大動態威脅 | person, car, truck, bus, motorcycle, bicycle |
| 環境靜態障礙 | traffic light, stop sign, chair, bench, potted plant, dog |
| 過濾忽略 | cat, bird, bottle, cup, fork, book, cell phone… |

