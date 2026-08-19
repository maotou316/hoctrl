---
paths:
  - "**/*.ino"
  - "**/*.dart"
---

# 並行子代理不可碰同一個檔案，「改完會還原」也算

## 2026-08-16 實際發生

同時派了兩個背景 agent：

- **A**：Phase 4 flash 探針 —— 在 `ho_master1.ino` 暫時加 `#include <HTTPClient.h>` 等量測用的程式碼，量完**用 `git checkout --` 還原**
- **B**：Phase 2b Task 1 —— 正常編輯同一個 `ho_master1.ino`

A 的還原動作把 B 進行到一半的編輯整份洗掉。B 偵測到檔案被重置回 HEAD、重做一次、並在報告裡誠實指出「多 agent 共用同一份工作目錄有並發寫入風險」。

**這是編排者的錯，不是任一 agent 的錯。** 兩個 agent 各自的行為都正確。

## 規則

派並行 agent 前，先列出每個 agent 會**寫入**的檔案，確認集合不相交。特別注意這幾種容易被忽略的寫入者：

- **「臨時修改後還原」的量測型任務**（探針編譯、效能量測）—— 它的 `git checkout --` / `git stash` 是**破壞性**的，會連別人的改動一起吃掉
- **格式化 / lint 自動修正**（`dart format`、`flutter fix --apply`）
- **會 commit 的任務** —— `git add` 到一半的檔案會把別人未完成的編輯一起提交

## 可以安全並行的組合

- **不同 repo**：`hoctrl_arduino` 與 `hoctrl` 完全獨立，隨便並行
- **唯讀任務**：Explore、盤點、審查（只讀 diff 檔）
- **寫入不同檔案**：例如一個寫 `ho_master1.ino`、一個寫 `ho_slave1.ino`（但要注意兩者都會跑 `flash.ps1`，編譯輸出目錄 `build/vscode` 目前是**共用**的，會互相覆蓋 `.bin`——只編譯不燒錄時無害，要取用產物時要小心）

## 不可並行的組合

- 兩個 agent 都寫同一個 `.ino` / `.dart`
- 任一方會 `git checkout --` / `git stash` / `git reset` 該檔案
- 兩個 agent 都會 commit（即使檔案不同，也可能互相把對方未完成的改動 `git add` 進去）

## 若真的需要量測型任務

量測與實作不要並行。順序是：**先量完、還原、確認 `git status` 乾淨，再派實作**。
或者讓量測 agent 在**複本**上做（`cp ho_master1.ino /tmp/probe.ino`），完全不碰工作目錄的原檔。
