# ==============================================================================
# 穿戴式視覺輔助系統 (VisualAssist System) - ESP32 韌體一鍵編譯與燒錄腳本
# ==============================================================================

try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch {}
$OutputEncoding = [System.Text.Encoding]::UTF8

if ($PSScriptRoot) {
    $scriptDir = $PSScriptRoot
    $projectRoot = Split-Path -Parent $scriptDir
} elseif ($MyInvocation.MyCommand.Path) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    $projectRoot = Split-Path -Parent $scriptDir
} else {
    $projectRoot = (Get-Location).Path
    $scriptDir = Join-Path $projectRoot "scripts"
}

Write-Host ""
Write-Host "==============================================================================" -ForegroundColor Cyan
Write-Host "               ESP32-S3 胸前相機韌體一鍵燒錄工具                            " -ForegroundColor White
Write-Host "       目標 Wi-Fi：名稱 [ 00000000 ] / 密碼 [ ****** ]                       " -ForegroundColor Yellow
Write-Host "==============================================================================" -ForegroundColor Cyan
Write-Host ""

# 1. 尋找 PlatformIO 執行檔路徑
$pioExe = $null

if (Get-Command pio -ErrorAction SilentlyContinue) {
    $pioExe = "pio"
} else {
    $candidates = @(
        (Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"),
        (Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Python\Python*\Scripts\pio.exe")
    )
    foreach ($c in $candidates) {
        $found = Get-Item $c -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found -and (Test-Path $found.FullName)) {
            $pioExe = $found.FullName
            break
        }
    }
}

if (-not $pioExe) {
    Write-Host "[X] 未在系統或 VS Code 預設路徑中偵測到 PlatformIO 工具！" -ForegroundColor Red
    Write-Host ""
    Write-Host "【建議直接於 VS Code 中一鍵燒錄】：" -ForegroundColor Cyan
    Write-Host "  1. 查看 VS Code 視窗最左側工具列，點選「PlatformIO 外星人圖示」"
    Write-Host "  2. 點擊「Project Tasks」->「chest_vision」->「General」->「Upload」"
    Write-Host "  3. 等待終端機顯示 [SUCCESS]，即代表燒錄完成！"
    Write-Host ""
    Write-Host "【或使用 Arduino IDE】：" -ForegroundColor Cyan
    Write-Host "  1. 開啟 chest_vision/chest_vision.ino"
    Write-Host "  2. 開發板選擇「ESP32S3 Dev Module」，啟用 PSRAM (OPI)"
    Write-Host "  3. 點擊左上角的「上傳 (Upload)」按鈕"
    Write-Host ""
    Read-Host "請按 Enter 鍵返回..."
    exit 1
}

Write-Host "[OK] 成功找到 PlatformIO 編譯工具：" -ForegroundColor Green
Write-Host "     $pioExe" -ForegroundColor Gray
Write-Host ""

# 2. 檢查序列埠連線
$ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
if ($ports) {
    Write-Host "[OK] 偵測到已接線之 USB 序列埠：" -ForegroundColor Green
    foreach ($p in $ports) {
        Write-Host "     -> $($p.DeviceID): $($p.Name)" -ForegroundColor Green
    }
} else {
    Write-Host "[!] 提示：尚未偵測到序列埠，請確認 ESP32-S3 已用 USB 線牢固接上電腦。" -ForegroundColor Yellow
}
Write-Host ""

# 3. 執行編譯與燒錄
Write-Host "[INFO] 正在編譯韌體並上傳至 ESP32-S3，這可能需要 30~60 秒，請勿拔除 USB 線..." -ForegroundColor Cyan
Write-Host "------------------------------------------------------------------------------" -ForegroundColor DarkGray

Push-Location $projectRoot
try {
    & $pioExe run -t upload
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
}

Write-Host "------------------------------------------------------------------------------" -ForegroundColor DarkGray

if ($exitCode -eq 0) {
    Write-Host ""
    Write-Host "==============================================================================" -ForegroundColor Green
    Write-Host "               [ 恭喜！ESP32-S3 韌體已成功燒錄完成！]                        " -ForegroundColor White
    Write-Host "==============================================================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "  * ESP32 將立即重新啟動並自動連線至 Wi-Fi: 00000000" -ForegroundColor Yellow
    Write-Host "  * 接下來請：" -ForegroundColor Cyan
    Write-Host "    1. 從手機熱點管理畫面確認 ESP32 連上後獲得的 IP 位址"
    Write-Host "    2. 在前端網頁中的輸入框貼上該 IP，按下「儲存並連線」即可立即開啟鏡頭！"
    Write-Host ""
} else {
    Write-Host ""
    Write-Host "[X] 燒錄失敗 (結束代碼: $exitCode)" -ForegroundColor Red
    Write-Host "常見原因排查：" -ForegroundColor Yellow
    Write-Host "  1. COM 埠被其他程式佔用：請關閉序列埠監控視窗後重試"
    Write-Host "  2. USB 傳輸線不良：請更換一條可傳輸資料的 USB Type-C 傳輸線"
    Write-Host "  3. 板子未進入下載模式：長按 ESP32 上的 BOOT 鍵不放，點一下 RESET 鍵後放開 BOOT 鍵再試"
    Write-Host ""
}

Read-Host "請按 Enter 鍵結束..."
exit $exitCode
