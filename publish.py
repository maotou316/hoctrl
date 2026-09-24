#!/usr/bin/env python3
"""
hoRelay 韌體發布自動化腳本（支援 hoRelay1～3）
此腳本會編譯韌體、上傳到 GitHub Releases / Firebase Storage，並登記發佈資料：
  - Firestore hoctrl：hoCtrl（齁控）App 讀這裡
  - HoLuCam 後台（MySQL FirmwareRelease）：HoLuCam App 與網頁後台現在以這裡為準
  - Firestore holucam-be6c6：只為還沒升級的舊版 HoLuCam App 保留

環境變數（HoLuCam 後台登記用）:
  HOLUCAM_FIRMWARE_PUBLISH_TOKEN  後台發版 token；沒設就略過後台登記（黃色警告，不中止發版）
  HOLUCAM_API_BASE                後台網址，預設 https://holucam.neuter.online
                                  （必須 https；只有 http://localhost、http://127.0.0.1 可用 http）

用法:
  python publish.py 2                     # 發布 hoRelay2
  python publish.py 3 -c "修正 WiFi 問題"  # 發布 hoRelay3 並附更新說明
  python publish.py 1 -y                   # 發布 hoRelay1 並跳過確認
"""

import os
import sys
import subprocess
import json
import re
import hashlib
import argparse
from pathlib import Path
from datetime import datetime, timezone
import shutil
import urllib.request
import urllib.parse
import urllib.error
import platform

# ── 每個型號的硬體設定 ──────────────────────────────────────────────

MODEL_CONFIGS = {
    1: {
        'dir': 'ho_relay1',
        'ino': 'ho_relay1.ino',
        'fqbn': 'esp32:esp32:esp32',
        'label': 'hoRelay1 (ESP32 WROOM)',
        # 沒有 variants 的型號，.ino 的 deviceModel 必須等於這個值才准發版
        # （見 validate_device_model()）
        'expected_model': 'hoRelay1',
    },
    2: {
        'dir': 'ho_relay2',
        'ino': 'ho_relay2.ino',
        'fqbn': 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash=all,FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default',
        'label': 'hoRelay2/hoRelay2-1 (ESP32-C3 MOSFET)',
        # 同一份 .ino 編譯出多個硬體變體（不同出廠年份 GPIO 不同）
        'variants': [
            {'model': 'hoRelay2',   'relay_pin': 4},  # 舊版
            {'model': 'hoRelay2-1', 'relay_pin': 7},  # 新版
        ],
    },
    3: {
        'dir': 'ho_relay3',
        'ino': 'ho_relay3.ino',
        'fqbn': 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash=all,FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default',
        'label': 'hoRelay v3.0 齁斑自製電路板',
        # ⚠ ho_relay3.ino 目前把 deviceModel 誤寫成 "hoRelay2"，照舊流程會把 hoRelay3 的
        #   韌體發到 hoRelay2 的 Firestore 文件與 HoLuCam 後台，讓 hoRelay2 設備刷到錯的韌體。
        #   所以這裡寫明預期型號，對不上就在編譯前中止（先修 .ino 才能發）。
        'expected_model': 'hoRelay3',
    },
}

# ── 終端顏色 ────────────────────────────────────────────────────────

class Colors:
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    CYAN = '\033[0;36m'
    WHITE = '\033[0;37m'
    GRAY = '\033[0;90m'
    NC = '\033[0m'

def print_color(message, color=Colors.WHITE):
    try:
        print(f"{color}{message}{Colors.NC}")
    except UnicodeEncodeError:
        safe_message = message.encode('ascii', errors='replace').decode('ascii')
        print(f"{color}{safe_message}{Colors.NC}")

def print_header(title):
    print_color(f"\n{'='*50}", Colors.CYAN)
    print_color(f"  {title}", Colors.CYAN)
    print_color(f"{'='*50}\n", Colors.CYAN)

# ── 工具檢查 ────────────────────────────────────────────────────────

def get_arduino_cli_path():
    if platform.system() == 'Windows':
        common_paths = [
            r'C:\Program Files\Arduino CLI\arduino-cli.exe',
            r'C:\Program Files (x86)\Arduino CLI\arduino-cli.exe',
            os.path.expandvars(r'%LOCALAPPDATA%\Arduino15\arduino-cli.exe'),
            os.path.expandvars(r'%USERPROFILE%\AppData\Local\Programs\Arduino CLI\arduino-cli.exe'),
        ]
        for path in common_paths:
            if os.path.exists(path):
                return path
    return 'arduino-cli'

def check_command(command):
    if command == 'arduino-cli':
        path = get_arduino_cli_path()
        if platform.system() == 'Windows' and path.endswith('.exe'):
            return os.path.exists(path)
    if command == 'gh' and platform.system() == 'Windows':
        gh_path = r'C:\Program Files\GitHub CLI\gh.exe'
        if os.path.exists(gh_path):
            return True
    return shutil.which(command) is not None

def check_requirements():
    print_header("檢查必要工具")
    requirements = {
        'arduino-cli': 'https://arduino.github.io/arduino-cli/',
        'firebase': 'npm install -g firebase-tools'
    }
    all_installed = True
    for cmd, install_guide in requirements.items():
        if check_command(cmd):
            print_color(f"✓ {cmd} 已安裝", Colors.GREEN)
        else:
            print_color(f"❌ 未安裝 {cmd}", Colors.RED)
            print_color(f"   安裝方式: {install_guide}", Colors.YELLOW)
            all_installed = False
    if check_command('gsutil'):
        print_color("✓ gsutil 已安裝 (推薦)", Colors.GREEN)
    else:
        print_color("⚠ 未安裝 gsutil (可選，但推薦安裝以自動上傳)", Colors.YELLOW)
    return all_installed

# ── 型號檢查 ────────────────────────────────────────────────────────

def validate_device_model(relay, parsed_model, configs=None):
    """檢查從 .ino 讀到的 deviceModel 能不能拿來發版。回傳錯誤說明，沒問題回 None。

    發版會以 model 當 Firestore 文件 ID 與 HoLuCam 後台的型號鍵，型號寫錯＝把韌體
    推給另一種硬體（例如 hoRelay3 的韌體蓋掉 hoRelay2 的發佈資料），設備刷了可能開不起來。
    所以只要有一點對不上就擋下來，而且要在「遞增版號、編譯、上傳」之前擋。
    有 variants 的型號由 variants 決定 model，不走這個檢查。
    """
    configs = configs or MODEL_CONFIGS
    cfg = configs[relay]
    if cfg.get('variants'):
        return None
    expected = cfg.get('expected_model')
    if not expected:
        return f"MODEL_CONFIGS[{relay}] 沒有設定 expected_model，無法確認型號，請先補上"
    if parsed_model != expected:
        return (f"{cfg['ino']} 的 deviceModel 是 \"{parsed_model}\"，"
                f"但型號 {relay} 預期是 \"{expected}\"")
    # 與其他型號的預期型號／variants 撞名（防 MODEL_CONFIGS 本身設錯）
    for other_relay, other_cfg in configs.items():
        if other_relay == relay:
            continue
        others = [other_cfg.get('expected_model')]
        others += [v['model'] for v in (other_cfg.get('variants') or [])]
        if parsed_model in others:
            return (f"deviceModel \"{parsed_model}\" 與型號 {other_relay}"
                    f"（{other_cfg['label']}）的型號撞名")
    return None

# ── 版本管理 ────────────────────────────────────────────────────────

def increment_version(version):
    try:
        parts = version.split('.')
        if len(parts) >= 1:
            parts[-1] = str(int(parts[-1]) + 1)
            return '.'.join(parts)
        return version
    except Exception as e:
        print_color(f"⚠ 版本號遞增失敗: {e}", Colors.YELLOW)
        return version

def update_firmware_version(project_dir, ino_file, new_version):
    ino_path = os.path.join(project_dir, ino_file)
    try:
        with open(ino_path, 'r', encoding='utf-8') as f:
            content = f.read()
        new_content = re.sub(
            r'const char\* firmwareVersion = "[^"]+"',
            f'const char* firmwareVersion = "{new_version}"',
            content
        )
        with open(ino_path, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print_color(f"✓ 版本號已更新為: {new_version}", Colors.GREEN)
        return True
    except Exception as e:
        print_color(f"❌ 更新版本號失敗: {e}", Colors.RED)
        return False

def get_firmware_info(project_dir, ino_file, cfg=None):
    print_header("讀取韌體資訊")
    ino_path = os.path.join(project_dir, ino_file)
    if not os.path.exists(ino_path):
        print_color(f"❌ 找不到 {ino_path}", Colors.RED)
        return None
    try:
        with open(ino_path, 'r', encoding='utf-8') as f:
            content = f.read()
        version_match = re.search(r'const char\* firmwareVersion = "([^"]+)"', content)
        if not version_match:
            print_color("❌ 無法從 .ino 檔案讀取版本號", Colors.RED)
            return None
        version = version_match.group(1)

        # 如果有多變體設定，model 由變體定義；否則從 .ino 讀取
        variants = cfg.get('variants') if cfg else None
        if variants:
            model = variants[0]['model']  # 主要型號
            print_color(f"韌體版本: {version}", Colors.WHITE)
            print_color(f"硬體變體: {len(variants)} 個", Colors.WHITE)
            for v in variants:
                print_color(f"  - {v['model']} (GPIO {v['relay_pin']})", Colors.GRAY)
        else:
            model_match = re.search(r'const char\* deviceModel = "([^"]+)"', content)
            if not model_match:
                print_color("❌ 無法從 .ino 檔案讀取設備型號", Colors.RED)
                return None
            model = model_match.group(1)
            print_color(f"設備型號: {model}", Colors.WHITE)
            print_color(f"韌體版本: {version}", Colors.WHITE)

        return {'version': version, 'model': model}
    except Exception as e:
        print_color(f"❌ 讀取檔案失敗: {e}", Colors.RED)
        return None

# ── 編譯韌體 ────────────────────────────────────────────────────────

def build_firmware(project_dir, fqbn, model, variant=None):
    print_header(f"編譯韌體: {model}")
    build_path = os.path.join(project_dir, 'build', model) if variant else os.path.join(project_dir, 'build')

    # 清空並重建 build 目錄，避免殘留舊的 .bin 檔案
    if os.path.exists(build_path):
        shutil.rmtree(build_path)
    os.makedirs(build_path)

    print_color(f"FQBN: {fqbn}", Colors.GRAY)

    # 組合編譯指令
    cli = get_arduino_cli_path()
    cmd = [cli, 'compile', '--fqbn', fqbn, '--output-dir', build_path]

    # 如果有變體定義，透過編譯旗標指定 GPIO（型號由 .ino 根據 RELAY_PIN 自動決定）
    if variant:
        extra_flag = f'-DRELAY_PIN={variant["relay_pin"]}'
        cmd += [
            '--build-property', f'compiler.cpp.extra_flags={extra_flag}',
            '--build-property', f'compiler.c.extra_flags={extra_flag}',
        ]
        print_color(f"編譯旗標: {extra_flag}", Colors.GRAY)

    cmd.append(project_dir)
    print_color("正在編譯...", Colors.YELLOW)

    try:
        res = subprocess.run(cmd, encoding='utf-8', errors='ignore')
        if res.returncode != 0:
            print_color(f"❌ {model} 編譯失敗", Colors.RED)
            return None

        bin_file = pick_app_image(build_path)
        if not bin_file:
            return None
        file_size = bin_file.stat().st_size / 1024
        print_color(f"✓ {model} 編譯成功: {bin_file.name}", Colors.GREEN)
        print_color(f"檔案大小: {file_size:.2f} KB", Colors.WHITE)
        return str(bin_file)
    except Exception as e:
        print_color(f"❌ {model} 編譯過程出錯: {e}", Colors.RED)
        return None

def pick_app_image(build_path):
    """從 arduino-cli 的輸出目錄挑出「主程式映像」（{sketch}.ino.bin）。

    輸出目錄還會有 .ino.bootloader.bin、.ino.partitions.bin、.ino.merged.bin，
    OTA 只能用主程式映像；以前 glob('*.bin')[0] 是任取一個，挑到別的會刷壞設備。
    找不到或不只一個就回 None（呼叫端中止）。
    """
    candidates = sorted(Path(build_path).glob('*.ino.bin'))
    if len(candidates) != 1:
        found = ', '.join(p.name for p in Path(build_path).glob('*.bin')) or '（無）'
        if not candidates:
            print_color("❌ 找不到主程式映像 *.ino.bin，中止", Colors.RED)
        else:
            print_color("❌ 主程式映像 *.ino.bin 不只一個，無法判斷要用哪個，中止", Colors.RED)
        print_color(f"   輸出目錄的 .bin：{found}", Colors.GRAY)
        return None
    return candidates[0]

# ── 上傳韌體 ────────────────────────────────────────────────────────

def get_firebase_project_id():
    firebase_config_path = os.path.join('..', 'hoctrl', '.firebaserc')
    try:
        with open(firebase_config_path, 'r') as f:
            config = json.load(f)
            return config['projects']['default']
    except Exception as e:
        print_color(f"⚠ 無法讀取 Firebase 專案 ID: {e}", Colors.YELLOW)
        return None

def release_bin_path(project_dir, model, version):
    """這次發版實際上傳的檔案路徑；main() 也用它算 MD5，確保兩邊是同一個檔。"""
    return os.path.join(project_dir, 'build', f"{model}_v{version}.bin")


def upload_to_firebase(bin_path, project_dir, model, version, changelog="更新"):
    print_header("上傳韌體")

    file_name = f"{model}_v{version}.bin"
    storage_path = f"firmware/{model}/{file_name}"

    print_color(f"上傳檔案: {file_name}", Colors.YELLOW)

    # 一律把這次新編譯的產物複製成 {model}_v{version}.bin（覆蓋同名舊檔），之後每一種
    # 上傳方式都用這份。以前同名檔已存在就直接上傳舊檔，會跟 main() 以新產物算出的
    # MD5 對不上，設備下載後比對失敗（或更糟：上傳到錯的韌體）。
    renamed_file = release_bin_path(project_dir, model, version)
    build_dir = os.path.dirname(renamed_file)
    try:
        os.makedirs(build_dir, exist_ok=True)
        if os.path.abspath(bin_path) != os.path.abspath(renamed_file):
            shutil.copyfile(bin_path, renamed_file)
        bin_path = renamed_file
    except Exception as e:
        print_color(f"❌ 無法準備上傳檔 {renamed_file}：{e}", Colors.RED)
        return None

    # 方法1: 使用 GitHub Releases (優先)
    if check_command('gh'):
        try:
            print_color("使用 GitHub Releases 上傳...", Colors.YELLOW)
            gh_cmd = r'C:\Program Files\GitHub CLI\gh.exe' if platform.system() == 'Windows' else 'gh'
            repo = os.getenv('GITHUB_REPO', 'maotou316/hoctrl-firmware')
            tag_name = f"v{version}"

            print_color(f"Repository: {repo}", Colors.GRAY)
            print_color(f"Tag: {tag_name}", Colors.GRAY)

            check_cmd = [gh_cmd, 'release', 'view', tag_name, '--repo', repo]
            check_res = subprocess.run(check_cmd, capture_output=True, text=True)

            if check_res.returncode == 0:
                print_color(f"Release {tag_name} 已存在，上傳檔案...", Colors.GRAY)
                upload_cmd = [
                    gh_cmd, 'release', 'upload', tag_name,
                    bin_path, '--clobber', '--repo', repo
                ]
            else:
                print_color(f"建立新 Release {tag_name}...", Colors.GRAY)
                upload_cmd = [
                    gh_cmd, 'release', 'create', tag_name,
                    bin_path,
                    '--title', f"{model} v{version}",
                    '--notes', f"韌體版本 {version}\n\n{changelog}",
                    '--repo', repo
                ]

            res = subprocess.run(upload_cmd, capture_output=True, text=True)
            if res.returncode == 0:
                uploaded_file_name = f"{model}_v{version}.bin"
                download_url = f"https://github.com/{repo}/releases/download/{tag_name}/{uploaded_file_name}"
                print_color("✓ 上傳成功", Colors.GREEN)
                print_color(f"下載 URL: {download_url}", Colors.WHITE)
                return download_url
            else:
                print_color(f"GitHub 上傳失敗: {res.stderr}", Colors.RED)
        except Exception as e:
            print_color(f"GitHub Releases 上傳失敗: {e}", Colors.YELLOW)

    # 方法2: 使用 Python 直接上傳到 Firebase Storage
    try:
        from google.cloud import storage
        from google.oauth2 import service_account

        service_account_path = _find_service_account_key(project_dir)

        if service_account_path:
            print_color(f"使用 Service Account: {service_account_path}", Colors.GRAY)
            credentials = service_account.Credentials.from_service_account_file(
                service_account_path
            )
            storage_client = storage.Client(credentials=credentials, project='hoctrl')
        else:
            print_color("嘗試使用預設認證...", Colors.GRAY)
            storage_client = storage.Client(project='hoctrl')

        bucket_name = 'hoctrl.firebasestorage.app'
        try:
            bucket = storage_client.get_bucket(bucket_name)
            print_color(f"使用現有 bucket: {bucket_name}", Colors.GRAY)
        except Exception:
            print_color(f"Bucket {bucket_name} 不存在，正在建立...", Colors.YELLOW)
            try:
                bucket = storage_client.create_bucket(bucket_name, location='asia-east1')
                print_color("✓ Bucket 建立成功", Colors.GREEN)
            except Exception as create_error:
                print_color(f"建立 bucket 失敗: {create_error}", Colors.YELLOW)
                bucket_name = 'hoctrl.appspot.com'
                print_color(f"嘗試使用預設 bucket: {bucket_name}", Colors.GRAY)
                try:
                    bucket = storage_client.get_bucket(bucket_name)
                except Exception:
                    bucket = storage_client.create_bucket(bucket_name, location='asia-east1')
                    print_color("✓ 預設 bucket 建立成功", Colors.GREEN)

        print_color("正在上傳...", Colors.YELLOW)
        blob = bucket.blob(storage_path)
        blob.upload_from_filename(bin_path)
        blob.make_public()
        download_url = blob.public_url
        print_color("✓ 上傳成功", Colors.GREEN)
        print_color(f"下載 URL: {download_url}", Colors.WHITE)
        return download_url

    except ImportError:
        print_color("⚠ 未安裝 google-cloud-storage", Colors.YELLOW)
    except Exception as e:
        print_color(f"⚠ Firebase Storage 上傳失敗: {e}", Colors.YELLOW)
        if os.getenv('DEBUG'):
            import traceback
            traceback.print_exc()

    # 方法3: 使用 gsutil
    if check_command('gsutil'):
        bucket = "gs://hoctrl.firebasestorage.app"
        try:
            print_color("使用 gsutil 上傳...", Colors.YELLOW)
            subprocess.run(
                ['gsutil', 'cp', bin_path, f"{bucket}/{storage_path}"],
                check=True
            )
            subprocess.run(
                ['gsutil', 'acl', 'ch', '-u', 'AllUsers:R', f"{bucket}/{storage_path}"],
                check=True
            )
            download_url = f"https://storage.googleapis.com/hoctrl.firebasestorage.app/{storage_path}"
            print_color("✓ 上傳成功", Colors.GREEN)
            print_color(f"下載 URL: {download_url}", Colors.WHITE)
            return download_url
        except subprocess.CalledProcessError as e:
            print_color(f"❌ 上傳失敗: {e}", Colors.RED)

    # 方法4: 手動上傳提示
    print_color("\n⚠ 自動上傳失敗，請手動上傳", Colors.YELLOW)
    print_color(f"\n檔案位置: {os.path.abspath(bin_path)}", Colors.CYAN)
    print_color(f"Firebase Storage 路徑: {storage_path}", Colors.CYAN)
    print_color("\n手動上傳步驟:", Colors.WHITE)
    print_color("1. 開啟 Firebase Console: https://console.firebase.google.com/project/hoctrl/storage", Colors.WHITE)
    print_color(f"2. 上傳檔案到: {storage_path}", Colors.WHITE)
    print_color("3. 點擊檔案，取得下載 URL", Colors.WHITE)
    print_color("4. 在下方輸入 URL\n", Colors.WHITE)

    default_url = f"https://storage.googleapis.com/hoctrl.firebasestorage.app/{storage_path}"
    print_color(f"預期的 URL (如果已手動上傳): {default_url}", Colors.GRAY)

    download_url = input("\n請輸入下載 URL (或直接按 Enter 使用預期的 URL): ").strip()
    return download_url if download_url else default_url

# ── Firestore 更新 ──────────────────────────────────────────────────

# ★ 為什麼同一份資料要寫進「兩個」Firebase 專案 ★
#
# 控制器硬體與韌體只有一批（hoRelay2／hoRelay2-1…），但目前有兩個 App 各自提供
# 韌體更新功能，而且各自讀「自己 Firebase 專案」的 firmware_updates/{型號}：
#   • hoCtrl （齁控）      → Firebase 專案 hoctrl
#   • HoLuCam（獵捕監控）  → Firebase 專案 holucam-be6c6（僅舊版 App）
#
# ⚠ HoLuCam 已改以「後台 MySQL FirmwareRelease」為準（新版 App 與網頁後台都讀後台，
#   由下方 register_holucam_backend() 登記）。holucam-be6c6 這份 Firestore 只為了
#   還沒升級的舊版 HoLuCam App 保留；等舊版 App 都淘汰後才能從清單拿掉，在那之前不要刪。
#
# 兩邊欄位是同一套：version / md5 / min_version / download_url / changelog /
# publish_time（另外的 publisher / updater / update_time 是 App 韌體管理頁寫的
# 使用者 uid，發版腳本不寫，也不跨專案複製——別的專案對不到那個 uid）。
#
# 以前這支腳本只寫 hoctrl，holucam-be6c6 那份靠人工複製，結果就是停在被複製過去的
# 那一版（2026-09 時停在 1.8.5），HoLuCam 的使用者從此看不到新韌體。
# 所以發版時要對清單裡「每一個」專案各寫一次同一份資料。
#
# ⚠ 這不是複製貼上的重複程式碼，請不要「順手」刪掉其中一個專案。
# ⚠ 日後要再加第三個 App，只要在這個清單裡加一筆。
FIRESTORE_TARGET_PROJECTS = [
    # hoctrl：維持原有憑證路徑（serviceAccountKey.json → 預設認證 ADC → Node 腳本），
    #         這條路跑很久了，不要動它的行為。
    {'project_id': 'hoctrl', 'label': 'hoCtrl（齁控）', 'legacy_path': True},
    # holucam-be6c6：這台發版機沒有它的 service account key，也沒裝 gcloud（所以連
    #         ADC 都沒有），因此走「firebase CLI 已登入的使用者憑證 + Firestore REST
    #         API」。理由見 _firebase_cli_access_token() 與 firestore_rest_set_merge()。
    #         ⚠ 只為舊版 HoLuCam App 保留；新版 App 讀後台（見 register_holucam_backend）。
    {'project_id': 'holucam-be6c6', 'label': 'HoLuCam（獵捕監控，舊版 App）', 'legacy_path': False},
]

# firebase CLI 把登入後的 refresh token 存在這裡（firebase-tools 的 configstore）。
FIREBASE_TOOLS_CONFIG_PATH = os.path.join(
    os.path.expanduser('~'), '.config', 'configstore', 'firebase-tools.json'
)
# firebase-tools 自己的 installed-app OAuth client。這組值**公開寫在 firebase-tools
# 的原始碼裡**（installed app 沒有真正的機密），所以可以直接放在這裡；它只是用來把
# 上面那個 refresh token 換成 access token，換不到任何額外權限。
FIREBASE_CLI_CLIENT_ID = '563584335869-fgrhgmd47bqnekij5i8b5pr03ho849e6.apps.googleusercontent.com'
FIREBASE_CLI_CLIENT_SECRET = 'j9iVZfS8kkCEFUPaAeJV0sAi'

FIRESTORE_COLLECTION = 'firmware_updates'

# 同一次發版會寫多個型號 × 多個專案，access token 取一次就夠。
_FIREBASE_CLI_TOKEN_CACHE = {'token': None}


def _find_service_account_key(project_dir):
    """在多個位置搜尋 serviceAccountKey.json（hoctrl 專用，路徑不要改）"""
    candidates = [
        os.path.join(project_dir, 'serviceAccountKey.json'),
        os.path.join('..', 'hoctrl', 'serviceAccountKey.json'),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def _find_service_account_key_for_project(project_id, project_dir):
    """找某個專案「專屬」的 service account key（hoctrl 以外的專案用）。

    目前這台機器上沒有 holucam-be6c6 的 key，所以一定回 None、接著走 CLI 憑證；
    但日後只要把 key 放到下面任一個位置（或設環境變數），就會自動優先採用，
    不必再改程式。
    """
    env_name = 'FIRESTORE_SA_KEY_' + project_id.upper().replace('-', '_')
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.environ.get(env_name),
        os.path.join(script_dir, f'serviceAccountKey.{project_id}.json'),
        os.path.join(project_dir, f'serviceAccountKey.{project_id}.json'),
        os.path.join('..', project_id, 'serviceAccountKey.json'),
    ]
    for path in candidates:
        if path and os.path.exists(path):
            return path
    return None


def compute_md5(bin_path):
    """算出韌體 .bin 的 MD5，寫進 Firestore 供設備下載後比對。

    設備下載韌體走的是 client.setInsecure()（不驗 TLS 憑證），HTTPS 在那條路上
    只提供加密、不提供來源鑑別；而 Update.end() 在沒有設定 MD5 時只認映像檔開頭的
    0xE9 magic byte，內容壞掉照樣會被接受並切換 otadata → 設備開不起來。
    hoRelay2 的 MOS gate 沒有下拉電阻，開不起來就等於繼電器恆閉合，
    所以這個欄位不是可選的（見 .claude/rules/ota-md5-required.md）。
    """
    h = hashlib.md5()
    with open(bin_path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


# ── firebase CLI 使用者憑證 + Firestore REST API ─────────────────────
#
# 為什麼不用 service account？因為這台發版機上就是沒有 holucam-be6c6 的 key，
# 也沒裝 gcloud（所以連 ADC 都沒有）。唯一現成、而且已經被授權的憑證，就是
# 「firebase CLI 登入的那個使用者」。
#
# ⚠ holucam-be6c6 的 firestore.rules 對 firmware_updates 是 allow write: if false，
#   但那只約束 client SDK。這裡走的是 Cloud Firestore REST API + 使用者 OAuth token，
#   判權靠 Cloud API／IAM，**不受 Firestore 安全規則限制**（已實測寫入成功）。
#   所以不必、也不要為了發版去放寬那個專案的安全規則。

def _firebase_cli_access_token(force_refresh=False):
    """用 firebase CLI 存下來的 refresh token 換一個 access token。

    回傳 access token 字串；失敗直接 raise，讓呼叫端把原因印出來。
    """
    if not force_refresh and _FIREBASE_CLI_TOKEN_CACHE['token']:
        return _FIREBASE_CLI_TOKEN_CACHE['token']

    if not os.path.exists(FIREBASE_TOOLS_CONFIG_PATH):
        raise RuntimeError(
            f"找不到 firebase CLI 設定檔 {FIREBASE_TOOLS_CONFIG_PATH}，請先執行 firebase login"
        )
    with open(FIREBASE_TOOLS_CONFIG_PATH, 'r', encoding='utf-8') as f:
        cfg = json.load(f)
    refresh_token = (cfg.get('tokens') or {}).get('refresh_token')
    if not refresh_token:
        raise RuntimeError("firebase CLI 設定檔裡沒有 refresh_token，請重新執行 firebase login")

    body = urllib.parse.urlencode({
        'grant_type': 'refresh_token',
        'refresh_token': refresh_token,
        'client_id': FIREBASE_CLI_CLIENT_ID,
        'client_secret': FIREBASE_CLI_CLIENT_SECRET,
    }).encode('utf-8')
    req = urllib.request.Request(
        'https://oauth2.googleapis.com/token',
        data=body,
        headers={'Content-Type': 'application/x-www-form-urlencoded'},
        method='POST',
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as res:
            payload = json.loads(res.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        detail = e.read().decode('utf-8', errors='replace')
        raise RuntimeError(f"以 firebase CLI 憑證換 access token 失敗（HTTP {e.code}）：{detail}")

    token = payload.get('access_token')
    if not token:
        raise RuntimeError("OAuth 回應裡沒有 access_token")
    _FIREBASE_CLI_TOKEN_CACHE['token'] = token
    return token


def _rfc3339_utc(dt=None):
    """產生 Firestore REST 要的 RFC3339 UTC 時間字串（毫秒精度）。

    REST API 沒有 firestore.SERVER_TIMESTAMP 這種東西，所以 publish_time 用
    「發版當下的 UTC 時間」。差別只有網路往返那幾百毫秒，對韌體更新判斷沒有影響。
    """
    dt = dt or datetime.now(timezone.utc)
    dt = dt.astimezone(timezone.utc)
    return dt.strftime('%Y-%m-%dT%H:%M:%S.') + f"{dt.microsecond // 1000:03d}Z"


def _firestore_rest_value(value):
    """把 Python 值包成 Firestore REST 的 typed value。"""
    if isinstance(value, datetime):
        return {'timestampValue': _rfc3339_utc(value)}
    return {'stringValue': str(value)}


def _firestore_rest_doc_url(project_id, collection, doc_id=None):
    base = (f"https://firestore.googleapis.com/v1/projects/{urllib.parse.quote(project_id)}"
            f"/databases/(default)/documents/{urllib.parse.quote(collection)}")
    if doc_id is None:
        return base
    return f"{base}/{urllib.parse.quote(doc_id, safe='')}"


def _firestore_rest_call(url, access_token, method='GET', payload=None):
    """打一次 Firestore REST API，回傳 (HTTP 狀態碼, 解析後的 JSON 或原始字串)。"""
    data = json.dumps(payload).encode('utf-8') if payload is not None else None
    headers = {'Authorization': f'Bearer {access_token}'}
    if data is not None:
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as res:
            raw = res.read().decode('utf-8')
            return res.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as e:
        raw = e.read().decode('utf-8', errors='replace')
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, raw


def _firestore_rest_error(body):
    if isinstance(body, dict):
        err = body.get('error') or {}
        msg = err.get('message') or err.get('status')
        if msg:
            return msg
    return str(body)[:500]


def firestore_rest_get(project_id, doc_id=None, collection=FIRESTORE_COLLECTION,
                       access_token=None):
    """【唯讀】用 CLI 憑證讀 firmware_updates（給人工驗證／除錯用）。

    doc_id 給 None 就列整個 collection。發版流程不會呼叫這個函式，它存在的目的是
    讓人可以在「不寫入任何資料」的前提下，確認憑證與 URL 組法是對的。
    回傳 (HTTP 狀態碼, JSON)。
    """
    access_token = access_token or _firebase_cli_access_token()
    return _firestore_rest_call(
        _firestore_rest_doc_url(project_id, collection, doc_id), access_token
    )


def firestore_rest_set_merge(project_id, doc_id, data, collection=FIRESTORE_COLLECTION,
                             access_token=None):
    """用 REST API 做等同 set(merge=True) 的寫入，回傳 (成功?, 說明)。

    PATCH 一定要帶 updateMask.fieldPaths 逐欄指定，否則 REST 的 PATCH 會把整份文件
    換成 request body（＝整份覆蓋），App 自己寫的 publisher / updater / update_time
    就會被抹掉。文件還不存在時 PATCH 也會建立，這裡仍保留 404 → POST
    ?documentId=<型號> 的退路，免得哪天 API 行為改變就寫不進去。
    """
    access_token = access_token or _firebase_cli_access_token()
    fields = {k: _firestore_rest_value(v) for k, v in data.items()}

    mask = '&'.join(
        f"updateMask.fieldPaths={urllib.parse.quote(k, safe='')}" for k in data
    )
    patch_url = f"{_firestore_rest_doc_url(project_id, collection, doc_id)}?{mask}"
    status, body = _firestore_rest_call(patch_url, access_token, 'PATCH', {'fields': fields})
    if 200 <= status < 300:
        return True, 'PATCH 逐欄合併成功'
    if status != 404:
        return False, f"PATCH 失敗（HTTP {status}）：{_firestore_rest_error(body)}"

    # 文件不存在 → 建立
    create_url = (f"{_firestore_rest_doc_url(project_id, collection)}"
                  f"?documentId={urllib.parse.quote(doc_id, safe='')}")
    status, body = _firestore_rest_call(create_url, access_token, 'POST', {'fields': fields})
    if 200 <= status < 300:
        return True, '文件不存在，已新建'
    return False, f"建立文件失敗（HTTP {status}）：{_firestore_rest_error(body)}"


def firestore_rest_delete(project_id, doc_id, collection=FIRESTORE_COLLECTION,
                          access_token=None):
    """用 REST API 刪除一份文件。只給自我驗證用（刪掉測試文件），發版流程不會呼叫。"""
    access_token = access_token or _firebase_cli_access_token()
    status, body = _firestore_rest_call(
        _firestore_rest_doc_url(project_id, collection, doc_id), access_token, 'DELETE'
    )
    if 200 <= status < 300:
        return True, ''
    return False, f"HTTP {status}：{_firestore_rest_error(body)}"


# ── 各專案的寫入實作 ────────────────────────────────────────────────

def _update_firestore_hoctrl(project_dir, model, version, download_url, changelog,
                             min_version, md5=None):
    """hoctrl 的原有寫入路徑：Python SDK（SA key → 預設認證）→ Node 腳本 → 手動提示。

    ⚠ 這個函式是從舊版 update_firestore() 原封不動搬過來的，行為刻意保持不變。
    """
    # 方法1: 使用 Python Firebase Admin SDK
    try:
        from google.cloud import firestore
        from google.oauth2 import service_account

        service_account_path = _find_service_account_key(project_dir)

        if service_account_path:
            print_color(f"使用 Service Account: {service_account_path}", Colors.GRAY)
            credentials = service_account.Credentials.from_service_account_file(
                service_account_path
            )
            db = firestore.Client(credentials=credentials, project='hoctrl')
        else:
            print_color("嘗試使用預設認證...", Colors.GRAY)
            db = firestore.Client(project='hoctrl')

        print_color("正在更新 Firestore...", Colors.YELLOW)
        update_data = {
            'version': version,
            'download_url': download_url,
            'changelog': changelog,
            'min_version': min_version,
            'publish_time': firestore.SERVER_TIMESTAMP
        }
        if md5:
            update_data['md5'] = md5
        db.collection('firmware_updates').document(model).set(update_data, merge=True)
        print_color("✓ Firestore 更新成功", Colors.GREEN)
        print_color(f"文件路徑: firmware_updates/{model}", Colors.WHITE)
        return True

    except ImportError:
        print_color("⚠ 未安裝 google-cloud-firestore，正在自動安裝...", Colors.YELLOW)
        try:
            subprocess.run(
                [sys.executable, '-m', 'pip', 'install', 'google-cloud-firestore'],
                check=True
            )
            print_color("✓ 安裝成功，重新嘗試更新 Firestore...", Colors.GREEN)
            return _update_firestore_hoctrl(project_dir, model, version, download_url,
                                            changelog, min_version, md5)
        except subprocess.CalledProcessError:
            print_color("❌ 自動安裝 google-cloud-firestore 失敗", Colors.RED)
    except Exception as e:
        print_color(f"⚠ Python 更新 Firestore 失敗: {e}", Colors.YELLOW)

    # 方法2: 使用 Node.js 腳本
    flutter_dir = Path("../hoctrl")
    md5_line = ("," + chr(10) + "  md5: '" + md5 + "'") if md5 else ""
    node_script = f"""
const admin = require('firebase-admin');
const serviceAccount = require('./serviceAccountKey.json');

admin.initializeApp({{
  credential: admin.credential.cert(serviceAccount)
}});

const db = admin.firestore();
const updateData = {{
  version: '{version}',
  download_url: '{download_url}',
  changelog: `{changelog}`,
  min_version: '{min_version}',
  publish_time: admin.firestore.Timestamp.now(){md5_line}
}};

db.collection('firmware_updates')
  .doc('{model}')
  .set(updateData, {{ merge: true }})
  .then(() => {{
    console.log('✓ Firestore 更新成功');
    process.exit(0);
  }})
  .catch((error) => {{
    console.error('❌ Firestore 更新失敗:', error);
    process.exit(1);
  }});
"""
    script_path = flutter_dir / "temp_firestore_update.js"

    try:
        if not check_command('node'):
            raise FileNotFoundError("未安裝 Node.js")
        with open(script_path, 'w', encoding='utf-8') as f:
            f.write(node_script)
        print_color("使用 Node.js 更新 Firestore...", Colors.YELLOW)
        res = subprocess.run(
            ['node', 'temp_firestore_update.js'],
            cwd=str(flutter_dir),
            capture_output=True,
            text=True
        )
        if res.returncode == 0:
            print_color("✓ Firestore 更新成功", Colors.GREEN)
            return True
        else:
            print_color("❌ Node.js 更新失敗", Colors.RED)
            print_color(res.stderr, Colors.RED)
    except Exception as e:
        print_color(f"❌ Node.js 更新失敗: {e}", Colors.RED)
    finally:
        if script_path.exists():
            script_path.unlink()

    # 方法3: 手動更新提示
    print_color("\n⚠ 自動更新 Firestore 失敗，請手動更新", Colors.YELLOW)
    print_color("\n手動更新步驟:", Colors.WHITE)
    print_color("1. 開啟 Firebase Console: https://console.firebase.google.com/project/hoctrl/firestore", Colors.WHITE)
    print_color(f"2. 進入集合: firmware_updates", Colors.WHITE)
    print_color(f"3. 編輯或新增文件 ID: {model}", Colors.WHITE)
    print_color("4. 設定以下欄位:", Colors.WHITE)
    print_color(f"   - version: {version}", Colors.GRAY)
    print_color(f"   - download_url: {download_url}", Colors.GRAY)
    print_color(f"   - changelog: {changelog}", Colors.GRAY)
    print_color(f"   - min_version: {min_version}", Colors.GRAY)
    print_color(f"   - publish_time: (使用 Timestamp.now())", Colors.GRAY)

    return False


def _update_firestore_with_service_account(project_id, key_path, model, version,
                                           download_url, changelog, min_version, md5=None):
    """用某個專案「專屬」的 service account key 寫入（未來放了 key 就會走這條）。"""
    from google.cloud import firestore
    from google.oauth2 import service_account

    credentials = service_account.Credentials.from_service_account_file(key_path)
    db = firestore.Client(credentials=credentials, project=project_id)
    update_data = {
        'version': version,
        'download_url': download_url,
        'changelog': changelog,
        'min_version': min_version,
        'publish_time': firestore.SERVER_TIMESTAMP,
    }
    if md5:
        update_data['md5'] = md5
    db.collection(FIRESTORE_COLLECTION).document(model).set(update_data, merge=True)


def _update_firestore_secondary(project_id, project_dir, model, version, download_url,
                                changelog, min_version, md5=None):
    """hoctrl 以外的專案：依序嘗試「專屬 SA key → firebase CLI 憑證 + REST」。

    兩條都失敗才算這個專案失敗，並把每一條的失敗原因都留在回傳值裡。
    回傳 (成功?, 失敗原因)。
    """
    failures = []

    # 嘗試1：專屬 service account key（優先，因為它不依賴某個人的 CLI 登入狀態）
    key_path = _find_service_account_key_for_project(project_id, project_dir)
    if key_path:
        print_color(f"使用 Service Account: {key_path}", Colors.GRAY)
        try:
            _update_firestore_with_service_account(
                project_id, key_path, model, version, download_url,
                changelog, min_version, md5
            )
            print_color(f"✓ {project_id} 更新成功（service account）", Colors.GREEN)
            print_color(f"文件路徑: {FIRESTORE_COLLECTION}/{model}", Colors.WHITE)
            return True, ''
        except Exception as e:
            failures.append(f"service account（{key_path}）失敗：{e}")
            print_color(f"⚠ service account 寫入失敗，改試 firebase CLI 憑證：{e}", Colors.YELLOW)
    else:
        print_color(f"找不到 {project_id} 專屬的 service account key，改用 firebase CLI 憑證",
                    Colors.GRAY)

    # 嘗試2：firebase CLI 已登入的使用者憑證 + Firestore REST API
    try:
        access_token = _firebase_cli_access_token()
    except Exception as e:
        failures.append(f"取 firebase CLI access token 失敗：{e}")
        print_color(f"❌ 取 firebase CLI access token 失敗：{e}", Colors.RED)
        return False, '；'.join(failures)

    update_data = {
        'version': version,
        'download_url': download_url,
        'changelog': changelog,
        'min_version': min_version,
        # REST 沒有 SERVER_TIMESTAMP，用發版當下的 UTC 時間
        'publish_time': datetime.now(timezone.utc),
    }
    if md5:
        update_data['md5'] = md5

    print_color("使用 firebase CLI 使用者憑證打 Firestore REST API...", Colors.YELLOW)
    try:
        ok, detail = firestore_rest_set_merge(project_id, model, update_data,
                                              access_token=access_token)
    except Exception as e:
        failures.append(f"REST 寫入發生例外：{e}")
        print_color(f"❌ REST 寫入發生例外：{e}", Colors.RED)
        return False, '；'.join(failures)

    if ok:
        print_color(f"✓ {project_id} 更新成功（REST／{detail}）", Colors.GREEN)
        print_color(f"文件路徑: {FIRESTORE_COLLECTION}/{model}", Colors.WHITE)
        return True, ''

    failures.append(f"REST 寫入失敗：{detail}")
    print_color(f"❌ {project_id} REST 寫入失敗：{detail}", Colors.RED)
    return False, '；'.join(failures)


def update_firestore(project_dir, model, version, download_url, changelog, min_version, md5=None):
    """把同一份韌體發佈資料登記到 FIRESTORE_TARGET_PROJECTS 裡的每一個 Firebase 專案。

    ⚠ 回傳值的定義是「全部成功」：任何一個專案失敗就回 False。
      絕對不要改成「有一個成功就算成功」——那會讓發版的人以為兩個 App 都拿到新韌體，
      實際上其中一個 App 的使用者永遠停在舊版，而且沒有人會發現。
    """
    targets = FIRESTORE_TARGET_PROJECTS
    print_header(f"更新 Firestore 記錄（{len(targets)} 個專案）")
    print_color(f"文件: {FIRESTORE_COLLECTION}/{model}   版本: {version}", Colors.WHITE)

    results = []
    for target in targets:
        project_id = target['project_id']
        label = target['label']
        print_color(f"\n── {label} / {project_id} " + "─" * 18, Colors.CYAN)
        try:
            if target.get('legacy_path'):
                ok = _update_firestore_hoctrl(project_dir, model, version, download_url,
                                              changelog, min_version, md5)
                reason = '' if ok else 'Python SDK 與 Node.js 都失敗（原因見上方訊息）'
            else:
                ok, reason = _update_firestore_secondary(
                    project_id, project_dir, model, version, download_url,
                    changelog, min_version, md5
                )
        except Exception as e:
            ok, reason = False, f"未預期的例外：{e}"
            print_color(f"❌ {project_id} 更新失敗: {e}", Colors.RED)
            if os.getenv('DEBUG'):
                import traceback
                traceback.print_exc()
        results.append({'project_id': project_id, 'label': label, 'ok': ok, 'reason': reason})

    ok_count = sum(1 for r in results if r['ok'])
    print_color("")
    if ok_count == len(results):
        print_color(f"✓ Firestore 登記全部成功（{ok_count}/{len(results)} 個專案）", Colors.GREEN)
        for r in results:
            print_color(f"   ✓ {r['label']} / {r['project_id']}", Colors.GREEN)
        return True

    print_color(f"❌ Firestore 登記未全部成功（{ok_count}/{len(results)} 個專案）", Colors.RED)
    for r in results:
        if r['ok']:
            print_color(f"   ✓ {r['label']} / {r['project_id']}", Colors.GREEN)
        else:
            print_color(f"   ❌ {r['label']} / {r['project_id']} 失敗：{r['reason']}", Colors.RED)
    print_color(f"⚠ 標 ❌ 的 App 使用者「看不到」{model} v{version}，"
                "請手動補登記後再宣布發版完成", Colors.YELLOW)
    return False

# ── HoLuCam 後台登記 ────────────────────────────────────────────────
#
# HoLuCam App 與網頁後台已改讀後台 MySQL 的 FirmwareRelease（以後台為準），
# Firestore holucam-be6c6 那份只為「還沒升級的舊版 App」保留。所以發版時除了寫
# Firestore，還要把同一份資料 PUT 到後台：
#   PUT {HOLUCAM_API_BASE}/api/firmware-publish/releases/{model}
#   Authorization: Bearer {HOLUCAM_FIRMWARE_PUBLISH_TOKEN}
# body 與後台 admin 版 PUT /api/admin/firmware/releases/:model 完全相同
# （version／downloadUrl／md5／minVersion／changelog，駝峰命名，與 Firestore 的底線命名不同）。
# 後台會自己再驗 version（x.y.z）、md5（32 hex）、網址（https、可列印 ASCII、長度塞得進設備的
# JSON 緩衝區）；這裡不重複驗，驗證失敗時把後台回的 code／message 原樣印出來。
#
# 沒設 token → 黃色警告並略過（不讓發版失敗）；有設但 PUT 失敗 → 紅色錯誤、結尾 exit 1。

HOLUCAM_API_BASE_DEFAULT = 'https://holucam.neuter.online'

# 走到後台登記時，韌體已上傳、Firestore 已寫、.ino 版號也已 +1，重跑只會再發一個新版號
HOLUCAM_MANUAL_REGISTER_HINT = ("不要重跑 publish.py（會再跳一個版號），"
                                "請到 HoLuCam 後台「韌體管理」頁手動登記")


def _holucam_api_base():
    return (os.getenv('HOLUCAM_API_BASE') or HOLUCAM_API_BASE_DEFAULT).strip().rstrip('/')


def _holucam_publish_token():
    return (os.getenv('HOLUCAM_FIRMWARE_PUBLISH_TOKEN') or '').strip()


# 只有本機測試可以用 http；其他一律要 https，否則 token 會以明文送出去。
_HOLUCAM_HTTP_ALLOWED_HOSTS = ('localhost', '127.0.0.1')


def validate_holucam_api_base(api_base):
    """檢查 HOLUCAM_API_BASE。回傳錯誤說明，沒問題回 None（純函式，不打網路）。"""
    try:
        parsed = urllib.parse.urlsplit(api_base)
        host = parsed.hostname
        has_userinfo = bool(parsed.username or parsed.password)
    except ValueError:
        return "HOLUCAM_API_BASE 不是合法網址"
    scheme = (parsed.scheme or '').lower()
    if not host:
        return "HOLUCAM_API_BASE 不是合法網址（缺主機名稱）"
    if has_userinfo:
        return "HOLUCAM_API_BASE 不可含帳號密碼"
    if scheme == 'https':
        return None
    if scheme == 'http' and host.lower() in _HOLUCAM_HTTP_ALLOWED_HOSTS:
        return None
    return ("HOLUCAM_API_BASE 必須是 https://（只有 http://localhost、http://127.0.0.1 "
            "可用 http），為避免 token 明文外洩，不送出")


def validate_holucam_token(token):
    """檢查 token 能不能放進 HTTP header。回傳錯誤說明（絕不含 token 本身），沒問題回 None。

    含 CR/LF 等控制字元的 token 放進 header 會被 urllib 拒絕，而那個例外訊息會把整個
    header 值（含 token）印出來；所以先擋，也順便擋 header injection。
    """
    if any(ord(ch) < 0x20 or ord(ch) == 0x7f for ch in token):
        return "HOLUCAM_FIRMWARE_PUBLISH_TOKEN 含換行或其他控制字元，請重新設定（不印出 token 內容）"
    if any(ord(ch) > 0x7e for ch in token):
        return "HOLUCAM_FIRMWARE_PUBLISH_TOKEN 含非 ASCII 字元，請重新設定（不印出 token 內容）"
    return None


def build_holucam_release_request(api_base, model, version, download_url, changelog,
                                  min_version, md5):
    """組出 PUT 的網址與 body（純函式，不打網路，方便單獨驗證）。"""
    url = (f"{api_base.rstrip('/')}/api/firmware-publish/releases/"
           f"{urllib.parse.quote(model, safe='')}")
    body = {
        'version': version,
        'downloadUrl': download_url,
        # 後台會轉小寫，這裡先轉，讓印出來的內容與存進 DB 的一致
        'md5': (md5 or '').lower(),
        'minVersion': min_version or None,
        'changelog': changelog or None,
    }
    return url, body


def _holucam_error_detail(status, body):
    """從後台錯誤回應抽出 (code, message)。

    兩種形狀都要吃：
      - 直接的 { ok:false, error:{ code, message } }
      - h3 createError：{ statusCode, statusMessage, message, data:{ ok:false, error:{ code, message } } }
    """
    if isinstance(body, dict):
        err = body.get('error')
        if not isinstance(err, dict):
            data = body.get('data')
            err = data.get('error') if isinstance(data, dict) else None
        if isinstance(err, dict) and (err.get('code') or err.get('message')):
            return err.get('code') or f'HTTP {status}', err.get('message') or ''
        return (body.get('statusMessage') or f'HTTP {status}',
                body.get('message') or str(body)[:500])
    return f'HTTP {status}', str(body)[:500]


def register_holucam_backend(model, version, download_url, changelog, min_version, md5):
    """把一個型號的發佈資料登記到 HoLuCam 後台。

    回傳 (狀態, 說明)，狀態為 'ok'／'skipped'／'failed'。
    'skipped' 只在沒設 token 時出現，不算發版失敗。
    """
    print_header(f"登記 HoLuCam 後台：{model}")
    token = _holucam_publish_token()
    if not token:
        print_color("⚠ 未設定 HOLUCAM_FIRMWARE_PUBLISH_TOKEN，略過 HoLuCam 後台登記"
                    "（新版 HoLuCam App 會看不到這一版，請之後到後台韌體頁手動登記）", Colors.YELLOW)
        return 'skipped', '未設定 HOLUCAM_FIRMWARE_PUBLISH_TOKEN'

    token_error = validate_holucam_token(token)
    if token_error:
        print_color(f"❌ HoLuCam 後台登記失敗（{model}）：{token_error}", Colors.RED)
        return 'failed', token_error

    api_base = _holucam_api_base()
    base_error = validate_holucam_api_base(api_base)
    if base_error:
        print_color(f"❌ HoLuCam 後台登記失敗（{model}）：{base_error}", Colors.RED)
        return 'failed', base_error

    try:
        url, body = build_holucam_release_request(
            api_base, model, version, download_url, changelog, min_version, md5
        )
        print_color(f"PUT {url}", Colors.GRAY)
        req = urllib.request.Request(
            url,
            data=json.dumps(body).encode('utf-8'),
            headers={'Authorization': f'Bearer {token}', 'Content-Type': 'application/json'},
            method='PUT',
        )
        with urllib.request.urlopen(req, timeout=60) as res:
            status = res.status
            raw = res.read().decode('utf-8', errors='replace')
            try:
                parsed = json.loads(raw) if raw else {}
            except ValueError:
                parsed = raw
    except urllib.error.HTTPError as e:
        # 讀錯誤內容本身也可能失敗（連線中斷等），不可讓它中斷整輪（後面還有別的變體）
        try:
            raw = e.read().decode('utf-8', errors='replace')
        except Exception as read_err:
            raw = f"（讀取錯誤回應失敗：{type(read_err).__name__}）"
        try:
            parsed = json.loads(raw)
        except ValueError:
            parsed = raw
        code, message = _holucam_error_detail(e.code, parsed)
        detail = f"HTTP {e.code} {code}：{message}"
        print_color(f"❌ HoLuCam 後台登記失敗（{model}）：{detail}", Colors.RED)
        if e.code == 503:
            print_color("   後台的韌體發版端點未啟用（伺服器沒設定發版 token）", Colors.YELLOW)
        elif e.code == 401:
            print_color("   HOLUCAM_FIRMWARE_PUBLISH_TOKEN 與後台設定不一致", Colors.YELLOW)
        return 'failed', detail
    except Exception as e:
        # 只印例外類別名稱：部分例外（例如 header 驗證失敗）的訊息會帶出 Authorization
        # header 的值，也就是 token。URLError 另外補上 reason 的類別名稱（同樣不印內容）。
        reason = ''
        if isinstance(e, urllib.error.URLError):
            reason = f"（{type(e.reason).__name__}）"
        detail = f"連線失敗：{type(e).__name__}{reason}"
        print_color(f"❌ HoLuCam 後台登記失敗（{model}）：{detail}", Colors.RED)
        return 'failed', detail

    if isinstance(parsed, dict) and parsed.get('ok') is True:
        print_color(f"✓ HoLuCam 後台登記成功：{model} v{version}", Colors.GREEN)
        return 'ok', ''
    code, message = _holucam_error_detail(status, parsed)
    detail = f"HTTP {status} 回應非預期：{code} {message}"
    print_color(f"❌ HoLuCam 後台登記失敗（{model}）：{detail}", Colors.RED)
    return 'failed', detail


def print_holucam_summary(holucam_results):
    """結尾摘要：列出 HoLuCam 後台登記結果。回傳是否有失敗項。"""
    if not holucam_results:
        return False
    print_color("\nHoLuCam 後台登記：", Colors.WHITE)
    has_failure = False
    for r in holucam_results:
        if r['status'] == 'ok':
            print_color(f"  ✓ {r['model']}", Colors.GREEN)
        elif r['status'] == 'skipped':
            print_color(f"  ⚠ {r['model']} 已略過（{r['reason']}）", Colors.YELLOW)
        else:
            has_failure = True
            print_color(f"  ❌ {r['model']} 失敗：{r['reason']}", Colors.RED)
    if has_failure:
        print_color("⚠ 新版 HoLuCam App 看不到標 ❌ 的型號新版。" + HOLUCAM_MANUAL_REGISTER_HINT,
                    Colors.YELLOW)
    return has_failure

# ── 主程式 ──────────────────────────────────────────────────────────

def select_relay():
    """互動式選擇型號"""
    print_color("\n╔════════════════════════════════════════╗", Colors.CYAN)
    print_color("║   hoRelay 韌體發布自動化腳本           ║", Colors.CYAN)
    print_color("╚════════════════════════════════════════╝\n", Colors.CYAN)
    print_color("請選擇要發布的型號:\n", Colors.WHITE)
    for num, cfg in MODEL_CONFIGS.items():
        print_color(f"  {num}) {cfg['label']}", Colors.WHITE)
    print_color("", Colors.NC)
    while True:
        try:
            choice = input("請輸入型號編號 (1-3): ").strip()
            num = int(choice)
            if num in MODEL_CONFIGS:
                return num
            print_color("⚠ 請輸入 1、2 或 3", Colors.YELLOW)
        except ValueError:
            print_color("⚠ 請輸入數字", Colors.YELLOW)
        except EOFError:
            sys.exit(0)

def main():
    parser = argparse.ArgumentParser(
        description='hoRelay 韌體發布自動化腳本（支援 hoRelay1～3）',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='範例:\n'
               '  python publish.py\n'
               '  python publish.py 2\n'
               '  python publish.py 3 -c "修正 WiFi 連接問題"\n'
               '  python publish.py 1 -y -m 1.0.0\n'
    )
    parser.add_argument('relay', type=int, choices=[1, 2, 3], nargs='?', default=None,
                        help='繼電器型號 (1=hoRelay1, 2=hoRelay2, 3=hoRelay3)')
    parser.add_argument('-c', '--changelog', help='更新說明')
    parser.add_argument('-m', '--min-version', default='1.3.5', help='最低版本要求')
    parser.add_argument('-y', '--yes', action='store_true', help='跳過確認直接發布')
    args = parser.parse_args()

    relay = args.relay if args.relay is not None else select_relay()
    cfg = MODEL_CONFIGS[relay]

    # 以腳本所在目錄為基準計算 project_dir
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.join(script_dir, cfg['dir'])

    if not os.path.isdir(project_dir):
        print_color(f"❌ 找不到目錄: {project_dir}", Colors.RED)
        sys.exit(1)

    print_color(f"\n╔════════════════════════════════════════╗", Colors.CYAN)
    print_color(f"║   {cfg['label']:^36s} ║", Colors.CYAN)
    print_color(f"║   韌體發布自動化腳本                  ║", Colors.CYAN)
    print_color(f"╚════════════════════════════════════════╝\n", Colors.CYAN)

    # 檢查必要工具
    if not check_requirements():
        print_color("\n❌ 缺少必要工具，無法繼續", Colors.RED)
        sys.exit(1)

    # 讀取韌體資訊
    firmware_info = get_firmware_info(project_dir, cfg['ino'], cfg)
    if not firmware_info:
        print_color("\n❌ 無法讀取韌體資訊", Colors.RED)
        sys.exit(1)

    version = firmware_info['version']
    model = firmware_info['model']

    # 型號檢查一定要在「遞增版號（會改 .ino）、編譯、上傳、寫 Firestore、登記後台」之前
    model_error = validate_device_model(relay, model)
    if model_error:
        print_color(f"\n❌ 型號不符，中止發版：{model_error}", Colors.RED)
        print_color("   沒有改版號、沒有編譯、沒有上傳、沒有寫 Firestore、也沒有登記 HoLuCam 後台；"
                    "請先修正 .ino 的 deviceModel 再發版", Colors.RED)
        sys.exit(1)

    # 自動將版本號加 1
    print_header("遞增版本號")
    old_version = version
    version = increment_version(version)
    print_color(f"舊版本: {old_version}", Colors.GRAY)
    print_color(f"新版本: {version}", Colors.GREEN)

    if not update_firmware_version(project_dir, cfg['ino'], version):
        print_color("\n❌ 無法更新版本號", Colors.RED)
        sys.exit(1)

    # 確認更新資訊
    print_header("確認更新資訊")
    print_color(f"韌體版本: {version}", Colors.WHITE)
    print_color(f"最低版本: {args.min_version}", Colors.WHITE)

    changelog = args.changelog
    if not changelog:
        changelog = "修正錯誤，優化效能"
        print_color(f"使用預設更新說明: {changelog}", Colors.GRAY)

    print_color(f"\n更新說明:\n{changelog}", Colors.WHITE)

    # 判斷是否有多變體
    variants = cfg.get('variants')
    holucam_results = []  # 每個型號的 HoLuCam 後台登記結果，結尾摘要與 exit code 用

    if variants:
        # 多變體：逐一編譯、上傳、更新
        results = []
        for variant in variants:
            vmodel = variant['model']
            print_color(f"\n{'─'*50}", Colors.CYAN)
            print_color(f"  處理變體: {vmodel} (GPIO {variant['relay_pin']})", Colors.CYAN)
            print_color(f"{'─'*50}", Colors.CYAN)

            bin_path = build_firmware(project_dir, cfg['fqbn'], vmodel, variant)
            if not bin_path:
                print_color(f"\n❌ {vmodel} 編譯失敗，中止發布", Colors.RED)
                sys.exit(1)

            download_url = upload_to_firebase(bin_path, project_dir, vmodel, version, changelog)
            if not download_url:
                print_color(f"\n❌ {vmodel} 上傳失敗，中止發布", Colors.RED)
                sys.exit(1)
                continue

            # 對「實際上傳的那個檔」算 MD5
            md5 = compute_md5(release_bin_path(project_dir, vmodel, version))
            print_color(f"MD5: {md5}", Colors.GRAY)

            # ↓ Firestore 發佈登記要實際反映到摘要上：只要有任何一個 Firebase 專案沒寫進去，
            #   這個變體就不算發版成功（舊版寫死 success=True，寫失敗也看不出來）。
            firestore_ok = update_firestore(project_dir, vmodel, version, download_url, changelog, args.min_version, md5)
            # ↓ HoLuCam 後台登記（新版 HoLuCam App 以後台為準）；沒設 token 只警告略過，不算失敗
            hl_status, hl_reason = register_holucam_backend(
                vmodel, version, download_url, changelog, args.min_version, md5)
            holucam_results.append({'model': vmodel, 'status': hl_status, 'reason': hl_reason})
            results.append({'model': vmodel,
                            'success': firestore_ok and hl_status != 'failed',
                            'url': download_url})

        # 摘要
        success_count = sum(1 for r in results if r['success'])
        total_count = len(results)

        if success_count == total_count:
            print_color(f"\n╔════════════════════════════════════════╗", Colors.GREEN)
            print_color(f"║   ✓ 所有韌體發布完成！({success_count}/{total_count})       ║", Colors.GREEN)
            print_color(f"╚════════════════════════════════════════╝", Colors.GREEN)
        else:
            print_color(f"\n╔════════════════════════════════════════╗", Colors.YELLOW)
            print_color(f"║   ⚠ 部分韌體發布完成 ({success_count}/{total_count})         ║", Colors.YELLOW)
            print_color(f"╚════════════════════════════════════════╝", Colors.YELLOW)

        print_color(f"\n版本: {version}", Colors.WHITE)
        for r in results:
            status = "✓" if r['success'] else "❌"
            url_info = f" - {r['url']}" if r.get('url') else ""
            print_color(f"  {status} {r['model']}{url_info}", Colors.GREEN if r['success'] else Colors.RED)
        print_holucam_summary(holucam_results)
        print_color("\n設備將在下次連線時收到更新通知\n", Colors.YELLOW)

    else:
        # 單一型號：原有流程
        print_color(f"設備型號: {model}", Colors.WHITE)

        bin_path = build_firmware(project_dir, cfg['fqbn'], model)
        if not bin_path:
            print_color("\n❌ 編譯失敗，無法繼續", Colors.RED)
            sys.exit(1)

        download_url = upload_to_firebase(bin_path, project_dir, model, version, changelog)
        if not download_url:
            print_color("\n❌ 上傳失敗，無法繼續", Colors.RED)
            sys.exit(1)

        # 對「實際上傳的那個檔」算 MD5
        md5 = compute_md5(release_bin_path(project_dir, model, version))
        print_color(f"MD5: {md5}", Colors.GRAY)

        firestore_ok = update_firestore(project_dir, model, version, download_url, changelog, args.min_version, md5)
        hl_status, hl_reason = register_holucam_backend(
            model, version, download_url, changelog, args.min_version, md5)
        holucam_results.append({'model': model, 'status': hl_status, 'reason': hl_reason})

        # 與多變體流程一致：Firestore 與 HoLuCam 後台都沒失敗才印綠色完成
        hl_failed = hl_status == 'failed'
        if firestore_ok and not hl_failed:
            print_color("\n╔════════════════════════════════════════╗", Colors.GREEN)
            print_color("║        ✓ 韌體發布完成！              ║", Colors.GREEN)
            print_color("╚════════════════════════════════════════╝", Colors.GREEN)
            print_color(f"\n版本: {version}", Colors.WHITE)
            print_color(f"下載 URL: {download_url}", Colors.WHITE)
            print_color("\n設備將在下次連線時收到更新通知\n", Colors.YELLOW)
        elif firestore_ok:
            print_color("\n╔════════════════════════════════════════╗", Colors.YELLOW)
            print_color("║  ⚠ 韌體已上傳，但 HoLuCam 後台未登記 ║", Colors.YELLOW)
            print_color("╚════════════════════════════════════════╝", Colors.YELLOW)
            print_color(f"\n版本: {version}", Colors.WHITE)
            print_color(f"下載 URL: {download_url}", Colors.WHITE)
            print_color("\nhoCtrl 會收到更新通知；新版 HoLuCam App 要等後台登記後才看得到\n",
                        Colors.YELLOW)
        else:
            print_color("\n╔════════════════════════════════════════╗", Colors.YELLOW)
            print_color("║    ⚠ 韌體已上傳，但 Firestore 未更新 ║", Colors.YELLOW)
            print_color("╚════════════════════════════════════════╝", Colors.YELLOW)
            print_color(f"\n版本: {version}", Colors.WHITE)
            print_color(f"下載 URL: {download_url}", Colors.WHITE)
            print_color("\n請手動更新 Firestore 後，設備才會收到更新通知\n", Colors.YELLOW)
        print_holucam_summary(holucam_results)

    # HoLuCam 後台有設 token 卻登記失敗 → exit 1，讓發版的人（或 CI）一定注意到。
    # 沒設 token 的「略過」不算失敗。
    if any(r['status'] == 'failed' for r in holucam_results):
        print_color("\n❌ HoLuCam 後台登記有失敗項（見上方摘要），結束代碼 1", Colors.RED)
        print_color(f"   {HOLUCAM_MANUAL_REGISTER_HINT}", Colors.RED)
        sys.exit(1)

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print_color("\n\n已取消", Colors.YELLOW)
        sys.exit(0)
    except Exception as e:
        print_color(f"\n❌ 發生錯誤: {e}", Colors.RED)
        import traceback
        traceback.print_exc()
        sys.exit(1)
