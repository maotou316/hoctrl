# 齁控韌體 — VSCode 一鍵編譯／燒錄
#
# 用法：
#   .\flash.ps1                      # 編譯 ho_relay2
#   .\flash.ps1 -Model 2 -Upload     # 編譯並燒錄 ho_relay2（自動偵測 COM 埠）
#   .\flash.ps1 -Upload -KeepConfig  # 燒錄但不抹 flash（保留 EEPROM 的 WiFi/MQTT 設定）
#   .\flash.ps1 -Upload -Port COM17  # 手動指定埠
#
# 註：這裡用的是 A:\server\arduino-cli 的 arduino-cli 1.3.1，不是 VSCode Arduino
#     擴充自帶的 0.31.0。擴充的板子清單解析不了 esp32 core 3.3.7，所以改走指令。

param(
    [ValidateSet('1', '2', '3', 'master', 'master-c3', 'slave', 'test')]
    [string]$Model = '2',
    [switch]$Upload,
    [switch]$KeepConfig,
    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'
$cli = 'A:\server\arduino-cli\arduino-cli.exe'
$root = $PSScriptRoot

# ── 各型號硬體設定（與 publish.py 的 MODEL_CONFIGS 一致）──
$configs = @{
    '1' = @{
        Dir  = 'ho_relay1'
        Fqbn = 'esp32:esp32:esp32'
        Label = 'hoRelay1 (ESP32 WROOM)'
    }
    '2' = @{
        Dir  = 'ho_relay2'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'hoRelay2/hoRelay2-1 (ESP32-C3 MOSFET)'
    }
    '3' = @{
        Dir  = 'ho_relay3'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'hoRelay v3.0 齁斑自製電路板'
    }
    'master' = @{
        Dir  = 'ho_master1'
        # Task 6（BLE 配網）加入 BLE 後，板子預設的 OTA 雙槽分區（每槽 1.31MB）放不下
        # BLE + WiFi + MQTT + ESP-NOW 全部連結進同一顆映像（Bluedroid stack 本身就要
        # 數百 KB），編譯會超出 flash 容量 28%。改用 PartitionScheme=custom 讀
        # ho_master1/partitions.csv（app0/app1 各 1.94MB，仍保留 OTA 雙槽，Phase 4
        # 轉送 OTA 需要），總和剛好等於 WROOM DevKit 的 4MB flash。
        Fqbn = 'esp32:esp32:esp32:PartitionScheme=custom'
        Label = 'hoMaster1 (ESP32 WROOM，ESP-NOW 主控)'
    }
    'master-c3' = @{
        # 同一份 ho_master1/ho_master1.ino，靠 CONFIG_IDF_TARGET_ESP32C3 條件編譯切換 GPIO，
        # 不是複製出來的第二個 sketch。FQBN 照抄 slave 型號（同款 C3 開發板）。
        Dir  = 'ho_master1'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'hoMaster1-C3 (ESP32-C3，ESP-NOW 主控，同一份 sketch 條件編譯)'
    }
    'slave' = @{
        Dir  = 'ho_slave1'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'hoSlave1 (ESP32-C3，ESP-NOW 受控端)'
    }
    'test' = @{
        Dir  = 'ho_espnow_test'
        Fqbn = 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash={0},FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=default,UploadSpeed=921600,ZigbeeMode=default'
        Label = 'ESP-NOW 協定測試'
    }
}

$cfg = $configs[$Model]
$sketch = Join-Path $root $cfg.Dir
$outDir = Join-Path $sketch 'build\vscode'

# EraseFlash：預設 all（出廠乾淨狀態），-KeepConfig 則保留 EEPROM
$eraseFlash = if ($KeepConfig) { 'none' } else { 'all' }
$fqbn = if ($cfg.Fqbn -like '*{0}*') { $cfg.Fqbn -f $eraseFlash } else { $cfg.Fqbn }

if (-not (Test-Path $cli)) {
    Write-Host "找不到 arduino-cli：$cli" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $sketch)) {
    Write-Host "找不到專案目錄：$sketch" -ForegroundColor Red
    exit 1
}

Write-Host "型號：$($cfg.Label)" -ForegroundColor Cyan
if ($KeepConfig) {
    Write-Host "模式：保留 EEPROM 設定（EraseFlash=none）" -ForegroundColor Yellow
} else {
    Write-Host "模式：全抹 flash（EraseFlash=all，燒完需重新 BLE 配網）" -ForegroundColor Yellow
}

# ── 編譯 ──
Write-Host "`n[1/2] 編譯中…" -ForegroundColor Cyan
$libDir = Join-Path $root 'libraries'
& $cli compile --fqbn $fqbn --libraries $libDir --output-dir $outDir $sketch
if ($LASTEXITCODE -ne 0) {
    Write-Host "編譯失敗" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "編譯完成 → $outDir" -ForegroundColor Green

if (-not $Upload) {
    Write-Host "`n（未指定 -Upload，略過燒錄）" -ForegroundColor Gray
    exit 0
}

# ── 自動偵測 COM 埠 ──
if ([string]::IsNullOrWhiteSpace($Port)) {
    Write-Host "`n偵測 ESP32 序列埠…" -ForegroundColor Cyan
    $json = & $cli board list --format json | Out-String
    $detected = ($json | ConvertFrom-Json).detected_ports

    # 只認得出 matching_boards 的埠才是板子，其餘（藍牙虛擬埠等）忽略
    $candidates = @($detected | Where-Object {
        $null -ne $_.matching_boards -and $_.matching_boards.Count -gt 0
    })

    if ($candidates.Count -eq 0) {
        Write-Host "找不到 ESP32 裝置。請確認 USB 已接上，或用 -Port COMx 手動指定。" -ForegroundColor Red
        Write-Host "目前所有埠：" -ForegroundColor Gray
        $detected | ForEach-Object { Write-Host "  $($_.port.address)" -ForegroundColor Gray }
        exit 1
    }

    if ($candidates.Count -gt 1) {
        Write-Host "偵測到多台裝置，請用 -Port 指定其中一個：" -ForegroundColor Yellow
        $candidates | ForEach-Object {
            Write-Host "  $($_.port.address)  $($_.matching_boards[0].name)" -ForegroundColor Yellow
        }
        exit 1
    }

    $Port = $candidates[0].port.address
    Write-Host "找到：$Port（$($candidates[0].matching_boards[0].name)）" -ForegroundColor Green
}

# ── 燒錄 ──
Write-Host "`n[2/2] 燒錄到 $Port …" -ForegroundColor Cyan
& $cli upload -p $Port --fqbn $fqbn --input-dir $outDir $sketch
if ($LASTEXITCODE -ne 0) {
    Write-Host "燒錄失敗" -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "`n燒錄完成" -ForegroundColor Green

# ── 燒完再硬重置一次 ──
# ESP32-C3 內建 USB-Serial/JTAG 把 DTR/RTS 當自動下載電路用，esptool 上傳收尾關埠時
# 兩條線的切換順序可能把晶片留在 ROM 下載模式：韌體沒跑、GPIO 4/7 沒人驅動，
# P25 版繼電器會穩定常開直到按 RESET（2026-09-09 實測，誤判成硬體壞了）。
# 明確再做一次 --after hard-reset，實測 esptool 這條路徑每次都正常開機。
# 細節見 .claude/rules/usb-cdc-bench-artifacts.md
$esptool = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py" -Filter esptool.exe -Recurse -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if ($null -ne $esptool) {
    Write-Host "硬重置 $Port，確保離開下載模式…" -ForegroundColor Cyan
    Start-Sleep -Milliseconds 500
    & $esptool.FullName --chip auto -p $Port --after hard-reset chip-id 2>&1 | Select-String 'Hard resetting' | ForEach-Object { Write-Host "  $_" -ForegroundColor Green }
    Write-Host "之後要看 log 請用 .\monitor.ps1（dtr/rts 都關），不要開 IDE 的監視視窗" -ForegroundColor Yellow
} else {
    Write-Host "找不到 esptool，略過硬重置；請手動按一下板上 RESET" -ForegroundColor Yellow
}
