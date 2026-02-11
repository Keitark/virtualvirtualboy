#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
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
    [[nodiscard]] bool hasMetadata() const { return metadataReady_; }
    [[nodiscard]] int metadataWidth() const { return metadataWidth_; }
    [[nodiscard]] int metadataHeight() const { return metadataHeight_; }
    [[nodiscard]] uint32_t metadataFrameId() const { return metadataFrameId_; }
    [[nodiscard]] const std::vector<int8_t>& metadataDisparity() const { return metadataDisparity_; }
    [[nodiscard]] const std::vector<uint8_t>& metadataWorldIds() const { return metadataWorldIds_; }
    [[nodiscard]] const std::vector<int16_t>& metadataSourceX() const { return metadataSourceX_; }
    [[nodiscard]] const std::vector<int16_t>& metadataSourceY() const { return metadataSourceY_; }
    [[nodiscard]] const std::string& romLabel() const { return romPathLabel_; }
    [[nodiscard]] std::string lastError() const { return lastError_; }
    [[nodiscard]] uint16_t inputMask() const { return inputMask_; }
    [[nodiscard]] int audioSampleRate() const { return audioSampleRate_; }

    void onVideoFrame(const void* data, unsigned width, unsigned height, size_t pitch);
    void onAudioBatch(const int16_t* interleavedSamples, size_t frames);
    size_t drainAudioFrames(int16_t* outInterleavedSamples, size_t maxFrames);

private:
    static unsigned mapInputToBitmask(const VbInputState& inputState);
    void captureMetadata(unsigned width, unsigned height);
    void setError(const std::string& error);

    bool initialized_ = false;
    bool romLoaded_ = false;
    bool frameReady_ = false;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    bool metadataReady_ = false;
    int metadataWidth_ = 0;
    int metadataHeight_ = 0;
    uint32_t metadataFrameId_ = 0;
    int audioSampleRate_ = 44100;
    uint16_t inputMask_ = 0;
    std::string romPathLabel_ = "memory.vb";
    std::vector<uint8_t> romData_;
    std::vector<uint32_t> frameBuffer_;
    std::vector<int8_t> metadataDisparity_;
    std::vector<uint8_t> metadataWorldIds_;
    std::vector<int16_t> metadataSourceX_;
    std::vector<int16_t> metadataSourceY_;
    std::deque<int16_t> audioQueue_;
    std::mutex audioMutex_;
    std::string lastError_;
};
