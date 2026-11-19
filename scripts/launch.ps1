# ==============================================================================
# 穿戴式視覺輔助系統 (VisualAssist System / DEWANS)
# 全新電腦一鍵自動化環境配置、依賴套件安裝與前端啟動核心腳本
# ==============================================================================

# 設定輸出編碼為 UTF-8
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch {}
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
$mobileAppDir = Join-Path $projectRoot "mobile_app"
$toolsDir = Join-Path $projectRoot ".tools"
$port = 5173
$hostUrl = "http://localhost:$port"

function Write-StepHeader ($stepNum, $totalSteps, $title) {
    Write-Host ""
    Write-Host "[$stepNum/$totalSteps] $title" -ForegroundColor Cyan
    Write-Host ("-" * 60) -ForegroundColor DarkGray
}

function Write-Success ($msg) {
    Write-Host "  [OK] $msg" -ForegroundColor Green
}

function Write-Info ($msg) {
    Write-Host "  [i] $msg" -ForegroundColor Yellow
}

function Write-Notice ($msg) {
    Write-Host "  [!] $msg" -ForegroundColor Magenta
}

function Write-Fail ($msg) {
    Write-Host "  [X] $msg" -ForegroundColor Red
}

Write-Host "==============================================================================" -ForegroundColor Cyan
Write-Host "               穿戴式視覺輔助系統 (VisualAssist System)                      " -ForegroundColor White
Write-Host "       全新電腦一鍵自動環境配置、相依套件安裝與邊緣推論前端啟動器           " -ForegroundColor Gray
Write-Host "==============================================================================" -ForegroundColor Cyan

# ------------------------------------------------------------------------------
# 步驟 0：檢查並自動編譯生成原生 .exe 執行檔 (若尚未存在)
# ------------------------------------------------------------------------------
$exePath = Join-Path $projectRoot "一鍵啟動_視覺輔助系統.exe"
if (-not (Test-Path $exePath)) {
    $cscCandidates = @(
        "C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe",
        "C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe"
    )
    $csSource = Join-Path $scriptDir "Launcher.cs"
    foreach ($csc in $cscCandidates) {
        if ((Test-Path $csc) -and (Test-Path $csSource)) {
            try {
                & $csc /nologo /target:exe /out:"$exePath" "$csSource" 2>$null | Out-Null
                if (Test-Path $exePath) {
                    Write-Success "已為您生成原生執行檔：一鍵啟動_視覺輔助系統.exe"
                    break
                }
            } catch {}
        }
    }
}

# ------------------------------------------------------------------------------
# 步驟 1/5：檢查與配置 Node.js 執行環境
# ------------------------------------------------------------------------------
Write-StepHeader 1 5 "檢查與準備 Node.js 執行環境"

$nodeExe = $null
$npmCmd = "npm"

# 1. 優先檢查本機系統全域環境
if (Get-Command node -ErrorAction SilentlyContinue) {
    $nodeExe = "node"
    $npmCmd = "npm"
    try {
        $nodeVer = (& node -v).Trim()
        Write-Success "偵測到本機已安裝 Node.js ($nodeVer)"
    } catch {
        Write-Success "偵測到本機已安裝 Node.js"
    }
}
else {
    # 2. 檢查專案目錄下是否有之前下載的可攜式 Node.js
    $portableNodeCandidates = Get-ChildItem -Path $toolsDir -Filter "node.exe" -Recurse -ErrorAction SilentlyContinue
    if ($portableNodeCandidates -and $portableNodeCandidates.Count -gt 0) {
        $nodeBinDir = $portableNodeCandidates[0].DirectoryName
        $env:PATH = "$nodeBinDir;$env:PATH"
        $nodeExe = $portableNodeCandidates[0].FullName
        $npmCmd = Join-Path $nodeBinDir "npm.cmd"
        try {
            $nodeVer = (& $nodeExe -v).Trim()
            Write-Success "已載入專案內建可攜式 Node.js ($nodeVer)"
        } catch {
            Write-Success "已載入專案內建可攜式 Node.js"
        }
    }
    else {
        # 3. 全新電腦自動下載官方免安裝 Portable Node.js LTS
        Write-Notice "檢測到全新電腦未安裝 Node.js！"
        Write-Info "正在自動為您下載官方免安裝可攜版 Node.js LTS (免管理員權限，約 30 MB)..."

        if (-not (Test-Path $toolsDir)) {
            New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null
        }

        $nodeZipUrl = "https://nodejs.org/dist/v20.18.0/node-v20.18.0-win-x64.zip"
        $nodeZipFile = Join-Path $toolsDir "node-portable.zip"
        $extractDir = Join-Path $toolsDir "node-v20"

        try {
            Write-Info "下載中：$nodeZipUrl"
            if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
                & curl.exe -fSL --progress-bar -o $nodeZipFile $nodeZipUrl
            }
            else {
                Invoke-WebRequest -Uri $nodeZipUrl -OutFile $nodeZipFile -UseBasicParsing
            }

            Write-Info "解壓縮中..."
            if (Get-Command tar.exe -ErrorAction SilentlyContinue) {
                if (-not (Test-Path $extractDir)) { New-Item -ItemType Directory -Path $extractDir -Force | Out-Null }
                & tar.exe -xf $nodeZipFile -C $extractDir --strip-components=1
            }
            else {
                Expand-Archive -Path $nodeZipFile -DestinationPath $toolsDir -Force
                $extractedFolder = Get-ChildItem -Path $toolsDir -Directory -Filter "node-v20*" | Select-Object -First 1
                if ($extractedFolder) {
                    Rename-Item -Path $extractedFolder.FullName -NewName "node-v20" -Force
                }
            }

            if (Test-Path $nodeZipFile) {
                Remove-Item $nodeZipFile -Force -ErrorAction SilentlyContinue
            }

            $nodeBinDir = $extractDir
            $env:PATH = "$nodeBinDir;$env:PATH"
            $nodeExe = Join-Path $nodeBinDir "node.exe"
            $npmCmd = Join-Path $nodeBinDir "npm.cmd"

            $nodeVer = (& $nodeExe -v).Trim()
            Write-Success "可攜式 Node.js 安裝成功！版本：$nodeVer"
        }
        catch {
            Write-Fail "自動下載 Node.js 失敗：$($_.Exception.Message)"
            Write-Fail "請手動至 https://nodejs.org/ 下載安裝 Node.js 後重新執行此程式。"
            Read-Host "按 Enter 鍵結束..."
            exit 1
        }
    }
}

# ------------------------------------------------------------------------------
# 步驟 2/5：檢查前端專案環境與依賴套件 (npm install)
# ------------------------------------------------------------------------------
Write-StepHeader 2 5 "檢查前端專案設定與下載依賴套件"

if (-not (Test-Path $mobileAppDir)) {
    Write-Fail "找不到前端專案目錄：$mobileAppDir"
    Read-Host "按 Enter 鍵結束..."
    exit 1
}

# 檢查 .env
$envFile = Join-Path $mobileAppDir ".env"
$envExample = Join-Path $mobileAppDir ".env.example"
if (-not (Test-Path $envFile)) {
    if (Test-Path $envExample) {
        Copy-Item -Path $envExample -Destination $envFile
        Write-Success "已為您自動自 .env.example 建立 mobile_app/.env"
    }
    else {
        Write-Info "建立預設 mobile_app/.env"
        Set-Content -Path $envFile -Value "VITE_GEMINI_API_KEY=`nVITE_GEMINI_MODEL=gemini-flash-lite-latest`n" -Encoding UTF8
    }
} else {
    Write-Success "前端環境設定檔 (.env) 已就緒"
}

# 檢查 YOLO 模型
$modelFile = Join-Path $mobileAppDir "public\models\yolov8n.onnx"
if (Test-Path $modelFile) {
    $modelSizeMb = [math]::Round(((Get-Item $modelFile).Length / 1MB), 2)
    Write-Success "第 1 層邊緣推論模型已存在 (yolov8n.onnx, $modelSizeMb MB)"
} else {
    Write-Notice "尚未在 public\models\ 找到 yolov8n.onnx 模型檔案，如需離線推論請參考 models/README.md"
}

# 檢查 node_modules 是否需安裝套件
$nodeModulesDir = Join-Path $mobileAppDir "node_modules"
$viteCli = Join-Path $nodeModulesDir "vite"

if (-not (Test-Path $nodeModulesDir) -or -not (Test-Path $viteCli)) {
    Write-Notice "檢測到依賴套件尚未安裝或不完整，即刻自動為您執行 npm install..."
    Write-Info "正在下載所有前端相依套件 (React 19, Vite, ONNX Runtime Web, TensorFlow.js 等)..."
    
    Push-Location $mobileAppDir
    try {
        & cmd.exe /c "$npmCmd install"
        if ($LASTEXITCODE -ne 0) {
            Write-Fail "npm install 發生錯誤，結束代碼：$LASTEXITCODE"
            Pop-Location
            Read-Host "按 Enter 鍵結束..."
            exit 1
        }
        Write-Success "所有相依套件已全數下載並安裝完成！"
    }
    finally {
        Pop-Location
    }
} else {
    Write-Success "前端依賴套件 (node_modules) 已就緒"
}

# ------------------------------------------------------------------------------
# 步驟 3/5：硬體直接接線與 USB 序列埠 (COM) 偵測
# ------------------------------------------------------------------------------
Write-StepHeader 3 5 "硬體接線狀態與序列埠 (COM) 偵測"

$detectedPorts = @()
try {
    $wmiPorts = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
    if ($wmiPorts) {
        foreach ($p in $wmiPorts) {
            $detectedPorts += [PSCustomObject]@{
                Port = $p.DeviceID
                Name = $p.Name
                Description = $p.Description
            }
        }
    }
} catch {}

if ($detectedPorts.Count -eq 0) {
    try {
        $pnpPorts = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue
        if ($pnpPorts) {
            foreach ($p in $pnpPorts) {
                if ($p.FriendlyName -match '\((COM\d+)\)') {
                    $detectedPorts += [PSCustomObject]@{
                        Port = $Matches[1]
                        Name = $p.FriendlyName
                        Description = $p.FriendlyName
                    }
                }
            }
        }
    } catch {}
}

$detectedCameraIp = $null
if ($detectedPorts.Count -gt 0) {
    Write-Success "已偵測到電腦連接之序列埠硬體裝置："
    foreach ($p in $detectedPorts) {
        Write-Host "    -> 連接埠: $($p.Port) | 裝置描述: $($p.Name)" -ForegroundColor Green
    }
    Write-Info "ESP32-S3 開發板已透過 USB 供電/通訊中"

    # 自動發送重設訊號並讀取 ESP32 鏡頭 IP 位址
    $targetPort = $detectedPorts[0].Port
    Write-Info "正在自動與 ESP32 ($targetPort) 連線並讀取 Wi-Fi/熱點 IP 位址 (約 6 秒)..."
    try {
        $sp = New-Object System.IO.Ports.SerialPort $targetPort, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One
        $sp.ReadTimeout = 600
        $sp.Open()

        # 發送 DTR/RTS 重設訊號觸發 ESP32 重新連線與輸出開機網路日誌
        $sp.DtrEnable = $false
        $sp.RtsEnable = $true
        Start-Sleep -Milliseconds 200
        $sp.RtsEnable = $false
        Start-Sleep -Milliseconds 100

        $ipDeadline = (Get-Date).AddSeconds(7)
        while ((Get-Date) -lt $ipDeadline) {
            try {
                $line = $sp.ReadLine()
                if ($line) {
                    if ($line -match 'http://(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})') {
                        $detectedCameraIp = "http://$($Matches[1])"
                        break
                    }
                    if ($line -match 'IP\s*:\s*(http://\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}|\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})') {
                        $matchedIp = $Matches[1]
                        if (-not $matchedIp.StartsWith('http')) { $matchedIp = "http://$matchedIp" }
                        $detectedCameraIp = $matchedIp
                        break
                    }
                }
            } catch {}
        }
        $sp.Close()
    } catch {
        Write-Notice "自動讀取序列埠日誌提示: $($_.Exception.Message)"
    }

    if ($detectedCameraIp) {
        Write-Success "已自動捕獲 ESP32 鏡頭 IP 位址：$detectedCameraIp"
    } else {
        Write-Notice "未在時限內捕獲即時 IP，將使用預設 mDNS (http://visualassist.local)"
    }
} else {
    Write-Notice "目前尚未偵測到 USB 序列埠 (COM)"
    Write-Info "小提示：若您剛插入 USB 傳輸線，Windows 可能正在自動載入驅動程式；"
    Write-Info "        或者 ESP32 正透過行動電源/Wi-Fi 連線。系統將直接開啟前端供您操作！"
}

# ------------------------------------------------------------------------------
# 步驟 4/5：啟動視覺輔助系統邊緣伺服器 (Vite Server)
# ------------------------------------------------------------------------------
Write-StepHeader 4 5 "啟動視覺輔助系統邊緣伺服器 (Vite Server)"

# 取得本機區域網路 IP
$localIP = "localhost"
try {
    $ipObj = Get-NetIPAddress -AddressFamily IPv4 -InterfaceAlias "Wi-Fi*", "Ethernet*", "區域連線*" -ErrorAction SilentlyContinue |
             Where-Object { $_.IPAddress -notmatch '^127\.' -and $_.IPAddress -notmatch '^169\.254\.' } |
             Select-Object -First 1
    if ($ipObj) {
        $localIP = $ipObj.IPAddress
    }
} catch {}

# 極速檢測連接埠 (避免 Test-NetConnection 延遲)
$portInUse = $false
try {
    $socket = New-Object System.Net.Sockets.TcpClient
    $iar = $socket.BeginConnect("127.0.0.1", $port, $null, $null)
    $portInUse = $iar.AsyncWaitHandle.WaitOne(300, $false)
    if ($portInUse) { $socket.EndConnect($iar) }
    $socket.Close()
} catch {
    $portInUse = $false
}

$serverProcess = $null
if ($portInUse) {
    Write-Success "前端伺服器已在運行中 ($hostUrl)"
} else {
    Write-Info "正在啟動 Vite 本地邊緣伺服器 (0.0.0.0:$port)..."

    # 使用 cmd.exe 啟動 npm run dev 確保 npm.cmd 在所有 Windows 版本正確執行
    $serverProcess = Start-Process -FilePath "cmd.exe" `
        -ArgumentList "/c $npmCmd run dev -- --host 0.0.0.0 --port $port" `
        -WorkingDirectory $mobileAppDir `
        -PassThru `
        -WindowStyle Hidden

    # 快速輪詢等待伺服器就緒
    $maxWaitSec = 20
    $isReady = $false
    for ($i = 0; $i -lt $maxWaitSec; $i++) {
        Start-Sleep -Milliseconds 800
        try {
            $testSock = New-Object System.Net.Sockets.TcpClient
            $asyncRes = $testSock.BeginConnect("127.0.0.1", $port, $null, $null)
            $connOk = $asyncRes.AsyncWaitHandle.WaitOne(300, $false)
            if ($connOk) {
                $testSock.EndConnect($asyncRes)
                $testSock.Close()
                $isReady = $true
                break
            }
            $testSock.Close()
        } catch {}
    }

    if ($isReady) {
        Write-Success "前端伺服器成功啟動！"
    } else {
        Write-Notice "伺服器已啟動背景進程，正在就緒中..."
    }
}

# ------------------------------------------------------------------------------
# 步驟 5/5：自動開啟瀏覽器前端畫面
# ------------------------------------------------------------------------------
Write-StepHeader 5 5 "自動開啟前端操作畫面"

# 若成功自動抓取到 ESP32 IP，直接將 ?camera= 參數自動注入給前端 URL
$launchUrl = $hostUrl
if ($detectedCameraIp) {
    $launchUrl = "$hostUrl/?camera=$detectedCameraIp"
}

Write-Success "正在開啟預設瀏覽器進入視覺輔助系統..."
Start-Process $launchUrl

Write-Host ""
Write-Host "==============================================================================" -ForegroundColor Green
Write-Host "               [系統已成功啟動！視覺輔助前瞻系統運行中]                      " -ForegroundColor White
Write-Host "==============================================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  * 本機畫面網址 : " -NoNewline; Write-Host "$launchUrl" -ForegroundColor Cyan
Write-Host "  * 手機同網網址 : " -NoNewline;
if ($detectedCameraIp) {
    Write-Host "http://${localIP}:${port}/?camera=$detectedCameraIp" -ForegroundColor Cyan
} else {
    Write-Host "http://${localIP}:${port}" -ForegroundColor Cyan
}
if ($detectedCameraIp) {
    Write-Host "  * 鏡頭硬體位址 : " -NoNewline; Write-Host "$detectedCameraIp (已自動配對綁定)" -ForegroundColor Green
} else {
    Write-Host "  * 鏡頭預設網址 : " -NoNewline; Write-Host "http://visualassist.local (mDNS)" -ForegroundColor Gray
}
Write-Host ""
Write-Host "  * 提示：" -ForegroundColor Yellow
Write-Host "    - 您可於手機瀏覽器輸入上述「手機同網網址」，即可在手機上體驗邊緣 YOLO 輔助避障" -ForegroundColor Gray
Write-Host "    - 接線模式：若有插上 ESP32-S3，本機已自動供電並維持通訊" -ForegroundColor Gray
Write-Host ""
Write-Host "  [快捷操作選單]" -ForegroundColor Cyan
Write-Host "    [O] 重新在瀏覽器開啟前端畫面"
Write-Host "    [P] 查看目前連接之 USB 序列埠"
Write-Host "    [S] 重設並監看 ESP32 序列埠日誌 (重新抓取 IP 位址)"
Write-Host "    [Q] 關閉伺服器並結束"
Write-Host ""

# 監聽鍵盤快捷操作
try {
    while ($true) {
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true).Key
            switch ($key) {
                "O" {
                    Write-Info "正在重新開啟瀏覽器..."
                    Start-Process $launchUrl
                }
                "P" {
                    Write-Info "重新掃描序列埠..."
                    $res = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
                    if ($res) {
                        foreach ($r in $res) { Write-Host "  -> $($r.DeviceID): $($r.Name)" -ForegroundColor Green }
                    } else {
                        Write-Notice "目前未發現已接線之序列埠裝置"
                    }
                }
                "S" {
                    Write-Info "正在重設 ESP32 並監聽序列埠日誌 (約 6 秒)..."
                    $ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
                    if ($ports) {
                        $targetPort = $ports[0].DeviceID
                        try {
                            $sp = New-Object System.IO.Ports.SerialPort $targetPort, 115200, None, 8, One
                            $sp.ReadTimeout = 600
                            $sp.Open()
                            $sp.DtrEnable = $false
                            $sp.RtsEnable = $true
                            Start-Sleep -Milliseconds 200
                            $sp.RtsEnable = $false
                            Start-Sleep -Milliseconds 100

                            $endTime = (Get-Date).AddSeconds(6)
                            while ((Get-Date) -lt $endTime) {
                                try {
                                    $line = $sp.ReadLine()
                                    if ($line) {
                                        Write-Host "  [ESP32] $line" -ForegroundColor Green
                                        if ($line -match 'http://(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})') {
                                            $detectedCameraIp = "http://$($Matches[1])"
                                            $launchUrl = "$hostUrl/?camera=$detectedCameraIp"
                                        }
                                    }
                                } catch {}
                            }
                            $sp.Close()
                            if ($detectedCameraIp) {
                                Write-Success "最新捕獲 IP: $detectedCameraIp (已更新網址，可按 [O] 開啟)"
                            }
                        } catch {
                            Write-Fail "序列埠讀取提示：$($_.Exception.Message)"
                        }
                    } else {
                        Write-Notice "目前未偵測到序列埠裝置"
                    }
                }
                "Q" {
                    Write-Notice "正在關閉服務..."
                    if ($serverProcess -and -not $serverProcess.HasExited) {
                        Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
                    }
                    Get-Process -Name "node" -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "*$mobileAppDir*" } | Stop-Process -Force -ErrorAction SilentlyContinue
                    Write-Success "服務已安全停止，感謝使用！"
                    Start-Sleep -Seconds 1
                    exit 0
                }
            }
        }
        Start-Sleep -Milliseconds 200
    }
}
finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
