#include "stereo_depth_reconstructor.h"

#include <algorithm>
#include <cmath>

void StereoDepthReconstructor::setConfig(const DepthReconstructionConfig& config) {
    config_ = config;
    config_.focalLengthPx = std::max(config_.focalLengthPx, 1.0f);
    config_.baselineMeters = std::max(config_.baselineMeters, 0.001f);
    config_.minDisparityPx = std::max(config_.minDisparityPx, 0.001f);
    config_.nearZ = std::max(config_.nearZ, 0.01f);
    config_.farZ = std::max(config_.farZ, config_.nearZ + 0.01f);
    config_.gridStepX = std::max(config_.gridStepX, 1);
    config_.gridStepY = std::max(config_.gridStepY, 1);
}

float StereoDepthReconstructor::reconstructDepthMeters(const float disparityPx) const {
    const float effectiveDisparity = std::fabs(disparityPx) - config_.disparityBiasPx;
    if (effectiveDisparity < config_.minDisparityPx) {
        return config_.farZ;
    }

    const float z =
        (config_.focalLengthPx * config_.baselineMeters) / std::max(effectiveDisparity, 0.001f);
    return std::clamp(z, config_.nearZ, config_.farZ);
}

bool StereoDepthReconstructor::buildMesh(
    const int8_t* disparity,
    const int disparityWidth,
    const int disparityHeight,
    const int disparityOffsetX,
    const int sampleWidth,
    const int sampleHeight,
    const float uvOffsetX,
    const float uvScaleX,
    DepthMeshData& out) const {
    out = {};
    if (disparity == nullptr || disparityWidth <= 1 || disparityHeight <= 1 || sampleWidth <= 1 ||
        sampleHeight <= 1 || disparityOffsetX < 0 || disparityOffsetX + sampleWidth > disparityWidth) {
        return false;
    }

    const int cols = ((sampleWidth - 1) / config_.gridStepX) + 1;
    const int rows = ((sampleHeight - 1) / config_.gridStepY) + 1;
    if (cols <= 1 || rows <= 1) {
        return false;
    }

    const int vertexCount = cols * rows;
    if (vertexCount >= 65535) {
        return false;
    }

    out.gridColumns = cols;
    out.gridRows = rows;
    out.vertices.resize(static_cast<size_t>(vertexCount) * 5);

    const float cx = static_cast<float>(sampleWidth - 1) * 0.5f;
    const float cy = static_cast<float>(sampleHeight - 1) * 0.5f;
    const float invSampleWidth = 1.0f / static_cast<float>(sampleWidth - 1);
    const float invSampleHeight = 1.0f / static_cast<float>(sampleHeight - 1);

    float prevZ = config_.farZ;
    for (int gy = 0; gy < rows; ++gy) {
        const int py = std::min(gy * config_.gridStepY, sampleHeight - 1);
        for (int gx = 0; gx < cols; ++gx) {
            const int px = std::min(gx * config_.gridStepX, sampleWidth - 1);
            const size_t sampleIndex =
                static_cast<size_t>(py) * static_cast<size_t>(disparityWidth) +
                static_cast<size_t>(disparityOffsetX + px);
            const float disparityPx = static_cast<float>(disparity[sampleIndex]);

            float z = reconstructDepthMeters(disparityPx);
            if (!std::isfinite(z)) {
                z = prevZ;
            }
            prevZ = z;

            const float xMeters = (static_cast<float>(px) - cx) * z / config_.focalLengthPx;
            const float yMeters = (cy - static_cast<float>(py)) * z / config_.focalLengthPx;
            const float zMeters = -(z + config_.baseDistanceMeters);

            const float u = uvOffsetX + (static_cast<float>(px) * invSampleWidth) * uvScaleX;
            const float v = static_cast<float>(py) * invSampleHeight;

            const size_t dst = static_cast<size_t>((gy * cols) + gx) * 5;
            out.vertices[dst + 0] = xMeters;
            out.vertices[dst + 1] = yMeters;
            out.vertices[dst + 2] = zMeters;
            out.vertices[dst + 3] = u;
            out.vertices[dst + 4] = v;
        }
    }

    out.indices.reserve(static_cast<size_t>(cols - 1) * static_cast<size_t>(rows - 1) * 6);
    for (int gy = 0; gy < rows - 1; ++gy) {
        for (int gx = 0; gx < cols - 1; ++gx) {
            const uint16_t i0 = static_cast<uint16_t>((gy * cols) + gx);
            const uint16_t i1 = static_cast<uint16_t>((gy * cols) + (gx + 1));
            const uint16_t i2 = static_cast<uint16_t>(((gy + 1) * cols) + gx);
            const uint16_t i3 = static_cast<uint16_t>(((gy + 1) * cols) + (gx + 1));

            out.indices.push_back(i0);
            out.indices.push_back(i2);
            out.indices.push_back(i1);
            out.indices.push_back(i1);
            out.indices.push_back(i2);
            out.indices.push_back(i3);
        }
    }

    out.valid = !out.vertices.empty() && !out.indices.empty();
    return out.valid;
}
