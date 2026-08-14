# CLAUDE.md 待補內容（Phase 1 收尾，暫緩併入）

本文件是 Task 7（Phase 1 收尾）原本要直接寫進 `CLAUDE.md` 的兩段內容。
因為執行當下 `CLAUDE.md` 已有其他人尚未提交的修改，而 `git add` 是整檔操作，
若連同這兩段一起 commit，會把別人還沒完成的工作一併提交，屬於不可逆的協作事故，
所以先原封不動整理在這裡，請維護者確認 `CLAUDE.md` 目前的未提交修改處理完畢後，
自行選擇時機把以下兩段貼回 `CLAUDE.md`。

---

## 插入點 1：「## 硬體型號」章節，`### hoRelay2` 區塊之後

```markdown
### hoMaster1 / hoSlave1（ESP-NOW 多機聯動）

一台 master 透過 ESP-NOW 控制最多 20 台 slave，用於「一鍵同時觸發多台」
與「slave 現場沒有網路」的場景。設計文件見
`docs/superpowers/specs/2026-08-14-esp32-master-slave-design.md`。

- **hoMaster1**（`ho_master1/`）：ESP32 WROOM，對外 MQTT 窗口 + ESP-NOW 主控，繼電器選配
- **hoSlave1**（`ho_slave1/`）：沿用 hoRelay2 的 C3 繼電器板，不連 WiFi

兩者共用 `libraries/HoEspNow/` 的協定定義，編譯時需帶 `--libraries`（`flash.ps1` 已處理）。

協定摘要：封包 7 bytes 標頭（magic/version/type/seq/crc），
CRC 混入共享密鑰過濾誤觸發，不使用 ESP-NOW 原生加密（原生加密 peer 上限只有 6 台）。

各 sketch 的詳細說明見 `ho_master1/readme.md` 與 `ho_slave1/readme.md`。
```

---

## 插入點 2：「### 編譯與上傳」章節的程式碼區塊之後

```markdown
本專案實際燒錄一律走 `flash.ps1`（見 `.claude/rules/vscode-arduino-toolchain.md`）：

```powershell
.\flash.ps1 -Model 2 -Upload          # hoRelay2
.\flash.ps1 -Model master -Upload     # hoMaster1
.\flash.ps1 -Model slave -Upload      # hoSlave1
.\flash.ps1 -Model test -Upload       # ESP-NOW 協定測試
```
```
