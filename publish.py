#!/usr/bin/env python3
"""
hoRelay 韌體發布自動化腳本（支援 hoRelay1～3）
此腳本會編譯韌體、上傳到 GitHub Releases / Firebase Storage 並更新 Firestore 記錄

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
import argparse
from pathlib import Path
from datetime import datetime
import shutil
import platform

# ── 每個型號的硬體設定 ──────────────────────────────────────────────

MODEL_CONFIGS = {
    1: {
        'dir': 'ho_relay1',
        'ino': 'ho_relay1.ino',
        'fqbn': 'esp32:esp32:esp32',
        'label': 'hoRelay1 (ESP32 WROOM)',
    },
    2: {
        'dir': 'ho_relay2',
        'ino': 'ho_relay2.ino',
        'fqbn': 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash=all,FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default',
        'label': 'hoRelay2 (ESP32-C3)',
    },
    3: {
        'dir': 'ho_relay3',
        'ino': 'ho_relay3.ino',
        'fqbn': 'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash=all,FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default',
        'label': 'hoRelay3 (ESP32-C3)',
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

def get_firmware_info(project_dir, ino_file):
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

def build_firmware(project_dir, fqbn, model):
    print_header("開始編譯韌體")
    build_path = os.path.join(project_dir, 'build')

    # 清空並重建 build 目錄，避免殘留舊的 .bin 檔案
    if os.path.exists(build_path):
        shutil.rmtree(build_path)
    os.makedirs(build_path)

    print_color(f"FQBN: {fqbn}", Colors.GRAY)
    print_color("正在編譯...", Colors.YELLOW)

    try:
        cli = get_arduino_cli_path()
        res = subprocess.run(
            [cli, 'compile', '--fqbn', fqbn, '--output-dir', build_path, project_dir],
            encoding='utf-8',
            errors='ignore'
        )
        if res.returncode != 0:
            print_color("❌ 編譯失敗", Colors.RED)
            return None

        bin_files = list(Path(build_path).glob('*.bin'))
        if not bin_files:
            print_color("❌ 找不到編譯後的 .bin 檔案", Colors.RED)
            return None

        bin_file = bin_files[0]
        file_size = bin_file.stat().st_size / 1024
        print_color(f"✓ 編譯成功: {bin_file.name}", Colors.GREEN)
        print_color(f"檔案大小: {file_size:.2f} KB", Colors.WHITE)
        return str(bin_file)
    except Exception as e:
        print_color(f"❌ 編譯過程出錯: {e}", Colors.RED)
        return None

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

def upload_to_firebase(bin_path, project_dir, model, version, changelog="更新"):
    print_header("上傳韌體")

    file_name = f"{model}_v{version}.bin"
    storage_path = f"firmware/{model}/{file_name}"

    print_color(f"上傳檔案: {file_name}", Colors.YELLOW)

    # 方法1: 使用 GitHub Releases (優先)
    if check_command('gh'):
        try:
            print_color("使用 GitHub Releases 上傳...", Colors.YELLOW)
            gh_cmd = r'C:\Program Files\GitHub CLI\gh.exe' if platform.system() == 'Windows' else 'gh'
            repo = os.getenv('GITHUB_REPO', 'maotou316/hoctrl-firmware')
            tag_name = f"v{version}"

            build_dir = os.path.join(project_dir, 'build')
            renamed_file = os.path.join(build_dir, f'{model}_v{version}.bin')
            if bin_path != renamed_file and not os.path.exists(renamed_file):
                shutil.copy(bin_path, renamed_file)
                bin_path = renamed_file
            elif os.path.exists(renamed_file):
                bin_path = renamed_file

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

def _find_service_account_key(project_dir):
    """在多個位置搜尋 serviceAccountKey.json"""
    candidates = [
        os.path.join(project_dir, 'serviceAccountKey.json'),
        os.path.join('..', 'hoctrl', 'serviceAccountKey.json'),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None

def update_firestore(project_dir, model, version, download_url, changelog, min_version):
    print_header("更新 Firestore 記錄")

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
            return update_firestore(project_dir, model, version, download_url, changelog, min_version)
        except subprocess.CalledProcessError:
            print_color("❌ 自動安裝 google-cloud-firestore 失敗", Colors.RED)
    except Exception as e:
        print_color(f"⚠ Python 更新 Firestore 失敗: {e}", Colors.YELLOW)

    # 方法2: 使用 Node.js 腳本
    flutter_dir = Path("../hoctrl")
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
  publish_time: admin.firestore.Timestamp.now()
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

# ── 主程式 ──────────────────────────────────────────────────────────

def select_relay():
    """互動式選擇型號"""
    print_color("\n╔════════════════════════════════════════╗", Colors.CYAN)
    print_color("║   hoRelay 韌體發布自動化腳本          ║", Colors.CYAN)
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
    parser.add_argument('-m', '--min-version', default='1.0.0', help='最低版本要求')
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
    firmware_info = get_firmware_info(project_dir, cfg['ino'])
    if not firmware_info:
        print_color("\n❌ 無法讀取韌體資訊", Colors.RED)
        sys.exit(1)

    version = firmware_info['version']
    model = firmware_info['model']

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
    print_color(f"設備型號: {model}", Colors.WHITE)
    print_color(f"韌體版本: {version}", Colors.WHITE)
    print_color(f"最低版本: {args.min_version}", Colors.WHITE)

    changelog = args.changelog
    if not changelog:
        changelog = "修正錯誤，優化效能"
        print_color(f"使用預設更新說明: {changelog}", Colors.GRAY)

    print_color(f"\n更新說明:\n{changelog}", Colors.WHITE)

    # 編譯韌體
    bin_path = build_firmware(project_dir, cfg['fqbn'], model)
    if not bin_path:
        print_color("\n❌ 編譯失敗，無法繼續", Colors.RED)
        sys.exit(1)

    # 上傳到 Firebase 或 GitHub
    download_url = upload_to_firebase(bin_path, project_dir, model, version, changelog)
    if not download_url:
        print_color("\n❌ 上傳失敗，無法繼續", Colors.RED)
        sys.exit(1)

    # 更新 Firestore
    firestore_ok = update_firestore(project_dir, model, version, download_url, changelog, args.min_version)

    # 完成
    if firestore_ok:
        print_color("\n╔════════════════════════════════════════╗", Colors.GREEN)
        print_color("║        ✓ 韌體發布完成！              ║", Colors.GREEN)
        print_color("╚════════════════════════════════════════╝", Colors.GREEN)
        print_color(f"\n版本: {version}", Colors.WHITE)
        print_color(f"下載 URL: {download_url}", Colors.WHITE)
        print_color("\n設備將在下次連線時收到更新通知\n", Colors.YELLOW)
    else:
        print_color("\n╔════════════════════════════════════════╗", Colors.YELLOW)
        print_color("║    ⚠ 韌體已上傳，但 Firestore 未更新 ║", Colors.YELLOW)
        print_color("╚════════════════════════════════════════╝", Colors.YELLOW)
        print_color(f"\n版本: {version}", Colors.WHITE)
        print_color(f"下載 URL: {download_url}", Colors.WHITE)
        print_color("\n請手動更新 Firestore 後，設備才會收到更新通知\n", Colors.YELLOW)

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
