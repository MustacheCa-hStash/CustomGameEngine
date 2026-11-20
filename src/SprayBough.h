#pragma once

#include "FixedLod.h"

struct SprayBough
{
    std::vector<std::uint32_t> placements;
    std::array<MeshData, 2> parts;
    float error = 0.0f;
};

using SprayBoughSources = std::array<std::filesystem::path, 3>;
// Keep the aggregate bake anchored to the same inputs when a shared spray level is appended.
inline std::size_t sprayBoughInputLevel(const FixedLod& lod)
{
    return lod.levels.size() == fixedLodSharedCoarseLevelCount ? fixedLodFarLevel : lod.levels.size() - 1;
}
SprayBoughSources sprayBoughSources(const std::filesystem::path& glb);
SprayBough readSprayBough(const SprayBoughSources& sources, const AssetData& asset,
                         const std::array<std::shared_ptr<const FixedLod>, 2>& lods,
                         const std::filesystem::path& bake = {});
void writeSprayBough(const std::filesystem::path& output, const SprayBoughSources& sources, const SprayBough& bough);
