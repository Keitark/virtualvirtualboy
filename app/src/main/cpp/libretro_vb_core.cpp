#include "libretro_vb_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "log.h"

extern "C" {
#include "libretro.h"
}

namespace {

// Libretro core entry points from beetle-vb.
extern "C" {
void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample(retro_audio_sample_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_init(void);
void retro_deinit(void);
bool retro_load_game(const struct retro_game_info* info);
void retro_unload_game(void);
void retro_run(void);
}

LibretroVbCore* gCore = nullptr;
retro_pixel_format gPixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;

void LogMessage(enum retro_log_level level, const char* fmt, ...) {
    (void)level;
    va_list args;
    va_start(args, fmt);
    char buffer[1024] = {};
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    LOGI("[beetle-vb] %s", buffer);
}

bool EnvironmentCallback(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            auto* log = static_cast<retro_log_callback*>(data);
            log->log = LogMessage;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            gPixelFormat = *static_cast<retro_pixel_format*>(data);
            return gPixelFormat == RETRO_PIXEL_FORMAT_XRGB8888;
        }
        case RETRO_ENVIRONMENT_GET_OVERSCAN: {
            auto* overscan = static_cast<bool*>(data);
            *overscan = false;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE:
            if (data == nullptr) {
                return false;
            }
            {
                auto* var = static_cast<retro_variable*>(data);
                if (var->key == nullptr) {
                    return false;
                }
                if (std::strcmp(var->key, "vb_3dmode") == 0) {
                    var->value = "side-by-side";
                    return true;
                }
                if (std::strcmp(var->key, "vb_cpu_emulation") == 0) {
                    var->value = "fast";
                    return true;
                }
            }
            return false;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            auto* changed = static_cast<bool*>(data);
            *changed = false;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        default:
            return false;
    }
}

void VideoRefreshCallback(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (gCore == nullptr || data == nullptr || width == 0 || height == 0) {
        return;
    }
    gCore->onVideoFrame(data, width, height, pitch);
}

void AudioSampleCallback(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

size_t AudioSampleBatchCallback(const int16_t* data, size_t frames) {
    (void)data;
    return frames;
}

void InputPollCallback() {}

int16_t InputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (gCore == nullptr || port != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        return gCore->inputMask();
    }
    return (gCore->inputMask() & (1u << id)) ? 1 : 0;
}

}  // namespace

bool LibretroVbCore::initialize() {
    if (initialized_) {
        return true;
    }

    gCore = this;
    retro_set_environment(EnvironmentCallback);
    retro_set_video_refresh(VideoRefreshCallback);
    retro_set_audio_sample(AudioSampleCallback);
    retro_set_audio_sample_batch(AudioSampleBatchCallback);
    retro_set_input_poll(InputPollCallback);
    retro_set_input_state(InputStateCallback);
    retro_init();

    initialized_ = true;
    return true;
}

void LibretroVbCore::setError(const std::string& error) {
    lastError_ = error;
    LOGE("%s", lastError_.c_str());
}

bool LibretroVbCore::loadRomFromFile(const std::string& path) {
    if (!initialized_) {
        setError("libretro core not initialized");
        return false;
    }

    unloadRom();
    frameReady_ = false;

    std::ifstream rom(path, std::ios::binary);
    if (!rom.good()) {
        setError("ROM file not found: " + path);
        return false;
    }

    rom.seekg(0, std::ios::end);
    const auto romSize = static_cast<size_t>(rom.tellg());
    rom.seekg(0, std::ios::beg);
    if (romSize == 0) {
        setError("ROM file is empty: " + path);
        return false;
    }

    romData_.resize(romSize);
    rom.read(reinterpret_cast<char*>(romData_.data()), static_cast<std::streamsize>(romData_.size()));
    if (!rom.good() && !rom.eof()) {
        setError("Failed reading ROM data: " + path);
        return false;
    }

    retro_game_info info{};
    info.path = path.c_str();
    info.data = romData_.data();
    info.size = romData_.size();
    info.meta = nullptr;

    if (!retro_load_game(&info)) {
        setError("retro_load_game failed: " + path);
        romData_.clear();
        return false;
    }

    romLoaded_ = true;
    lastError_.clear();
    LOGI("ROM loaded: %s", path.c_str());
    return true;
}

void LibretroVbCore::unloadRom() {
    if (romLoaded_) {
        retro_unload_game();
    }
    romLoaded_ = false;
    frameReady_ = false;
    frameWidth_ = 0;
    frameHeight_ = 0;
    frameBuffer_.clear();
    romData_.clear();
}

unsigned LibretroVbCore::mapInputToBitmask(const VbInputState& inputState) {
    unsigned mask = 0;
    if (inputState.left) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
    }
    if (inputState.right) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    }
    if (inputState.up) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_UP);
    }
    if (inputState.down) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
    }
    if (inputState.a) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_A);
    }
    if (inputState.b) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_B);
    }
    if (inputState.l) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_L);
    }
    if (inputState.r) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_R);
    }
    if (inputState.start) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_START);
    }
    if (inputState.select) {
        mask |= (1u << RETRO_DEVICE_ID_JOYPAD_SELECT);
    }
    return mask;
}

void LibretroVbCore::setInputState(const VbInputState& inputState) {
    inputMask_ = static_cast<uint16_t>(mapInputToBitmask(inputState));
}

void LibretroVbCore::onVideoFrame(const void* data, unsigned width, unsigned height, size_t pitch) {
    const auto* src = static_cast<const uint8_t*>(data);
    frameReady_ = true;
    frameWidth_ = static_cast<int>(width);
    frameHeight_ = static_cast<int>(height);
    frameBuffer_.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

    for (unsigned y = 0; y < height; ++y) {
        auto* dstRow = frameBuffer_.data() + static_cast<size_t>(y) * width;
        const auto* srcRow = reinterpret_cast<const uint32_t*>(src + (y * pitch));
        std::memcpy(dstRow, srcRow, width * sizeof(uint32_t));
    }
}

void LibretroVbCore::runFrame() {
    if (!romLoaded_) {
        return;
    }
    retro_run();
}

void LibretroVbCore::shutdown() {
    unloadRom();
    if (initialized_) {
        retro_deinit();
    }
    initialized_ = false;
    gCore = nullptr;
}
