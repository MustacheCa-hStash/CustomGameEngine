#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

inline constexpr int voxelBrickSide = 4;
using VoxelCoordinate = std::array<std::int32_t, 3>;

struct VoxelBrick
{
    VoxelCoordinate coordinate;
    std::uint64_t occupancy;

    bool operator==(const VoxelBrick&) const = default;
};

struct VoxelFoliage
{
    std::uint64_t sourceFingerprint;
    std::uint32_t meshIndex;
    std::array<double, 3> origin;
    double cellSize;
    std::vector<VoxelBrick> bricks;

    bool operator==(const VoxelFoliage&) const = default;
};

VoxelFoliage readVoxelFoliage(const std::filesystem::path& path);
void writeVoxelFoliage(const std::filesystem::path& path, const VoxelFoliage& foliage);

using PackedVoxelNormals = std::array<std::uint32_t, 4>;
struct VoxelNormalData
{
    std::vector<PackedVoxelNormals> cells;
    bool hasDirectionalWeights = false;
    bool hasShadowReceivers = false;
};
std::size_t occupiedVoxelCount(const VoxelFoliage& foliage);
VoxelNormalData readVoxelNormals(const std::filesystem::path& path, std::uint64_t maskHash, std::size_t cellCount);
void writeVoxelNormals(const std::filesystem::path& path, std::uint64_t maskHash, std::span<const PackedVoxelNormals> cells);

inline constexpr std::size_t voxelBranchRegionCount = 4;
inline constexpr std::size_t voxelRegionGroupStart(std::size_t region, std::size_t groupCount)
{
    return region * groupCount / voxelBranchRegionCount;
}
std::vector<std::uint8_t> readVoxelRegionOwners(const std::filesystem::path& path, std::uint64_t maskHash,
                                               std::uint64_t lodHash, std::size_t cellCount);
void writeVoxelRegionOwners(const std::filesystem::path& path, std::uint64_t maskHash, std::uint64_t lodHash,
                           std::span<const std::uint8_t> owners);
