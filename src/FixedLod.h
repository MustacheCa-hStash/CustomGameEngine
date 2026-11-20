#pragma once

#include "MeshData.h"

#include <algorithm>
#include <array>
#include <filesystem>

inline constexpr std::size_t fixedLodRimLevel = 3;
inline constexpr std::size_t fixedLodFarLevel = 4;
inline constexpr std::size_t fixedLodFourTriangleLevel = 5;
inline constexpr std::size_t fixedLodTwoTriangleLevel = 6;
inline constexpr std::size_t fixedLodLevelCount = 7;
inline constexpr std::size_t fixedLodIndicesOnlyCount = fixedLodFourTriangleLevel;
inline constexpr std::size_t fixedLodSharedCoarseLevel = fixedLodIndicesOnlyCount;
inline constexpr std::size_t fixedLodSharedCoarseLevelCount = fixedLodSharedCoarseLevel + 1;
inline constexpr std::size_t fixedLodProxyVerticesPerComponent = 5;
inline constexpr std::size_t maxSprayLodGroups = 32;
inline constexpr std::size_t maxSprayGroupCuts = 32;
inline constexpr std::size_t maxWholeSprayCuts = 32;

enum class FixedLodTint : std::uint32_t { Shaded, Triangle, Group, Cluster, Level, SprayGroup, BoughProxy };

struct FixedLodIndexRange
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct FixedLodGroupLevel
{
    std::uint32_t firstCluster;
    std::uint32_t clusterCount;
    float sourceToLevel;
    float levelToSource;
    float simplifierError;
    float rimDeviation;

    float surfaceError() const { return std::max(sourceToLevel, levelToSource); }
};

struct FixedLodGroup
{
    Bounds bounds;
    std::uint32_t componentCount;
    std::array<FixedLodGroupLevel, fixedLodLevelCount> levels;
    Bounds proxyBounds;
};

struct FixedLodLevel
{
    std::vector<std::uint32_t> indices;
    std::vector<FixedLodIndexRange> clusters;
};

struct SprayGroupCut
{
    FixedLodIndexRange range;
    float sourceError;
    float referenceError;
    std::uint32_t tintLevel;
    std::uint32_t fittedVertices;
};

struct FixedLod
{
    std::uint32_t meshIndex;
    std::uint32_t triangleCap;
    std::uint32_t componentsPerGroup;
    std::uint32_t sampleSubdivisions;
    std::vector<FixedLodGroup> groups;
    // In group order, with IDs assigned from the original triangle traversal.
    std::vector<std::uint32_t> components;
    // Leaf LOD 5 and 6 share a fitted mesh; an index-only bake uses the source vertices at every level.
    std::vector<Vertex> proxyVertices;
    std::vector<FixedLodLevel> levels;
    // Additional independent cuts for the same parts. Their reference is the full LOD 4 cut.
    std::vector<std::uint32_t> sprayGroupIndices;
    std::vector<std::vector<SprayGroupCut>> sprayGroups;
    std::vector<SprayGroupCut> wholeSprayCuts;

    bool usesProxyVertices(std::size_t level) const { return level >= fixedLodIndicesOnlyCount && !proxyVertices.empty(); }
    FixedLodIndexRange indicesForGroups(std::size_t level, std::size_t firstGroup, std::size_t lastGroup) const;
};

std::filesystem::path fixedLodPath(std::filesystem::path source);
std::uint64_t fixedLodSourceFingerprint(const std::filesystem::path& source);
FixedLod readFixedLod(const std::filesystem::path& source, const AssetData& asset);
FixedLod readFixedLod(const std::filesystem::path& source, const std::filesystem::path& bake, const AssetData& asset);
void writeFixedLod(const std::filesystem::path& output, std::uint64_t fingerprint, const FixedLod& lod);
