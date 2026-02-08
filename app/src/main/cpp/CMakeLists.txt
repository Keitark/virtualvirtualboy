cmake_minimum_required(VERSION 3.22.1)
project(virtualvirtualboy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DEFINED CMAKE_ANDROID_NDK)
    set(ANDROID_NDK_DIR "${CMAKE_ANDROID_NDK}")
elseif(DEFINED ENV{ANDROID_NDK})
    set(ANDROID_NDK_DIR "$ENV{ANDROID_NDK}")
else()
    message(FATAL_ERROR "ANDROID_NDK path is not available.")
endif()

set(NATIVE_APP_GLUE_DIR "${ANDROID_NDK_DIR}/sources/android/native_app_glue")
set(BEETLE_VB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/beetle-vb-libretro")

if(NOT EXISTS "${BEETLE_VB_DIR}/libretro.cpp")
    message(FATAL_ERROR
        "Missing Beetle VB core source at ${BEETLE_VB_DIR}. "
        "Run: git submodule update --init --recursive"
    )
endif()

add_library(
    virtualvirtualboy SHARED
    "${NATIVE_APP_GLUE_DIR}/android_native_app_glue.c"
    native_app.cpp
    audio_player.cpp
    renderer_gl.cpp
    xr_stereo_renderer.cpp
    libretro_vb_core.cpp
    "${BEETLE_VB_DIR}/libretro.cpp"
    "${BEETLE_VB_DIR}/mednafen/hw_cpu/v810/v810_cpu.cpp"
    "${BEETLE_VB_DIR}/mednafen/mempatcher.cpp"
    "${BEETLE_VB_DIR}/mednafen/vb/vsu.c"
    "${BEETLE_VB_DIR}/mednafen/vb/input.c"
    "${BEETLE_VB_DIR}/mednafen/vb/timer.c"
    "${BEETLE_VB_DIR}/mednafen/vb/vip.c"
    "${BEETLE_VB_DIR}/mednafen/hw_cpu/v810/fpu-new/softfloat.c"
    "${BEETLE_VB_DIR}/mednafen/sound/Blip_Buffer.c"
    "${BEETLE_VB_DIR}/mednafen/state.c"
    "${BEETLE_VB_DIR}/mednafen/settings.c"
    "${BEETLE_VB_DIR}/libretro-common/compat/compat_strl.c"
    "${BEETLE_VB_DIR}/libretro-common/compat/compat_snprintf.c"
)
target_include_directories(
    virtualvirtualboy PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${NATIVE_APP_GLUE_DIR}"
    "${BEETLE_VB_DIR}"
    "${BEETLE_VB_DIR}/mednafen"
    "${BEETLE_VB_DIR}/mednafen/include"
    "${BEETLE_VB_DIR}/mednafen/hw_sound"
    "${BEETLE_VB_DIR}/mednafen/hw_cpu"
    "${BEETLE_VB_DIR}/mednafen/hw_misc"
    "${BEETLE_VB_DIR}/libretro-common/include"
)

target_compile_definitions(
    virtualvirtualboy PRIVATE
    WANT_32BPP
    FRONTEND_SUPPORTS_RGB565
    STDC_HEADERS
    __STDC_LIMIT_MACROS
    __LIBRETRO__
    MEDNAFEN_VERSION=\"0.9.31\"
    MEDNAFEN_VERSION_NUMERIC=931
    INLINE=inline
    LSB_FIRST
)

find_library(ANDROID_LIB android)
find_library(LOG_LIB log)
find_library(EGL_LIB EGL)
find_library(GLESV2_LIB GLESv2)
find_library(AAUDIO_LIB aaudio)
find_package(OpenXR REQUIRED CONFIG)

target_link_libraries(
    virtualvirtualboy
    PRIVATE
    ${ANDROID_LIB}
    ${LOG_LIB}
    ${EGL_LIB}
    ${GLESV2_LIB}
    ${AAUDIO_LIB}
    OpenXR::openxr_loader
)
