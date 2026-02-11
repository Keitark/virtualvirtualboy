#pragma once

#include <cstdint>
#include <vector>

struct DepthReconstructionConfig {
    float focalLengthPx = 250.0f;
    float baselineMeters = 0.064f;
    float disparityBiasPx = 0.0f;
    float minDisparityPx = 0.30f;
    float nearZ = 0.45f;
    float farZ = 8.5f;
    float baseDistanceMeters = 1.25f;
    int gridStepX = 8;
    int gridStepY = 2;
};

struct DepthMeshData {
    std::vector<float> vertices;  // xyzuv (5 floats)
    std::vector<uint16_t> indices;
    int gridColumns = 0;
    int gridRows = 0;
    bool valid = false;
};

class StereoDepthReconstructor {
public:
    void setConfig(const DepthReconstructionConfig& config);
    [[nodiscard]] const DepthReconstructionConfig& config() const { return config_; }

    bool buildMesh(
        const int8_t* disparity,
        int disparityWidth,
        int disparityHeight,
        int disparityOffsetX,
        int sampleWidth,
        int sampleHeight,
        float uvOffsetX,
        float uvScaleX,
        DepthMeshData& out) const;

private:
    [[nodiscard]] float reconstructDepthMeters(float disparityPx) const;

    DepthReconstructionConfig config_{};
};
