---
paths:
  - "**/*.ino"
  - "flash.ps1"
---

# flash.ps1 在子代理環境可能誤判編譯失敗（主控端正常）

## 觀察到的現象

編譯**含 BLE 的 sketch**（目前是 `ho_master1`，兩種型號都會）時：

- **主控端用 PowerShell 工具直接跑** `.\flash.ps1 -Model master` → **exit 0，編譯完成**（2026-08-16 實測兩次）
- **子代理跑同一條指令** → 回報「腳本提前中止」，兩個獨立的子代理（Phase 2a Task 6、Phase 4 探針）都遇到

## 成因推測

`flash.ps1` 開頭有 `$ErrorActionPreference = 'Stop'`。Bluedroid 的 BLE 標頭會引出
`#pragma message ("BT: forcing BR/EDR max sync conn eff to 1 ...")`，arduino-cli 把它寫到 stderr。
Windows PowerShell 5.1 對 native command 的 stderr 有特殊處理，某些呼叫路徑下會把
stderr 行轉成終止性錯誤——即使 arduino-cli 本身的 exit code 是 0。

只在 BLE 這條路徑現形，因為只有它會印 pragma message。不含 BLE 的 `slave`／`test`／
`ho_relay2` 都不受影響。

**沒有結論說 `flash.ps1` 壞了**——主控端明確可用。差異可能來自子代理與主控端的 shell
呼叫方式不同。**不要為此去改 `flash.ps1`**，已有兩個子代理提議要改，都被實測駁回。

## 務實處理

派子代理做韌體編譯驗證時，在 dispatch 裡直接寫明：

> 若 `.\flash.ps1 -Model xxx` 在編譯成功的情況下仍中止，改用它內部呼叫的同一組指令：
> ```
> A:\server\arduino-cli\arduino-cli.exe compile --fqbn <照 flash.ps1 的 $configs> \
>   --libraries A:\project\hoctrl_arduino\libraries \
>   --output-dir <sketch>\build\vscode <sketch>
> ```
> 這只繞過腳本外殼，編譯步驟完全等價（沒有 `-Upload` 時腳本也只做編譯這一步）。
> 回報時要說明用了哪一種，不要宣稱 `flash.ps1` 壞了。

另外提醒子代理：`flash.ps1` 找不到時通常是**工作目錄不對**，先
`Set-Location A:\project\hoctrl_arduino`，不是腳本有問題。

## 附帶：arduino-cli 顯示的 flash 百分比不可信

用 custom 分區（`PartitionScheme=custom`）時，arduino-cli 印的百分比是拿**整顆晶片**
當分母（例如 16MB），不是實際的 app0 分區。

真實分母請看 `partitions.csv` 的 `app0`：**0x1F0000 = 2,031,616 bytes**（master 與 slave 共用同一份）。

例：master 印「10%」實際是 **82.86%**；slave 印「5%」實際是 **48.2%**。
報告用量時一律換算成對 app0 的百分比。
