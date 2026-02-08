#include "audio_player.h"

#include <aaudio/AAudio.h>

#include "log.h"

bool AudioPlayer::open(const int sampleRate, const int channelCount) {
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
        LOGE("AAudio_createStreamBuilder failed");
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(builder, sampleRate);
    AAudioStreamBuilder_setChannelCount(builder, channelCount);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);

    aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", static_cast<int>(result));
        stream_ = nullptr;
        return false;
    }

    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
        LOGE("AAudioStream_requestStart failed: %d", static_cast<int>(result));
        AAudioStream_close(stream_);
        stream_ = nullptr;
        return false;
    }

    sampleRate_ = AAudioStream_getSampleRate(stream_);
    channelCount_ = AAudioStream_getChannelCount(stream_);
    LOGI("Audio stream started: %d Hz, %d ch", sampleRate_, channelCount_);
    return true;
}

bool AudioPlayer::ensureStarted(const int sampleRate, const int channelCount) {
    if (stream_ != nullptr && sampleRate_ == sampleRate && channelCount_ == channelCount) {
        return true;
    }
    shutdown();
    return open(sampleRate, channelCount);
}

bool AudioPlayer::writeFrames(const int16_t* interleavedPcm, const int32_t frameCount) {
    if (stream_ == nullptr || interleavedPcm == nullptr || frameCount <= 0) {
        return false;
    }

    const aaudio_result_t written = AAudioStream_write(stream_, interleavedPcm, frameCount, 0);
    if (written < 0) {
        LOGE("AAudioStream_write failed: %d", static_cast<int>(written));
        return false;
    }

    return true;
}

void AudioPlayer::shutdown() {
    if (stream_ != nullptr) {
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);
        stream_ = nullptr;
    }
    sampleRate_ = 0;
    channelCount_ = 0;
}

