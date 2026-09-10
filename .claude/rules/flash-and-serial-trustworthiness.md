---
paths:
  - "ho_relay*/**"
  - "ho_master*/**"
  - "ho_slave*/**"
  - "flash.ps1"
---

# 燒錄與序列埠的可信度：不要相信「Hash of data verified」

## 教訓來源

2026-08-16 診斷 hoRelay2 繼電器故障時，有大半時間在追一個假問題：序列埠印出的 log 格式
與原始碼對不上。過程中提出過三個錯誤假設（編譯快取、OTA 分區、讀取腳本吃字），
真正原因是**三份東西各自不同步**：使用者編輯中的原始碼、Arduino IDE 燒進板子的 binary、
arduino-cli 編譯的 binary。

esptool 全程回報 `Hash of data verified`，那只證明「寫進去的位元組等於送出去的位元組」，
**不證明板子開機後執行的是它**。

## 規則一：燒錄後要 read-flash 驗證

改動有疑慮、或行為與程式碼對不上時，直接把 flash 讀回來比對，不要只信燒錄工具的回報。

```powershell
$esptool = "C:\Users\maoto\AppData\Local\Arduino15\packages\esp32\tools\esptool_py\5.1.0\esptool.exe"
& $esptool --chip esp32c3 -p COM21 -b 921600 read-flash 0x10000 0x40000 app0.bin
```

然後在 dump 裡搜尋一個**只存在於新版**的字串（改過的 log 格式、版本號都可以），
用 byte 級比對而不是 regex（PowerShell 對 1.4MB 字串做 regex 會有意外行為，
第一次就是這樣得到「兩份 bin 都含新格式」的錯誤結論）：

```powershell
function Find-Bytes($path, $needle) {
  $hay = [System.IO.File]::ReadAllBytes($path)
  $pat = [System.Text.Encoding]::UTF8.GetBytes($needle)
  for ($i = 0; $i -le $hay.Length - $pat.Length; $i++) {
    $ok = $true
    for ($j = 0; $j -lt $pat.Length; $j++) { if ($hay[$i+$j] -ne $pat[$j]) { $ok = $false; break } }
    if ($ok) { return $i }
  }
  return -1
}
```

## 規則二：使用者開著 Arduino IDE 時，燒錄可能被蓋掉

IDE 有自己的 build 目錄與快取，它燒的 binary 與 `arduino-cli` 編的**不是同一份**。
排查期間若對方開著 IDE，先講好由誰燒錄，否則會出現「我燒完、對方又燒一次」的競態，
而且兩邊都看不出來。

有疑慮時用 `--clean` 重編，繞開 sketch build cache。

## 規則三：版本號不能用來判斷 binary 新舊

`firmwareVersion` 是手寫常數，只有改版才會動。同一個版本號可能對應好幾份不同的 binary。
要區分必須靠 read-flash 比內容。

## 規則四：ESP32-C3 的序列埠讀不到，不代表韌體沒跑

實測遇過：`arduino-cli` 燒錄後序列埠完全讀不到（`.NET SerialPort` 與 `arduino-cli monitor`
都是 0 bytes），但 esptool 通訊正常、MQTT 也證明韌體跑得好好的。Arduino IDE 燒的版本則讀得到。
差異推測在板子選項組合，未查明。

**因應方式：讓診斷韌體把結果發到 MQTT，不要依賴序列埠。**
只要板子連得上 WiFi，這條路比序列埠可靠，而且不需要人在旁邊看。
PubSubClient 預設封包上限 256 bytes，發較大的 JSON 前要 `mqtt.setBufferSize(512)`。

## 規則五：序列埠讀取要累積 raw bytes 再一次解碼

`SerialPort.ReadExisting()` 逐次解碼時，中文字元（UTF-8 為 3 bytes）跨讀取邊界會被解壞，
**而且會連帶吃掉後面的字元**。這害我把韌體的正常輸出誤判成舊版格式。

正確寫法是累積 `byte[]`，讀完再 `[System.Text.Encoding]::UTF8.GetString()`：

```powershell
$all = New-Object System.Collections.Generic.List[byte]
$chunk = New-Object byte[] 4096
while (...) {
  try { $n = $sp.BaseStream.Read($chunk, 0, $chunk.Length); if ($n -gt 0) { $all.AddRange($chunk[0..($n-1)]) } } catch {}
}
$text = [System.Text.Encoding]::UTF8.GetString($all.ToArray())
```

另外讀取時要 `$sp.DtrEnable = $false`，避免 DTR 驅動 GPIO 9(BOOT)
（見 `button-pin-stuck-low.md`）。
