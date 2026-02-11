# VRboy

[![Status](https://img.shields.io/badge/status-stable-brightgreen?style=for-the-badge)](https://github.com/Keitark/VRboy/releases)
[![Release](https://img.shields.io/github/v/tag/Keitark/VRboy?sort=semver&style=for-the-badge)](https://github.com/Keitark/VRboy/releases)
[![Platform](https://img.shields.io/badge/platform-Android%20(Quest)-3DDC84?style=for-the-badge)](https://developer.android.com/)
[![OpenXR](https://img.shields.io/badge/OpenXR-1.1-0066B8?style=for-the-badge)](https://www.khronos.org/openxr/)
[![License](https://img.shields.io/badge/license-MIT-blue?style=for-the-badge)](LICENSE)
[![Last Commit](https://img.shields.io/github/last-commit/Keitark/VRboy?style=for-the-badge)](https://github.com/Keitark/VRboy/commits/main)

Virtual Boy emulator for Meta Quest (Quest 2+), implemented as a native Android + OpenXR app.

Current milestone: `v1.0.0`

Project links:
- Releases: <https://github.com/Keitark/VRboy/releases>
- Issues: <https://github.com/Keitark/VRboy/issues>
- Discussions/feedback: open an Issue with the `type: feature` label.

---

## English

### Highlights
- Real VB emulation core: Beetle VB (`mednafen`/libretro).
- OpenXR stereo renderer for Quest (with GLES fallback).
- Red palette rendering (`black & red`) + side-by-side stereo path.
- Three view modes: `Anchored` (default, world-fixed), `Classic` (head-locked), and `Depth Layer` (fallback depth presentation + 6DOF walkthrough).
- AAudio output.
- ROM picker (SAF) with arbitrary filenames.
- Runtime calibration (screen size / stereo convergence) with persistence.

### Build Requirements
- JDK 17
- Android SDK Platform 35
- NDK `26.1.10909125`
- CMake `3.22.1`

### First-Time Setup (Core Download)
This repository uses a Git submodule for the Beetle VB core.

```bash
git clone --recurse-submodules https://github.com/Keitark/VRboy.git
cd VRboy
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

No local patching of `beetle-vb-libretro` is required for this build.

### Build Commands
```bash
./gradlew assembleDebug
./gradlew assembleRelease
```

### ROM Reverse Engineering (V810 Disasm)
Use `tools/vb_disasm.py` to inspect ROM code and find VIP writes (BG/OBJ related setup paths).

Disassemble from reset vector:
```bash
python tools/vb_disasm.py disasm "D:\Users\keita\Downloads\VB\extracted\Virtual Boy Wario Land (Japan, USA)\Virtual Boy Wario Land (Japan, USA).vb" --count 800 --output logs/wl_reset_disasm.txt
```

Scan a ROM or folder for VIP write candidates:
```bash
python tools/vb_disasm.py scan-vip "D:\Users\keita\Downloads\VB\extracted" --focus obj-bg --limit 200 --output logs/vip_write_scan.txt
```

Tips:
- If reset output looks mostly non-code, disassemble from a known function address with `--start 0x070xxxxx`.
- `scan-vip` is a static heuristic pass, so verify suspicious results with runtime traces.

### Codex / Claude Setup Prompt
You can paste the following prompt into Codex/Claude to bootstrap this repo quickly.

```text
You are in the `VRboy` repository.
Set up the Android build environment and produce a debug APK.

Requirements:
- JDK 17
- Android SDK Platform 35
- NDK 26.1.10909125
- CMake 3.22.1

Steps:
1. Verify Java version and ensure Gradle uses JDK 17.
2. Run `git submodule update --init --recursive`.
3. Run `./gradlew clean assembleDebug`.
4. Report build result and exact APK path.
5. Confirm APK filename format is `virtualvirtualboy-<version>-debug.apk`.
6. If `adb` is available and Quest is connected, run:
   - `adb install -r app/build/outputs/apk/debug/virtualvirtualboy-1.0.0-debug.apk`
   - `adb shell am start -n com.keitark.virtualvirtualboy/.MainActivity`
7. If any step fails, show the error and propose the minimum fix.
```

### APK Output Naming
APK files are now generated with explicit names:
- `app/build/outputs/apk/debug/virtualvirtualboy-<version>-debug.apk`
- `app/build/outputs/apk/release/virtualvirtualboy-<version>-release.apk`

Example (`v1.0.0`):
- `virtualvirtualboy-1.0.0-debug.apk`

### Install / Run on Quest
```bash
adb devices
adb install -r app/build/outputs/apk/debug/virtualvirtualboy-1.0.0-debug.apk
adb shell am start -n com.keitark.virtualvirtualboy/.MainActivity
```

### How to Add ROMs
Two supported methods:

1. In-app picker (recommended)
- Hide info window (`R3`) if it is open.
- Press `L3` to open Android file picker.
- Select your `.vb` / `.vboy` ROM (any filename is allowed).

2. Fallback auto-load path (`adb push`)
```bash
adb push "Red Alarm (Japan).vb" /sdcard/Download/test.vb
```

The app probes these fallback paths on startup:
- `/sdcard/Download/test.vb`
- `/sdcard/Download/test.vboy`
- `/sdcard/Download/rom.vb`

### Controls (Quest)
| Quest Input | Emulator Action |
| --- | --- |
| `A` | VB `A` |
| `B` | VB `B` |
| `Y` | Start |
| `X` | Select |
| `L1` / `R1` / triggers | VB `L` / `R` |
| Left stick / D-pad | Movement |
| `R3` | Toggle info window |
| `L3` | Open ROM picker (only when info window is hidden) |

Anchored / Depth Layer walkthrough:

| Input | Effect |
| --- | --- |
| `B` (while info window visible) | Cycle `CLASSIC` -> `ANCHORED` -> `DEPTH LAYER` |
| Hold any grip + left stick | Move (strafe/forward/back) |
| Hold any grip + right stick | Look yaw / pitch |
| Hold any grip + `R` / `L` trigger | Move up / down |
| Hold any grip + `A` | Reset walkthrough transform |
| `L3` + `R3` (together) | Reset walkthrough transform + recenter world anchor |

Calibration (while info window is shown):

| Input | Effect |
| --- | --- |
| Hold `L + R` | Enter calibration modifier |
| `Up` / `Down` | Increase / decrease screen size |
| `Left` / `Right` | Adjust stereo convergence (Classic mode only) |
| `A` | Reset calibration to defaults |

### Project Layout
- `app/src/main/java/.../MainActivity.kt`: Android activity + picker bridge.
- `app/src/main/cpp/native_app.cpp`: native loop, lifecycle, input, overlay, calibration.
- `app/src/main/cpp/xr_stereo_renderer.*`: OpenXR stereo renderer + XR input actions.
- `app/src/main/cpp/libretro_vb_core.*`: libretro bridge (video/audio/input).
- `third_party/beetle-vb-libretro/`: Beetle VB Git submodule (download on setup).

### Roadmap
- Save states + per-ROM config.
- CI for debug build and checks.
- Release build hardening and distribution workflow.

---

## 日本語

### 概要
`VRboy` は Meta Quest（Quest 2 以降）向けの Virtual Boy エミュレータです。  
Android ネイティブ + OpenXR で実装しています。

現在のマイルストーン: `v1.0.0`

### 主な機能
- Beetle VB（`mednafen` / libretro）コアを統合。
- Quest 向け OpenXR ステレオ描画（GLES フォールバックあり）。
- 赤色パレット（`black & red`）表示。
- 3つの表示モード: `Anchored`（デフォルト/ワールド固定）、`Classic`（ヘッド固定）、`Depth Layer`（フォールバック深度表示 + 6DOF移動）。
- AAudio による音声出力。
- SAF による ROM ピッカー（任意ファイル名対応）。
- 画面サイズ / 立体収束（convergence）のランタイム調整と保存。

### ビルド要件
- JDK 17
- Android SDK Platform 35
- NDK `26.1.10909125`
- CMake `3.22.1`

### 初回セットアップ（コア取得）
このリポジトリでは Beetle VB コアを Git submodule として管理しています。

```bash
git clone --recurse-submodules https://github.com/Keitark/VRboy.git
cd VRboy
```

submodule なしで clone 済みの場合:

```bash
git submodule update --init --recursive
```

このビルドでは `beetle-vb-libretro` へのローカルパッチ適用は不要です。

### ビルドコマンド
```bash
./gradlew assembleDebug
./gradlew assembleRelease
```

### ROM 解析（V810逆アセンブル）
`tools/vb_disasm.py` で ROM コード逆アセンブルと VIP 書き込み候補（BG/OBJ 系初期化）を確認できます。

リセットベクタから逆アセンブル:
```bash
python tools/vb_disasm.py disasm "D:\Users\keita\Downloads\VB\extracted\Virtual Boy Wario Land (Japan, USA)\Virtual Boy Wario Land (Japan, USA).vb" --count 800 --output logs/wl_reset_disasm.txt
```

ROM 1本またはフォルダ全体の VIP 書き込み候補スキャン:
```bash
python tools/vb_disasm.py scan-vip "D:\Users\keita\Downloads\VB\extracted" --focus obj-bg --limit 200 --output logs/vip_write_scan.txt
```

補足:
- `reset` 付近がコードに見えない場合は `--start 0x070xxxxx` で既知関数先頭から解析してください。
- `scan-vip` は静的ヒューリスティックなので、怪しい箇所は実行時トレースで確認してください。

### Codex / Claude 用セットアッププロンプト
以下を Codex / Claude に貼り付けると、セットアップとビルドを自動実行できます。

```text
You are in the `VRboy` repository.
Set up the Android build environment and produce a debug APK.

Requirements:
- JDK 17
- Android SDK Platform 35
- NDK 26.1.10909125
- CMake 3.22.1

Steps:
1. Verify Java version and ensure Gradle uses JDK 17.
2. Run `git submodule update --init --recursive`.
3. Run `./gradlew clean assembleDebug`.
4. Report build result and exact APK path.
5. Confirm APK filename format is `virtualvirtualboy-<version>-debug.apk`.
6. If `adb` is available and Quest is connected, run:
   - `adb install -r app/build/outputs/apk/debug/virtualvirtualboy-1.0.0-debug.apk`
   - `adb shell am start -n com.keitark.virtualvirtualboy/.MainActivity`
7. If any step fails, show the error and propose the minimum fix.
```

### APK 名称
出力 APK 名を分かりやすくしています:
- `app/build/outputs/apk/debug/virtualvirtualboy-<version>-debug.apk`
- `app/build/outputs/apk/release/virtualvirtualboy-<version>-release.apk`

例（`v1.0.0`）:
- `virtualvirtualboy-1.0.0-debug.apk`

### Quest へのインストール
```bash
adb devices
adb install -r app/build/outputs/apk/debug/virtualvirtualboy-1.0.0-debug.apk
adb shell am start -n com.keitark.virtualvirtualboy/.MainActivity
```

### ROM の入れ方
対応方法は 2 つです。

1. アプリ内ピッカー（推奨）
- 情報ウィンドウが表示中なら `R3` で閉じる
- `L3` で Android のファイルピッカーを開く
- `.vb` / `.vboy` ROM を選択（ファイル名は任意）

2. 自動読込用の固定パスに `adb push`
```bash
adb push "Red Alarm (Japan).vb" /sdcard/Download/test.vb
```

起動時に以下のパスを自動探索します:
- `/sdcard/Download/test.vb`
- `/sdcard/Download/test.vboy`
- `/sdcard/Download/rom.vb`

### 操作（Quest）
| Quest 入力 | 動作 |
| --- | --- |
| `A` | VB `A` |
| `B` | VB `B` |
| `Y` | Start |
| `X` | Select |
| `L1` / `R1` / トリガー | VB `L` / `R` |
| 左スティック / D-pad | 移動 |
| `R3` | 情報ウィンドウ表示切替 |
| `L3` | ROM ピッカー起動（情報ウィンドウ非表示時のみ） |

Anchored / Depth Layer（6DOF移動）:

| 入力 | 効果 |
| --- | --- |
| 情報ウィンドウ表示中 `B` | `CLASSIC` -> `ANCHORED` -> `DEPTH LAYER` の順で切替 |
| いずれかのグリップを押しながら左スティック | 前後左右移動 |
| いずれかのグリップを押しながら右スティック | 視点のYaw/Pitch |
| いずれかのグリップを押しながら `R` / `L` | 上昇 / 下降 |
| いずれかのグリップを押しながら `A` | 移動・回転をリセット |
| `L3` + `R3` を同時押し | 移動・回転リセット + ワールドアンカー再センター |

情報ウィンドウ表示中の調整:

| 入力 | 効果 |
| --- | --- |
| `L + R` を押し続ける | 調整モード |
| `Up` / `Down` | 画面サイズの増減 |
| `Left` / `Right` | 立体収束量の調整（Classicモードのみ） |
| `A` | 初期値へ戻す |

### ディレクトリ構成
- `app/src/main/java/.../MainActivity.kt`: Activity と ROM ピッカー連携。
- `app/src/main/cpp/native_app.cpp`: ネイティブループ、入力、HUD、調整処理。
- `app/src/main/cpp/xr_stereo_renderer.*`: OpenXR 描画と XR 入力。
- `app/src/main/cpp/libretro_vb_core.*`: libretro ブリッジ。
- `third_party/beetle-vb-libretro/`: Beetle VB の Git submodule（セットアップ時に取得）。

---

## Legal
- First-party repository code is distributed under MIT. See `LICENSE`.
- Third-party notices and dependency licenses are documented in `THIRD_PARTY_NOTICES.md`.
- Beetle VB is GPL-2.0. If you distribute binaries linked with it, comply with GPL-2.0 obligations.
- This is an unofficial project and is not affiliated with, endorsed by, or sponsored by Nintendo.
- ROMs are not included. Use only ROM images you legally own.
- Publishing emulator source code is generally lower risk than publishing game content, but legal risk still exists depending on distribution details and jurisdiction.
- Do not upload/distribute commercial ROMs, BIOS files, keys, or copyrighted game assets in this repository.
- If you distribute modified binaries, provide corresponding source and preserve required license notices.
- This is not legal advice.
