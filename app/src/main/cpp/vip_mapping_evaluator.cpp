#include "vip_mapping_evaluator.h"

#include <cstddef>

bool VipMappingEvaluator::bind(
    const int16_t* sourceX,
    const int16_t* sourceY,
    const int width,
    const int height,
    const int eyeWidth,
    const int eyeHeight) {
    sourceX_ = sourceX;
    sourceY_ = sourceY;
    width_ = width;
    height_ = height;
    eyeWidth_ = eyeWidth;
    eyeHeight_ = eyeHeight;
    valid_ = sourceX_ != nullptr && sourceY_ != nullptr && width_ > 0 && height_ > 0 &&
             eyeWidth_ > 0 && eyeHeight_ > 0;
    return valid_;
}

VipMappingEvaluator::EyeSample VipMappingEvaluator::evaluateEye(
    const int eye, const int x, const int y) const {
    EyeSample sample{};
    if (!valid_ || eye < 0 || eye > 1 || x < 0 || y < 0 || x >= eyeWidth_ || y >= eyeHeight_) {
        return sample;
    }

    const int screenX = x + (eye * eyeWidth_);
    if (screenX < 0 || screenX >= width_ || y >= height_) {
        return sample;
    }

    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width_) +
                         static_cast<size_t>(screenX);
    const int16_t rawX = sourceX_[index];
    const int16_t rawY = sourceY_[index];
    if (rawX == kInvalidSourceCoord || rawY == kInvalidSourceCoord) {
        return sample;
    }

    sample.sx = static_cast<float>(rawX);
    sample.sy = static_cast<float>(rawY);
    sample.valid = true;
    return sample;
}
