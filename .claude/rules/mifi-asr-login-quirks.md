---
paths:
  - "ho_relay2/**"
---

# 隨身 WiFi（ASR 方案 MiFi）管理頁登入的陷阱

hoRelay2 1.10.x 從隨身 WiFi 管理頁讀電量（`pollMifi()`），2026-10-04 實機除錯踩到三個坑。

## 1. 登入請求本身也要帶 Authorization

`/login.cgi?Action=Digest&...` 這個請求要同時帶 `Authorization: Digest ... nc=00000001`
（格式與讀 `xml_action.cgi` 時相同），之後讀取從 nc=00000002 起算。
沒帶的話登入**照樣回 200**，但 session 不成立，讀 status1 只拿到
`<login_status>UNAUTHORIZED</login_status>`（94 bytes）。

依據是管理頁自己的 `js/base/ajax_calls.js` 的 `authentication()` 與 `js/base/utils.js` 的
`getAuthHeader()`。規格與網路上的範例常漏這條。**有疑問就直接抓管理頁的 JS 來看**，
它是唯一可信的規格。

## 2. 不要用電腦測試的結果證明流程正確

session 是**以 IP 綁定**的。電腦上開過管理頁（瀏覽器登入過），之後用 Python 照著錯的流程打，
讀取照樣成功，因為沿用的是瀏覽器的 session。驗證一律看設備自己發的 MQTT 狀態。

## 3. status1 回應慢

`xml_action.cgi?...file=status1` 實測要 1.6～1.9 秒才回，讀取逾時不能設 2 秒
（加上 Modem-sleep 收包延遲就超時）。目前連線逾時 2 秒、讀取逾時 8 秒。

## 除錯手法

ESP32-C3 序列埠常讀不到（見 `flash-and-serial-trustworthiness.md` 規則四）。
這次是暫時在 status JSON 加 `mifi_err` 欄位，放「失敗在哪一步 + HTTP code + body 前 120 字」，
從 MQTT 讀出來才看到 UNAUTHORIZED。查完要拿掉。

## 4. 電量精度看機種，不是韌體能改的

同是 ASR 方案（realm "Highwmg"、同一套管理頁），`Battery_voltage` 的內容因機種而異：

| 機種（status1 的 version_num） | Battery_voltage | Battery_charging |
|---|---|---|
| JZ10_ZHONGXING_20260123_V1.0.1 | 只給分段 `">20"` | 0 |
| MF808_HP_V51_SER_DE_LA_260116_CN | 精確百分比 `"70"` | 3（規格外） |

管理頁 JS 只是把值接上「%」顯示，不自己算。JZ10 機種已確認沒有其他管道
（`Engineer_parameter` 全空、只開 53／80 port），不要再花時間挖。
`Battery_charging` 各機種定義不同，MF808 的管理頁完全不看，判斷充電一律用 `Battery_charge`。
