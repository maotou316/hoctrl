---
paths:
  - "ho_relay*/**"
  - "ho_slave*/**"
---

# 繼電器恆開／恆關的診斷流程（純軟體定位到元件）

## 前置條件：一定要先接外接電源

**繼電器的負載回路靠外接電源供電，沒接電源時負載永遠不會通電，任何觀察都無效。**

2026-08-16 在 hoRelay2 花了三輪測試（12 腳全掃描、GPIO 4 單獨、GPIO 7 單獨）才發現當時
根本沒接外接電源，「負載完全不動」是因為沒有電，不是因為繼電器不受控。三輪資料全部作廢。

動手前先確認：外接電源已接、負載已接、而且**負載本身是好的**（先手動短接繼電器輸出端驗證負載會亮）。

## 第一步：繼電器還受不受 GPIO 控制

最省事的方法是**用 MQTT 下指令**，不必燒任何診斷韌體：

```powershell
# 連發 12 次 ON，每次韌體點動 2 秒，間隔 6 秒 → 負載應呈現「通電 2 秒 / 斷 4 秒」的節奏
for ($i = 1; $i -le 12; $i++) {
  python mqtt_pub.py "hoban/<device-id>/control" "ON"
  Start-Sleep -Seconds 6
}
```

`mqtt_pub.py` / `mqtt_sub.py` 是手刻的極簡 MQTT 3.1.1 客戶端（只用 Python 標準函式庫，
不需要 paho-mqtt），留在 scratchpad，需要時重寫也只有幾十行。

要燒診斷韌體時，用「LED 節拍器」寫法：把待測腳與面板 LED(GPIO 0) 一起每 5 秒 LOW/HIGH 交替。
**LED 亮 = 全高、LED 滅 = 全低**，觀察者只要看負載有沒有跟著 LED 走，不必盯序列埠、不必按鍵。
（曾做過「按 BOOT 鍵標記」的版本，但人得守在旁邊，實務上會錯過整輪。）

- 負載跟著節奏變 → 繼電器正常，恆開另有原因
- 負載完全不動 → 進第二步

## 第二步：分辨「gate 斷線」還是「gate 短路」

MOS 的 gate 是個電容（數百 pF）。把腳位充到高電位後**放掉驅動**、讓內部下拉（約 45kΩ）
洩電荷，量電位翻轉要多少 CPU cycle：

| 狀況 | 下降 cycles @160MHz | 判定 |
|---|---|---|
| 接著正常 gate（~500pF） | 約 3500（τ≈22μs） | 驅動路徑正常 |
| 走線斷／虛焊（只剩 pad 電容 ~5pF） | 數十（跟浮空腳相同） | **補焊可救** |
| gate 被外部低阻抗源釘住 | 撞到計數上限 | **短路，換 MOS** |

**不可以用 `pinMode()` 切換**：它要重設 IO_MUX 與 GPIO matrix，耗時十幾 μs，比要量的 22μs
還久，電位在開始讀之前就掉完了。第一版就是這樣，對照組全部歸零、毫無分辨力。
正確做法是預先設好「輸出 + 內部下拉」，量測瞬間只用一個 store 關掉 output enable：

```c
GPIO.enable_w1tc.val = mask;          // 放掉驅動，只剩內部下拉洩電荷
uint32_t t0 = esp_cpu_get_cycle_count();
while ((GPIO.in.val >> pin) & 1) { n = esp_cpu_get_cycle_count() - t0; if (n > CEILING) break; }
```

**判讀前先檢查對照組**：清單裡要同時放「確定沒外接的腳」（板上未引出的 GPIO）和「確定有外接
負載的腳」（面板 LED）。兩者讀數若相同，代表這次量測沒有分辨力，**數據作廢，不可硬解讀**。

## 第三步：ADC 量 gate 實際電位（只有 GPIO 0~4 可用）

ESP32-C3 的 ADC1 只涵蓋 GPIO 0~4，所以 Y176 版（繼電器在 GPIO 4）量得到、P25 版（GPIO 7）量不到。

量兩次：①腳位浮空時的電位 ②強制 `OUTPUT LOW` 50ms 後放手再量。
**關鍵在第二次**——正常腳位拉低後電位會明顯降下來，被外部源釘住的腳則毫無變化。

ADC 滿檔約 3100mV，讀到飽和只代表「至少到滿檔」，無法分辨 3.3V 還是更高。

## 致命陷阱：兩版板子的「正常 gate 電位」是相反的

**不可以用「gate 電位高就是故障」下判斷。** 2026-08-17 實測一張**功能正常**的 P25 版板子：

```
[pin, 下降cycles, 上升cycles]
[4,   28,      27]    ← 空接腳
[7,   200008,  333]   ← 繼電器腳，撞上限 —— 但這張板繼電器完全正常
```

P25 版（繼電器在 GPIO 7）放掉驅動後 gate 會停在高電位、1.25 ms 內不會靠內部下拉洩掉；
Y176 版（GPIO 4）則會掉下來。所以同一個讀數在兩版上意義相反。

**不要把「放手後停在高電位」解讀成「靠拉低導通」**（2026-09-09 曾這樣寫，錯）：
兩版韌體都是 HIGH＝開，正常的 P25 板照這樣跑得好好的，P25 一定也是 HIGH 才導通。
那個讀數只說明 gate 前級的電容或偏壓讓它洩得慢，不說明導通極性。

曾據此在韌體加過 `relay_fault` 自檢，一上線就在正常的 P25 板上持續誤報，**當天撤回**。
要重做必須先能分辨板版，或改用不依賴絕對電位的判準。

**連帶的方法論警告**：診斷時只用「同板的空接腳」當基準是不夠的——空接腳不等於
「正常的 gate 腳」。要對故障下定論，必須有一張**同版本的正常板**做對照。
下面那筆 2026-08-16 的資料就缺這個對照，所以「gate 被短路」是推論而非定論。

## 2026-08-16 實測資料（hoRelay2，MAC 28:37:2f:4b:69:e4，341305A_Y176_250318）

11 輪讀數完全一致：

```
[pin, 下降cycles, 上升cycles]        [pin, 浮空mV, 拉低後放手mV]
[4,   200008,     26]  ← gate       [4,  2814,   2814]  ← 零回落
[7,   28,         27]               [2,  562,    503]   ← 浮空對照
[0,   28,         45]  ← LED 對照    [3,  1442,   623]   ← LED 對照，明顯下降
[6,   28,         27]  ← 浮空基準
[10,  28,         27]  ← 浮空基準
```

推論：gate 被外部釘在 2.81V（遠高於閘極門檻）→ MOS 恆導通 → 繼電器恆開，換 MOSFET。

**但這是推論不是定論**——缺一張正常的 Y176 板做對照（見上一節）。真正確定的只有
「這張板故障」這件事，那是行為證據：接上外接電源後連下 12 次 ON/OFF，負載完全無反應、
恆通電。至於故障機制的解釋，唯一旁證是它的上升只有 26 cycles，而正常接著 gate 的腳
（那張 P25 板）是 333 cycles，差一個數量級，代表壞板那支腳缺少 gate 該有的電容性。

## 2026-09-09 實測資料（hoRelay2，MAC 10:B4:1D:4A:FE:5C，P25 版）——板子沒壞

**這一筆的最終結論是「板子正常」。** 當晚曾寫成「gate 側正常但恆開 → MOS D-S 貫通、換 MOSFET」，
隔天推翻：恆開全部發生在**晶片根本沒在跑韌體**的時候，機制見 `usb-cdc-bench-artifacts.md`。

9 輪 gate 讀數完全一致（這部分仍有效，是 P25 版第二筆「正常板」對照）：

```
[pin, 下降cycles, 上升cycles]
[7,   200005,  260]   ← 繼電器 gate（P25），對照另一張正常 P25 板是 [7, 200008, 333]
[4,   25,      26]    ← 空接腳（P25 版未接 MOS）
[0,   709,     926]   ← 面板 LED 對照，有外接負載
[3,   61,      62]    ← 板載 LED 對照，有外接負載
[6,   25,      26]    ← 浮空基準
[10,  25,      26]    ← 浮空基準
```

**判準（P25 版看上升，不看下降）**：下降撞上限是 P25 的正常表現，沒有鑑別力。
上升 260 與另一張正常 P25 板的 333 同數量級，而空接腳只有 26。

### 當晚誤判的過程，下次別再走一遍

1. 燒完韌體「繼電器常開」→ 其實 esptool 關埠時 DTR/RTS 把 C3 弄進 ROM 下載模式，韌體沒跑
2. 用 MQTT 下 ON/OFF，序列埠開著、韌體有回應，使用者卻回報「負載完全不動、恆通電」
   → 據此判定硬體壞。**事後看這個觀察對不上其他所有證據**，但當時沒有追問就下了定論
3. 燒回韌體後使用者說「正常了」；關掉監視視窗又常開 → 又是下載模式（序列埠只剩 ROM banner）
4. 用 esptool 硬重置、**完全不開序列埠**、純 MQTT 查狀態：韌體 `relay:0`，負載斷 → 板子沒壞

**教訓**：
- 「韌體說關、負載卻通」要先確認**韌體真的在跑**（開機秒數、能不能回 MQTT），不是先懷疑 MOS
- 使用者一句與其他證據矛盾的觀察，要當場重測，不能直接當結論的基石
- 本規則第一步「用 MQTT 下指令」在 USB 接著電腦時有第二個陷阱（HWCDC 阻塞，同上檔案），
  指令可能根本沒被處理；1.8.1 起 `Serial.setTxTimeoutMs(0)` 已修，舊韌體要開著監視測

### 診斷 sketch 的可用寫法（實測跑得動）

用 `soc/gpio_reg.h` 的暫存器宏，比直接碰 `GPIO.` struct 穩（ESP-IDF 各版欄位名不同）：

```c
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "esp_cpu.h"

// toHigh=false：充高後放手，量掉到 LOW 幾 cycle（配內部下拉）
// toHigh=true ：拉低後放手，量升到 HIGH 幾 cycle（配內部上拉）
uint32_t measure(int pin, bool toHigh) {
  const uint32_t mask = 1UL << pin;
  gpio_set_pull_mode((gpio_num_t)pin, toHigh ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
  REG_WRITE(GPIO_ENABLE_W1TS_REG, mask);
  REG_WRITE(toHigh ? GPIO_OUT_W1TC_REG : GPIO_OUT_W1TS_REG, mask);
  delayMicroseconds(500);                      // 等 gate 電容充飽
  uint32_t n = 0;
  portDISABLE_INTERRUPTS();
  REG_WRITE(GPIO_ENABLE_W1TC_REG, mask);       // 只用一個 store 放掉驅動
  uint32_t t0 = esp_cpu_get_cycle_count();
  while (true) {
    uint32_t level = (REG_READ(GPIO_IN_REG) >> pin) & 1;
    if (toHigh ? level : !level) break;
    n = esp_cpu_get_cycle_count() - t0;
    if (n > 200000) break;                     // CEILING
  }
  portENABLE_INTERRUPTS();
  REG_WRITE(GPIO_ENABLE_W1TS_REG, mask);       // 收尾一律回到輸出 LOW
  REG_WRITE(GPIO_OUT_W1TC_REG, mask);
  return n;
}
```

- 兩個方向都要量。P25 版只看下降會得到「撞上限」，跟正常板無從分辨
- `setup()` 第一行仍要 `initRelayPins()`，每輪測完再拉低一次（測試會把 gate 充高 500μs）
- sketch 燒進去用 `EraseFlash=none`，測完 `.\flash.ps1 -Model 2 -Upload -KeepConfig` 燒回，
  EEPROM 的 WiFi 設定不會掉，省一次 BLE 配網
- 每 3 秒重印一輪：ESP32-C3 開 `CDCOnBoot=cdc` 時 USB CDC 會重新列舉，
  只在 `setup()` 印一次的話，序列埠還沒接上就印完了

### 判斷「韌體到底有沒有在跑」的取證方式

`CDCOnBoot=cdc` 下開序列埠常常只讀到 `ESP-ROM:esp32c3-...` 就沒了，那是 USB CDC 重新列舉
把 handle 弄失效，**不代表韌體沒跑**。要看開機訊息就用 esptool 硬重置後立刻重連：

```powershell
& $esptool --chip esp32c3 -p COM14 --after hard-reset flash-id
# 接著迴圈重試 Open COM14（DtrEnable/RtsEnable 都設 $false）
```

**.NET SerialPort 的 `RtsEnable = $true` 會 assert RTS，把 EN 拉低讓晶片一直卡在 reset**，
全程讀不到東西。要讓晶片正常跑，DTR 與 RTS 都設 `$false`。

## 順帶確認的事

- 韌體的 `initRelayPins()` 雙腳位拉低是對的，在這種硬體故障下只是敵不過外部短路源
- 兩版板子都沒有 gate 下拉電阻（見 `ho_relay2/readme.md`）。補上 10kΩ 之後，「gate 失去驅動」
  會表現成恆關而不是恆開——對捕捉籠來說失效方向從危險變安全，下一版 layout 應優先處理
- 現有硬體**無法**讓韌體自己偵測繼電器實際狀態：可外接的腳只有 GPIO 0（面板 LED）與繼電器腳，
  沒有空腳可拉回授線。曾嘗試改用「讀繼電器腳自身電位」取代回授線，被上面那個
  兩版正常狀態相反的陷阱擋掉。要做到「App 顯示的開關狀態是真的」，仍須下一版加回授線
- 順帶修掉的兩個與板子好壞無關、所有板都受影響的韌體問題：
  - `pulseRelay()` 是開 2 秒 → 關 → 才發布狀態，外部永遠觀測不到開啟中的狀態，
    `relay` 欄位恆為 0。已改成開啟後立刻補發一次
  - PubSubClient 預設封包上限 256 bytes，狀態 JSON 約 200 bytes 加 topic 35 bytes 就超過，
    `publish()` 靜默回傳 false 而序列埠照印「已發布狀態」。已加 `setBufferSize(512)`
    並改為檢查回傳值後才印成敗
