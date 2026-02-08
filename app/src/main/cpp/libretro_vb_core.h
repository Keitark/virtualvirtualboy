#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct VbInputState {
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool a = false;
    bool b = false;
    bool l = false;
    bool r = false;
    bool start = false;
    bool select = false;
};

class LibretroVbCore {
public:
    bool initialize();
    void shutdown();

    bool loadRomFromFile(const std::string& path);
    bool loadRomFromBytes(const uint8_t* data, size_t size, const std::string& nameHint);
    void unloadRom();

    void setInputState(const VbInputState& inputState);
    void runFrame();

    [[nodiscard]] bool isInitialized() const { return initialized_; }
    [[nodiscard]] bool isRomLoaded() const { return romLoaded_; }
    [[nodiscard]] bool hasFrame() const { return frameReady_; }
    [[nodiscard]] int frameWidth() const { return frameWidth_; }
    [[nodiscard]] int frameHeight() const { return frameHeight_; }
    [[nodiscard]] const std::vector<uint32_t>& framePixels() const { return frameBuffer_; }
    [[nodiscard]] std::string lastError() const { return lastError_; }
    [[nodiscard]] uint16_t inputMask() const { return inputMask_; }

    void onVideoFrame(const void* data, unsigned width, unsigned height, size_t pitch);

private:
    static unsigned mapInputToBitmask(const VbInputState& inputState);
    void setError(const std::string& error);

    bool initialized_ = false;
    bool romLoaded_ = false;
    bool frameReady_ = false;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    uint16_t inputMask_ = 0;
    std::string romPathLabel_ = "memory.vb";
    std::vector<uint8_t> romData_;
    std::vector<uint32_t> frameBuffer_;
    std::string lastError_;
};
