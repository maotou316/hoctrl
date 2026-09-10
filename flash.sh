#!/usr/bin/env bash
# 齁控韌體 — flash.ps1 的 bash 包裝
#
# 用法：
#   ./flash.sh                    # 編譯 ho_relay2（預設型號，不燒錄）
#   ./flash.sh slave -u           # 編譯並燒錄 ho_slave1（自動偵測埠）
#   ./flash.sh master-c3 -u       # 編譯並燒錄 ho_master1（C3 版）
#   ./flash.sh slave -u -k        # 燒錄但保留 EEPROM（WiFi／配對設定不清）
#   ./flash.sh slave -u -p COM23  # 手動指定埠
#   ./flash.sh -l                 # 列出目前接著的板子與埠號
#
# 型號設定（FQBN、分區、EraseFlash）只存在於 flash.ps1，本檔不複製一份，
# 純粹轉呼叫。改硬體設定請改 flash.ps1。

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ps1="$here/flash.ps1"
cli="/a/server/arduino-cli/arduino-cli.exe"

model=''
args=()

while [ $# -gt 0 ]; do
  case "$1" in
    -u|--upload)     args+=(-Upload) ;;
    -k|--keep)       args+=(-KeepConfig) ;;
    -p|--port)       shift; args+=(-Port "$1") ;;
    -l|--list)       exec "$cli" board list ;;
    -h|--help)       awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' \
                       "${BASH_SOURCE[0]}"; exit 0 ;;
    -*)              echo "未知選項：$1（-h 看用法）" >&2; exit 1 ;;
    *)               model="$1" ;;
  esac
  shift
done

[ -n "$model" ] && args+=(-Model "$model")

if ! command -v powershell >/dev/null 2>&1; then
  echo "找不到 powershell —— flash.ps1 需要它才能執行" >&2
  exit 1
fi

exec powershell -ExecutionPolicy Bypass -File "$ps1" "${args[@]}"
