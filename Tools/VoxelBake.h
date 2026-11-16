#pragma once

#include "MeshData.h"
#include "VoxelFoliage.h"

#include <optional>

inline constexpr std::array voxelProbeDirections{
    std::array{1, 0, 0}, std::array{0, 1, 0}, std::array{0, 0, 1},
    std::array{1, 1, 1}, std::array{1, 1, -1}, std::array{1, -1, 1}, std::array{-1, 1, 1}};
using VoxelProbeCounts = std::array<unsigned, voxelProbeDirections.size()>;

struct VoxelBakeCell
{
    VoxelCoordinate coordinate;
    VoxelProbeCounts hits;
};

struct VoxelBake
{
    double cellSize;
    VoxelProbeCounts raysThroughCell;
    std::size_t degenerateTriangles = 0;
    std::vector<VoxelBakeCell> cells;

    double meanCoverage(const VoxelBakeCell& cell) const;
};

VoxelBake sampleVoxelCoverage(const MeshData& mesh, double cellSize, unsigned samplesPerSide);
VoxelFoliage packVoxelFoliage(const VoxelBake& bake, std::uint64_t fingerprint, std::uint32_t meshIndex,
                            std::optional<double> minimumCoverage);

struct VoxelNormalBake
{
    std::vector<PackedVoxelNormals> cells;
    std::size_t fallbackCells = 0;
};
VoxelNormalBake bakeVoxelNormals(const MeshData& mesh, const VoxelFoliage& mask, unsigned samplesPerSide);
std::vector<std::uint8_t> assignVoxelRegions(const VoxelFoliage& mask, std::span<const VoxelBake> samples);
