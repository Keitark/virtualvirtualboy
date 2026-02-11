#pragma once

#include <array>

#include "stereo_depth_reconstructor.h"
#include "vip_mapping_evaluator.h"

class WorldMeshBuilder {
public:
    bool buildStereoMeshes(
        const VipMappingEvaluator& mappingEvaluator,
        const StereoDepthReconstructor& reconstructor,
        std::array<DepthMeshData, 2>& outMeshes) const;
};
