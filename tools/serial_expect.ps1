# 讀取序列埠輸出並比對關鍵字，供韌體驗證使用。
#
# 用法：
#   .\tools\serial_expect.ps1 -Port COM13 -Expect "ALL TESTS PASSED" -Seconds 10
#   .\tools\serial_expect.ps1 -Port COM13 -Seconds 15            # 只印出，不比對
#
# 註：環境沒有 pyserial，這裡走 arduino-cli 1.3.1 的 monitor 指令。

param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$Expect = '',
    [int]$Seconds = 10,
    [switch]$Reset   # 開始前用 RTS 脈衝硬體重啟板子，才抓得到 setup() 的開機輸出
)

$ErrorActionPreference = 'Stop'
$cli = 'A:\server\arduino-cli\arduino-cli.exe'
$logFile = Join-Path $env:TEMP "ho_serial_$(Get-Random).log"

if (-not (Test-Path $cli)) {
    Write-Host "找不到 arduino-cli：$cli" -ForegroundColor Red
    exit 1
}

if ($Reset) {
    # ESP32 自動下載電路：DTR 驅動 GPIO 9(BOOT)、RTS 驅動 EN。
    # 因此硬體重啟要用 RTS 短暫拉高（EN 拉低 → 進 reset）再放開，
    # 而 DTR 必須全程維持 false，否則會把 GPIO 9 拉低：板子會改進 UART 下載模式，
    # 且與「按鈕腳位卡在 LOW」的症狀無法區分（見 .claude/rules/button-pin-stuck-low.md）。
    # 舊版只開關一次序列埠、完全沒碰 RTS，實際上是 no-op，抓不到開機輸出。
    #
    # 已知限制：上述自動下載電路存在於「有 USB-serial 橋接晶片」的板子
    #（CP2102／CH340，例如 master 的 ESP32 WROOM DevKit）。
    # 若板子走 ESP32-C3 原生 USB CDC（FQBN 帶 CDCOnBoot=cdc，例如 hoSlave1），
    # RTS／DTR 並沒有接到 EN／GPIO 9，這個 -Reset 可能仍然無效 ——
    # 此時序列埠不會出現 setup() 的開機訊息，請手動按板子上的 EN 鍵重啟，
    # 這不是操作錯誤。（尚未在實體板子上確認過屬於哪一種。）
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, 115200
        $sp.Open()
        $sp.DtrEnable = $false
        $sp.RtsEnable = $true    # 拉高 RTS → EN 拉低 → 板子進入 reset
        Start-Sleep -Milliseconds 100
        $sp.RtsEnable = $false   # 放開 → 板子重新啟動
        $sp.Close()
        # 只等到序列埠釋放即可，太久會錯過開機訊息
        Start-Sleep -Milliseconds 200
    } catch {
        Write-Host "重置序列埠失敗（可忽略）：$_" -ForegroundColor Yellow
    }
}

Write-Host "監聽 $Port，共 $Seconds 秒…" -ForegroundColor Cyan

$proc = Start-Process -FilePath $cli `
    -ArgumentList 'monitor', '-p', $Port, '--config', 'baudrate=115200', '--quiet' `
    -RedirectStandardOutput $logFile -PassThru -NoNewWindow

Start-Sleep -Seconds $Seconds
if (-not $proc.HasExited) { $proc.Kill() }
Start-Sleep -Milliseconds 300

$output = if (Test-Path $logFile) { Get-Content $logFile -Raw -Encoding UTF8 } else { '' }
Remove-Item $logFile -ErrorAction SilentlyContinue

Write-Host "───── 序列埠輸出 ─────" -ForegroundColor Gray
Write-Host $output
Write-Host "──────────────────────" -ForegroundColor Gray

if ([string]::IsNullOrWhiteSpace($Expect)) { exit 0 }

if ($output -match [regex]::Escape($Expect)) {
    Write-Host "通過：找到「$Expect」" -ForegroundColor Green
    exit 0
} else {
    Write-Host "失敗：找不到「$Expect」" -ForegroundColor Red
    exit 1
}
