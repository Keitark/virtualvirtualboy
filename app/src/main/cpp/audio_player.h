#pragma once

#include <cstdint>
#include <aaudio/AAudio.h>

class AudioPlayer {
public:
    bool ensureStarted(int sampleRate, int channelCount);
    void shutdown();

    [[nodiscard]] bool initialized() const { return stream_ != nullptr; }
    [[nodiscard]] int sampleRate() const { return sampleRate_; }
    [[nodiscard]] int channelCount() const { return channelCount_; }

    bool writeFrames(const int16_t* interleavedPcm, int32_t frameCount);

private:
    bool open(int sampleRate, int channelCount);

    AAudioStream* stream_ = nullptr;
    int sampleRate_ = 0;
    int channelCount_ = 0;
};
