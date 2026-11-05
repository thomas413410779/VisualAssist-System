@echo off
echo [1/2] 安裝 ultralytics + opencv-python ...
python -m pip install ultralytics opencv-python onnx onnxslim
echo.
echo [2/2] 完成！可以開始執行 run_test.py 了。
echo    把圖片丟進 images\ 資料夾，然後執行：
echo    python run_test.py
pause

