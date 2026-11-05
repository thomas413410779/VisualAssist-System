"""
VisualAssist System — YOLO 本地測試腳本
=========================================
用法：
  1. 把任意圖片丟進 images/ 資料夾
  2. 執行：python run_test.py
  3. 結果儲存到 output/ 資料夾（含標注框、類別、信心度）

額外選項（命令列）：
  python run_test.py --model yolov8n          # 指定模型 (yolov8n/yolov8s/yolo11n...)
  python run_test.py --conf 0.3               # 信心閾值（預設 0.25）
  python run_test.py --img path/to/img.jpg    # 只跑單張
  python run_test.py --show                   # 同時彈視窗預覽（需桌面環境）
"""

import argparse
import sys
import time
from pathlib import Path

# ── 顏色 (與 edgeDetector.ts 威脅等級對應) ──────────────────────────────────
THREAT_COLORS = {
    "critical": (0,   0,   255),   # 紅 — 極度緊急
    "warning":  (0,   165, 255),   # 橘 — 注意警告
    "safe":     (0,   200, 80),    # 綠 — 安全
    "ignored":  (180, 180, 180),   # 灰 — 過濾類別
}

# ── 與 edgeDetector.ts 完全一致的類別分級 ───────────────────────────────────
IMMEDIATE_THREATS = {"person", "car", "truck", "bus", "motorcycle", "bicycle"}
STATIC_HAZARDS    = {"traffic light", "stop sign", "chair", "bench",
                     "potted plant", "dog"}
IGNORED_CLASSES   = {"cat", "bird", "bottle", "cup", "fork",
                     "book", "cell phone", "mouse", "keyboard",
                     "scissors", "vase", "clock", "teddy bear"}

# ── 空間安全錐邊界（正規化 X 座標） ─────────────────────────────────────────
CONE_LEFT  = 0.35
CONE_RIGHT = 0.65

IMG_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".webp", ".tiff"}


def get_threat_level(label: str, cx_norm: float, h_norm: float) -> tuple[str, float]:
    """
    複製 edgeDetector.ts 的威脅判定邏輯，回傳 (威脅等級, 估算距離m)
    距離公式：clamp(0.5, 8.0, 1.2 / max(0.08, h_norm))
    """
    dist_m = max(0.5, min(8.0, 1.2 / max(0.08, h_norm)))
    in_cone = CONE_LEFT <= cx_norm <= CONE_RIGHT

    if label in IGNORED_CLASSES:
        return "ignored", dist_m

    if label in IMMEDIATE_THREATS:
        if in_cone and dist_m <= 1.8:
            return "critical", dist_m
        if in_cone and dist_m <= 3.5:
            return "warning", dist_m
        if not in_cone and dist_m <= 1.2:
            return "warning", dist_m
        return "safe", dist_m

    if label in STATIC_HAZARDS:
        if dist_m <= 2.0 and (in_cone or abs(cx_norm - 0.5) < 0.35):
            return "warning", dist_m
        return "safe", dist_m

    return "safe", dist_m


def draw_results(img, boxes, model_names, show_zone: bool = True):
    """在圖上畫框、標籤、安全錐分割線"""
    import cv2
    import numpy as np

    h_img, w_img = img.shape[:2]
    result_img = img.copy()

    # 畫安全錐分割線
    if show_zone:
        lx = int(CONE_LEFT  * w_img)
        rx = int(CONE_RIGHT * w_img)
        cv2.line(result_img, (lx, 0), (lx, h_img), (255, 255, 0), 1)
        cv2.line(result_img, (rx, 0), (rx, h_img), (255, 255, 0), 1)
        cv2.putText(result_img, "SAFE CONE", (lx + 4, 18),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (255, 255, 0), 1)

    for box in boxes:
        x1, y1, x2, y2 = map(int, box.xyxy[0])
        conf  = float(box.conf[0])
        cls_id = int(box.cls[0])
        label = model_names[cls_id]

        cx_norm = ((x1 + x2) / 2) / w_img
        h_norm  = (y2 - y1) / h_img

        threat, dist_m = get_threat_level(label, cx_norm, h_norm)
        color = THREAT_COLORS[threat]

        # 邊界框
        cv2.rectangle(result_img, (x1, y1), (x2, y2), color, 2)

        # 標籤背景
        tag = f"{label} {conf:.2f} | {threat} {dist_m:.1f}m"
        (tw, th), _ = cv2.getTextSize(tag, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)
        yt = max(y1 - 4, th + 2)
        cv2.rectangle(result_img,
                      (x1, yt - th - 2), (x1 + tw + 4, yt + 2),
                      color, -1)
        cv2.putText(result_img, tag,
                    (x1 + 2, yt),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                    (255, 255, 255), 1, cv2.LINE_AA)

    return result_img


def run(args):
    try:
        from ultralytics import YOLO
        import cv2
    except ImportError as e:
        print(f"[ERROR] 缺少套件：{e}")
        print("請先執行：python -m pip install ultralytics opencv-python")
        sys.exit(1)

    model = YOLO(f"{args.model}.pt")
    print(f"[INFO] 模型載入完成：{args.model}  (classes={len(model.names)})")

    # 收集圖片
    if args.img:
        images = [Path(args.img)]
    else:
        images = [p for p in Path("images").iterdir()
                  if p.suffix.lower() in IMG_EXTS]

    if not images:
        print("[WARN] images/ 資料夾是空的，請先放入圖片！")
        return

    out_dir = Path("output")
    out_dir.mkdir(exist_ok=True)
    total_time = 0.0

    for img_path in sorted(images):
        img = cv2.imread(str(img_path))
        if img is None:
            print(f"[SKIP] 無法讀取：{img_path}")
            continue

        t0 = time.perf_counter()
        results = model(img, conf=args.conf, verbose=False)
        elapsed = (time.perf_counter() - t0) * 1000
        total_time += elapsed

        boxes = results[0].boxes
        annotated = draw_results(img, boxes, model.names)

        out_path = out_dir / img_path.name
        cv2.imwrite(str(out_path), annotated)

        n_det = len(boxes) if boxes is not None else 0
        print(f"  [{img_path.name}]  偵測到 {n_det} 個物件  |  推論 {elapsed:.1f} ms  →  {out_path}")

        if args.show:
            cv2.imshow(img_path.name, annotated)
            cv2.waitKey(0)
            cv2.destroyAllWindows()

    avg = total_time / len(images) if images else 0
    print(f"\n[完成] 共處理 {len(images)} 張  |  平均推論 {avg:.1f} ms  ({1000/avg:.1f} FPS 理論值)")
    print(f"[輸出] 結果已存至 {out_dir.resolve()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="VisualAssist YOLO 本地測試")
    parser.add_argument("--model", default="yolov8n",
                        help="模型名稱，不含 .pt，例如 yolov8n / yolov8s / yolo11n")
    parser.add_argument("--conf",  default=0.25, type=float,
                        help="信心閾值 (0~1，預設 0.25)")
    parser.add_argument("--img",   default=None,
                        help="只測試單張圖片路徑")
    parser.add_argument("--show",  action="store_true",
                        help="測試完後彈視窗預覽")
    args = parser.parse_args()
    run(args)

