# virtualvirtualboy

Quest-focused Virtual Boy emulator project (Quest 2+ baseline).

## Current Status
- Native Android app (`NativeActivity`) with NDK/CMake build.
- Real Virtual Boy emulation core integrated: `libretro/beetle-vb-libretro` (Mednafen VB).
- OpenXR stereo renderer integrated (Quest-ready path) with fallback flat GLES renderer.
- Core is forced to `side-by-side` 3D mode and each eye samples its respective half.
- ROM loading:
  - SAF file picker supports arbitrary ROM filenames.
  - Auto-load probes (fallback):
  - `/sdcard/Download/test.vb`
  - `/sdcard/Download/test.vboy`
  - `/sdcard/Download/rom.vb`

## Project Layout
- `app/src/main/java/.../MainActivity.kt`: Android entry activity.
- `app/src/main/cpp/native_app.cpp`: native loop, app lifecycle, input mapping.
- `app/src/main/cpp/libretro_vb_core.*`: libretro bridge and ROM/frame callbacks.
- `app/src/main/cpp/renderer_gl.*`: EGL/GLES renderer.
- `app/src/main/cpp/xr_stereo_renderer.*`: OpenXR session, stereo swapchains, per-eye rendering.
- `third_party/beetle-vb-libretro/`: embedded VB core source (GPL-2.0).

## Build
1. Install Android SDK components:
   - `platforms;android-35`
   - `build-tools;35.0.1`
   - `ndk;26.1.10909125`
   - `cmake;3.22.1`
2. Use JDK 17.
3. Build:

```bash
./gradlew assembleDebug
```

## Run on Quest
1. Enable Developer Mode on Quest.
2. Connect via USB and verify with `adb devices`.
3. Install:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

4. Push a ROM:

```bash
adb push your_game.vb /sdcard/Download/test.vb
```

5. Launch app from Unknown Sources.
6. If no fallback ROM is found, the picker opens and you can choose any `.vb/.vboy` file.

## Input Mapping
- D-pad: VB left D-pad
- Gamepad `A/B`: VB `A/B`
- `L1/R1`: VB `L/R`
- `Start/Select`: VB `Start/Select`

## Next Steps
1. Validate on Quest hardware and tune OpenXR lifecycle edge cases.
2. Add SAF ROM picker and persistent game library.
3. Wire audio output (AAudio/OpenSL ES) and in-app options.
4. Add save states and per-game config.
