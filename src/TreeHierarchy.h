#pragma once

#include "SprayBough.h"

struct TreeLodNode
{
    Bounds bounds;
    FixedLodIndexRange geometry;
    float error = 0;
    float referenceError = 0;
    std::uint32_t firstChild = 0;
    std::uint32_t childCount = 0;
    std::uint32_t firstPlacement = 0;
    std::uint32_t placementCount = 0;
};

struct TreeLodPart
{
    std::uint32_t meshIndex;
    std::vector<TreeLodNode> nodes;
    MeshData geometry;
    std::vector<std::uint32_t> placementOrder; // Optional per-part order for independently grouped materials.
    // 1 draws the local range per placement. 2 chooses the reusable or stock subtree.
    std::vector<std::uint8_t> instancedNodes;
};

struct TreeHierarchy
{
    std::vector<std::uint32_t> placementOrder;
    std::vector<TreeLodPart> parts;
};

std::filesystem::path treeHierarchyPath(std::filesystem::path glb);
TreeHierarchy readTreeHierarchy(const SprayBoughSources& sources, const AssetData& asset);
void writeTreeHierarchy(const std::filesystem::path& path, const SprayBoughSources& sources,
                        const TreeHierarchy& hierarchy);
