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
    [switch]$Reset   # 開始前先用 DTR 重置板子
)

$ErrorActionPreference = 'Stop'
$cli = 'A:\server\arduino-cli\arduino-cli.exe'
$logFile = Join-Path $env:TEMP "ho_serial_$(Get-Random).log"

if ($Reset) {
    # 開關一次序列埠讓板子重新啟動，才能抓到 setup() 的輸出
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port, 115200
        $sp.DtrEnable = $false
        $sp.Open()
        Start-Sleep -Milliseconds 200
        $sp.Close()
        Start-Sleep -Milliseconds 500
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
