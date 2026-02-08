# virtualvirtualboy

Virtual Boy emulator for Meta Quest (Quest 2+), built as a native Android/OpenXR app.

Current milestone: `0.1.0-beta.1`.

## Features
- Beetle VB (`mednafen`) libretro core integrated in-app.
- OpenXR stereo rendering path for Quest, with GLES fallback renderer.
- Red palette output (`black & red`) and side-by-side eye rendering.
- Audio output via `AAudio`.
- ROM loading:
  - Android SAF picker (arbitrary filename support).
  - Fallback scan: `/sdcard/Download/test.vb`, `/sdcard/Download/test.vboy`, `/sdcard/Download/rom.vb`.
- In-app info window and live stereo calibration:
  - Screen size and stereo convergence tuning.
  - Settings persisted across launches.

## Build
Requirements:
- JDK 17
- Android SDK platform 35
- NDK `26.1.10909125`
- CMake `3.22.1`

Build debug APK:
```bash
./gradlew assembleDebug
```

## Install on Quest
```bash
adb devices
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.keitark.virtualvirtualboy/.MainActivity
```

## Controls (Quest)
- `A/B`: VB `A/B`
- `Y`: Start
- `X`: Select
- `L1/R1` (and triggers): VB `L/R`
- Left stick / D-pad: movement
- `R3`: toggle info window
- `L3`: open ROM picker (only when info window is hidden)

Calibration (when info window is shown):
- Hold `L + R`, then:
  - `Up/Down`: screen size
  - `Left/Right`: stereo convergence
  - `A`: reset defaults

## Repository Layout
- `app/src/main/java/.../MainActivity.kt`: Android activity + picker bridge.
- `app/src/main/cpp/native_app.cpp`: native app loop, input mapping, overlay, calibration.
- `app/src/main/cpp/xr_stereo_renderer.*`: OpenXR session, swapchains, XR input.
- `app/src/main/cpp/libretro_vb_core.*`: libretro core bridge.
- `third_party/beetle-vb-libretro/`: embedded core source.

## Legal
- This project is distributed under GPL-2.0 (see `LICENSE`).
- Use only ROMs you legally own. Do not distribute copyrighted ROMs.
