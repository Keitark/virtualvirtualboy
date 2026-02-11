#pragma once

#include <cstdint>

class VipMappingEvaluator {
public:
    static constexpr int16_t kInvalidSourceCoord = static_cast<int16_t>(-32768);

    struct EyeSample {
        float sx = 0.0f;
        float sy = 0.0f;
        bool valid = false;
    };

    bool bind(
        const int16_t* sourceX,
        const int16_t* sourceY,
        int width,
        int height,
        int eyeWidth,
        int eyeHeight);

    [[nodiscard]] EyeSample evaluateEye(int eye, int x, int y) const;

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] bool stereoReady() const { return valid_ && width_ >= eyeWidth_ * 2; }
    [[nodiscard]] int eyeWidth() const { return eyeWidth_; }
    [[nodiscard]] int eyeHeight() const { return eyeHeight_; }

private:
    const int16_t* sourceX_ = nullptr;
    const int16_t* sourceY_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int eyeWidth_ = 0;
    int eyeHeight_ = 0;
    bool valid_ = false;
};
