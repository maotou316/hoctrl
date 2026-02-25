## 韌體發布工具 (publish.py)

hoRelay 韌體發布自動化腳本，支援 hoRelay1～3。自動完成版本遞增、編譯、上傳至 GitHub Releases / Firebase Storage，並更新 Firestore 記錄。

### 必要工具

| 工具 | 用途 | 安裝方式 |
|------|------|----------|
| arduino-cli | 編譯韌體 | https://arduino.github.io/arduino-cli/ |
| firebase | Firestore 更新 | `npm install -g firebase-tools` |
| gh | GitHub Releases 上傳（優先） | https://cli.github.com/ |
| gsutil | Firebase Storage 上傳（備選） | https://cloud.google.com/storage/docs/gsutil_install |

### 用法

```bash
# 互動式選擇型號
python publish.py

# 指定型號發布
python publish.py 1                       # hoRelay1 (ESP32 WROOM)
python publish.py 2                       # hoRelay2 (ESP32-C3)
python publish.py 3                       # hoRelay3 (ESP32-C3)

# 附帶更新說明
python publish.py 2 -c "修正 WiFi 連接問題"

# 跳過確認直接發布
python publish.py 1 -y

# 指定最低版本要求
python publish.py 2 -m 1.2.0

# 組合使用
python publish.py 3 -c "新增藍牙配對功能" -m 1.0.0 -y
```

### 參數說明

| 參數 | 說明 | 預設值 |
|------|------|--------|
| `relay` | 型號編號 (1/2/3)，不帶則進入互動選單 | 無 |
| `-c`, `--changelog` | 更新說明 | `修正錯誤，優化效能` |
| `-m`, `--min-version` | 最低版本要求 | `1.0.0` |
| `-y`, `--yes` | 跳過確認直接發布 | 否 |

### 發布流程

1. **檢查工具** — 確認 arduino-cli、firebase 等已安裝
2. **讀取韌體資訊** — 從 `.ino` 讀取目前版本號與設備型號
3. **版本遞增** — 自動將末位版本號 +1（例如 1.3.9 → 1.3.10）
4. **編譯韌體** — 使用 arduino-cli 編譯，產出 `.bin` 檔案
5. **上傳韌體** — 依序嘗試 GitHub Releases → Firebase Storage → gsutil → 手動上傳
6. **更新 Firestore** — 寫入 `firmware_updates/{model}` 文件，設備下次連線時收到更新通知

### 上傳優先順序

1. **GitHub Releases**（需安裝 gh CLI）— 上傳至 `maotou316/hoctrl-firmware`
2. **Python Firebase Storage**（需安裝 `google-cloud-storage`）
3. **gsutil**（需安裝 Google Cloud SDK）
4. **手動上傳** — 提示開啟 Firebase Console 手動操作
