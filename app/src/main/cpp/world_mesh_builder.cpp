#include "world_mesh_builder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

bool WorldMeshBuilder::buildStereoMeshes(
    const VipMappingEvaluator& mappingEvaluator,
    const StereoDepthReconstructor& reconstructor,
    std::array<DepthMeshData, 2>& outMeshes) const {
    outMeshes[0] = {};
    outMeshes[1] = {};

    if (!mappingEvaluator.stereoReady()) {
        return false;
    }

    const int eyeWidth = mappingEvaluator.eyeWidth();
    const int eyeHeight = mappingEvaluator.eyeHeight();
    if (eyeWidth <= 1 || eyeHeight <= 1) {
        return false;
    }

    const DepthReconstructionConfig& cfg = reconstructor.config();
    const int stepX = std::max(1, cfg.gridStepX);
    const int stepY = std::max(1, cfg.gridStepY);
    const int cols = ((eyeWidth - 1) / stepX) + 1;
    const int rows = ((eyeHeight - 1) / stepY) + 1;
    if (cols <= 1 || rows <= 1) {
        return false;
    }

    DepthMeshData mesh{};
    mesh.gridColumns = cols;
    mesh.gridRows = rows;
    mesh.vertices.resize(static_cast<size_t>(cols * rows) * 5);

    const float cx = static_cast<float>(eyeWidth - 1) * 0.5f;
    const float cy = static_cast<float>(eyeHeight - 1) * 0.5f;
    const float invW = 1.0f / static_cast<float>(eyeWidth - 1);
    const float invH = 1.0f / static_cast<float>(eyeHeight - 1);

    float prevD = 0.0f;
    for (int gy = 0; gy < rows; ++gy) {
        const int py = std::min(gy * stepY, eyeHeight - 1);
        for (int gx = 0; gx < cols; ++gx) {
            const int px = std::min(gx * stepX, eyeWidth - 1);

            auto left = mappingEvaluator.evaluateEye(0, px, py);
            auto right = mappingEvaluator.evaluateEye(1, px, py);
            if (!left.valid || !right.valid) {
                left.sx = static_cast<float>(px);
                left.sy = static_cast<float>(py);
                right.sx = static_cast<float>(px);
                right.sy = static_cast<float>(py);
                left.valid = true;
                right.valid = true;
            }

            const int nx = std::min(px + stepX, eyeWidth - 1);
            auto leftNext = mappingEvaluator.evaluateEye(0, nx, py);
            auto rightNext = mappingEvaluator.evaluateEye(1, nx, py);
            if (!leftNext.valid || !rightNext.valid) {
                leftNext = left;
                rightNext = right;
            }

            const float sCx = (left.sx + right.sx) * 0.5f;
            const float sCy = (left.sy + right.sy) * 0.5f;
            const float sCxN = (leftNext.sx + rightNext.sx) * 0.5f;
            const float sCyN = (leftNext.sy + rightNext.sy) * 0.5f;

            const float tx = sCxN - sCx;
            const float ty = sCyN - sCy;
            const float dsx = left.sx - right.sx;
            const float dsy = left.sy - right.sy;

            const float t2 = (tx * tx) + (ty * ty);
            float d = prevD;
            if (t2 > 1e-4f) {
                d = ((tx * dsx) + (ty * dsy)) / t2;
            }
            prevD = d;

            const float denom = d - cfg.disparityBiasPx;
            float zMeters = cfg.farZ;
            if (std::fabs(denom) >= cfg.minDisparityPx) {
                zMeters = (cfg.focalLengthPx * cfg.baselineMeters) / std::fabs(denom);
                zMeters = std::clamp(zMeters, cfg.nearZ, cfg.farZ);
            }

            const float xMeters = (static_cast<float>(px) - cx) * zMeters / cfg.focalLengthPx;
            const float yMeters = (cy - static_cast<float>(py)) * zMeters / cfg.focalLengthPx;
            const float zWorld = -(zMeters + cfg.baseDistanceMeters);

            const float u = static_cast<float>(px) * invW;
            const float v = static_cast<float>(py) * invH;

            const size_t dst = static_cast<size_t>((gy * cols) + gx) * 5;
            mesh.vertices[dst + 0] = xMeters;
            mesh.vertices[dst + 1] = yMeters;
            mesh.vertices[dst + 2] = zWorld;
            mesh.vertices[dst + 3] = u;
            mesh.vertices[dst + 4] = v;
        }
    }

    mesh.indices.reserve(static_cast<size_t>(cols - 1) * static_cast<size_t>(rows - 1) * 6);
    for (int gy = 0; gy < rows - 1; ++gy) {
        for (int gx = 0; gx < cols - 1; ++gx) {
            const uint16_t i0 = static_cast<uint16_t>((gy * cols) + gx);
            const uint16_t i1 = static_cast<uint16_t>((gy * cols) + (gx + 1));
            const uint16_t i2 = static_cast<uint16_t>(((gy + 1) * cols) + gx);
            const uint16_t i3 = static_cast<uint16_t>(((gy + 1) * cols) + (gx + 1));

            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }

    mesh.valid = !mesh.vertices.empty() && !mesh.indices.empty();
    if (!mesh.valid) {
        return false;
    }

    outMeshes[0] = mesh;
    outMeshes[1] = mesh;
    return true;
}
