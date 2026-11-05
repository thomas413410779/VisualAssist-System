@echo off
echo [匯出 YOLOv8n -> ONNX] 供手機 App 使用
echo.
python -c "from ultralytics import YOLO; m=YOLO('yolov8n.pt'); m.export(format='onnx', imgsz=640, dynamic=False, simplify=True)"
echo.
echo [複製到 mobile_app/public/models/]
if exist yolov8n.onnx (
    copy /Y yolov8n.onnx ..\mobile_app\public\models\yolov8n.onnx
    echo 複製完成！
) else (
    echo [ERROR] 找不到 yolov8n.onnx，匯出可能失敗了。
)
pause

