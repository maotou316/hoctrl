# 齁控韌體 — 不擾動晶片的序列埠監視
#
# 用法：
#   .\monitor.ps1                # 自動偵測 ESP32 埠
#   .\monitor.ps1 -Port COM23    # 手動指定
#   Ctrl+C 離開
#
# 為什麼不用 IDE 的監視視窗：ESP32-C3 內建 USB-Serial/JTAG 會把 DTR/RTS 當 esptool 的
# 自動下載電路用，IDE 開埠、關埠時兩條線的切換順序可能把晶片重置進 ROM 下載模式——
# 韌體停了、GPIO 4/7 沒人驅動，P25 版繼電器會穩定常開直到按 RESET。
# 這裡用 arduino-cli monitor 並把 dtr/rts 都設 off，開關都不會動到晶片。
# 細節見 .claude/rules/usb-cdc-bench-artifacts.md

param(
    [string]$Port = '',
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'
$cli = 'A:\server\arduino-cli\arduino-cli.exe'

if (-not (Test-Path $cli)) {
    Write-Host "找不到 arduino-cli：$cli" -ForegroundColor Red
    exit 1
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $json = & $cli board list --format json | Out-String
    $ports = @(($json | ConvertFrom-Json).detected_ports | Where-Object {
        $null -ne $_.matching_boards -and $_.matching_boards.Count -gt 0
    })
    if ($ports.Count -eq 0) {
        Write-Host "找不到 ESP32 裝置。請確認 USB 已接上，或用 -Port COMx 手動指定。" -ForegroundColor Red
        exit 1
    }
    if ($ports.Count -gt 1) {
        Write-Host "偵測到多台裝置，請用 -Port 指定其中一個：" -ForegroundColor Yellow
        $ports | ForEach-Object { Write-Host "  $($_.port.address)  $($_.matching_boards[0].name)" }
        exit 1
    }
    $Port = $ports[0].port.address
}

Write-Host "監視 $Port @ $Baud（dtr=off, rts=off，Ctrl+C 離開）" -ForegroundColor Cyan
& $cli monitor -p $Port -c "baudrate=$Baud,dtr=off,rts=off"
