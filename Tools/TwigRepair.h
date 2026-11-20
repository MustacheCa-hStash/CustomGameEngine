#pragma once

#include "MeshData.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <span>

namespace twig_repair
{
using TwigIndices = std::vector<std::uint32_t>;

inline TwigIndices fitTwigPatch(const MeshData& mesh, std::span<const std::uint32_t> indices, float allowance,
                               const unsigned char* locks = nullptr)
{
    TwigIndices fit(indices.size());
    constexpr std::array weights{0.002f, 0.002f, 0.002f, 0.01f, 0.01f, 0.01f, 0.01f};
    fit.resize(meshopt_simplifyWithAttributes(fit.data(), indices.data(), indices.size(),
        &mesh.vertices.front().position.x, mesh.vertices.size(), sizeof(Vertex),
        &mesh.vertices.front().normal.x, sizeof(Vertex), weights.data(), weights.size(), locks,
        3, allowance, meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, nullptr));
    return fit;
}

inline DirectX::XMVECTOR twigProjectedAreas(const MeshData& mesh, const TwigIndices& indices)
{
    using namespace DirectX;
    auto sum = XMVectorZero();
    for (std::size_t i = 0; i < indices.size(); i += 3)
    {
        const auto point = [&](unsigned j) { return XMLoadFloat3(&mesh.vertices[indices[i + j]].position); };
        sum += XMVectorAbs(XMVector3Cross(point(1) - point(0), point(2) - point(0)));
    }
    return sum;
}

enum class TwigRepair { Loose, Anchored, Intermediate };

inline TwigIndices fitAnchoredTwigPatches(const MeshData& mesh, std::span<const std::uint32_t> failed, float allowance,
                                         TwigRepair recipe)
{
    using namespace DirectX;
    // Index connectivity preserves material/attribute splits. Only the failed faces
    // participate, so meshoptimizer's locked borders include the repair boundary.
    TwigIndices parent(mesh.vertices.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](unsigned i) {
        while (parent[i] != i)
        {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (std::size_t i = 0; i < failed.size(); i += 3)
    {
        auto id = root(failed[i]);
        parent[root(failed[i + 1])] = id;
        parent[root(failed[i + 2])] = id;
    }
    std::vector<TwigIndices> patches(mesh.vertices.size());
    for (auto v : failed) patches[root(v)].push_back(v);
    TwigIndices fit, remap(mesh.vertices.size(), UINT32_MAX);
    for (const auto& patch : patches)
    {
        if (patch.empty()) continue;
        MeshData local;
        TwigIndices global;
        for (auto v : patch)
        {
            if (remap[v] == UINT32_MAX)
            {
                remap[v] = static_cast<unsigned>(global.size());
                global.push_back(v);
                local.vertices.push_back(mesh.vertices[v]);
            }
            local.indices.push_back(remap[v]);
        }
        std::vector<unsigned char> locked(global.size());
        // Both recipes keep the longitudinal endpoints. The stricter one keeps
        // transverse extrema too, which protects the apparent width of a thin twig.
        float longest = -1;
        std::size_t first = 0, last = 0;
        for (auto axis : {&XMFLOAT3::x, &XMFLOAT3::y, &XMFLOAT3::z})
        {
            const auto [lo, hi] = std::ranges::minmax_element(local.vertices, {}, [axis](const Vertex& v) { return v.position.*axis; });
            const auto span = hi->position.*axis - lo->position.*axis;
            if (span > longest)
            {
                longest = span;
                first = lo - local.vertices.begin();
                last = hi - local.vertices.begin();
            }
            if (recipe == TwigRepair::Anchored)
                locked[lo - local.vertices.begin()] = locked[hi - local.vertices.begin()] = meshopt_SimplifyVertex_Lock;
        }
        locked[first] = locked[last] = meshopt_SimplifyVertex_Lock;
        auto indices = fitTwigPatch(local, local.indices, allowance * (recipe == TwigRepair::Anchored ? 0.125f : 1.0f), locked.data());
        const auto area = twigProjectedAreas(local, local.indices);
        const float areaTolerance = recipe == TwigRepair::Anchored ? 0.005f : 0.05f;
        if (indices.empty() || !XMVector3LessOrEqual(XMVectorAbs(twigProjectedAreas(local, indices) - area), area * areaTolerance))
            indices = local.indices;
        for (auto v : indices) fit.push_back(global[v]);
        for (auto v : global) remap[v] = UINT32_MAX;
    }
    return fit;
}
}
