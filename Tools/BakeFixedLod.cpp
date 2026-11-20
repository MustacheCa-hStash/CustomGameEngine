#include "FixedLod.h"
#include "GltfLoader.h"
#include "LeafOutline.h"
#include "SurfaceDistance.h"

#include <meshoptimizer.h>
#include "MeshletPacking.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace DirectX;

namespace
{
using Index = std::uint32_t;
using Indices = std::vector<Index>;
using Edge = std::pair<Index, Index>;
using surface_distance::SurfaceTriangle;
using surface_distance::sampledDistance;
using surface_distance::segmentDistanceSquared;
using surface_distance::surface;

struct Component
{
    Indices indices;
    Bounds bounds;
};

struct Recipe
{
    unsigned triangleCap = 128;
    unsigned groupSize = 24;
    unsigned samples = 4;
    std::array<unsigned, fixedLodIndicesOnlyCount> targets{100, 80, 50, 35, 25};
    std::array<unsigned, fixedLodIndicesOnlyCount> rimTolerancePercent{0, 0, 0, 10, 15};
    bool useUvEnds = false;
    bool useLeafProxies = false;
    bool weldNormalSplits = false;
    std::optional<std::size_t> firstTransitionLevel;
    unsigned farComponentTarget = 0;
    std::optional<float> farComponentErrorLimit;
    unsigned sharedFarTarget = 0;
    std::optional<float> sharedFarUvWeight;
    unsigned sharedCoarseTarget = 0;
    unsigned sprayGroupCount = 0;
    std::uint32_t textureCoordinateSet = 0;
    float uvWeight = 1.0f;
};

struct LevelStats
{
    std::size_t targetMisses = 0;
    std::size_t unchanged = 0;
    std::size_t rejected = 0;
    std::size_t uvLimited = 0;
    std::size_t changedRims = 0;
    std::size_t rimEdges = 0;
    float sourceToLevel = 0;
    float levelToSource = 0;
    float simplifierError = 0;
    float rimDeviation = 0;
    float planarAreaErrorPercent = 0;
};

struct TransitionStats
{
    float fineToCoarse = 0.0f;
    float coarseToFine = 0.0f;
    float rimDeviation = 0.0f;
    std::size_t comparableRims = 0;
    std::size_t unmatchedRims = 0;
};

using TransitionTable = std::array<std::array<TransitionStats, fixedLodLevelCount>, fixedLodLevelCount>;

Bounds mergeBounds(const Bounds& a, const Bounds& b)
{
    const auto ac = XMLoadFloat3(&a.center);
    const auto ah = XMLoadFloat3(&a.halfExtent);
    const auto bc = XMLoadFloat3(&b.center);
    const auto bh = XMLoadFloat3(&b.halfExtent);
    const auto lower = XMVectorMin(ac - ah, bc - bh);
    const auto upper = XMVectorMax(ac + ah, bc + bh);
    Bounds result;
    XMStoreFloat3(&result.center, (upper + lower) * 0.5f);
    XMStoreFloat3(&result.halfExtent, (upper - lower) * 0.5f);
    return result;
}

Bounds boundsOf(std::span<const Vertex> vertices, const Indices& indices)
{
    auto lower = XMLoadFloat3(&vertices[indices.front()].position);
    auto upper = lower;
    for (auto index : indices)
    {
        const auto point = XMLoadFloat3(&vertices[index].position);
        lower = XMVectorMin(lower, point);
        upper = XMVectorMax(upper, point);
    }
    Bounds result;
    XMStoreFloat3(&result.center, (upper + lower) * 0.5f);
    XMStoreFloat3(&result.halfExtent, (upper - lower) * 0.5f);
    return result;
}

Indices vertexRepresentatives(const MeshData& mesh)
{
    // Keep texture seams. The PVE wood export splits vertices at every triangle's normal.
    std::map<std::array<float, 7>, Index> representatives;
    Indices remap;
    remap.reserve(mesh.vertices.size());
    for (Index i = 0; i < mesh.vertices.size(); ++i)
    {
        const auto& v = mesh.vertices[i];
        const std::array key{v.position.x, v.position.y, v.position.z, v.texcoords[0].x, v.texcoords[0].y,
                             v.texcoords[1].x, v.texcoords[1].y};
        remap.push_back(representatives.try_emplace(key, i).first->second);
    }
    return remap;
}

std::vector<Component> findComponents(const MeshData& mesh, std::span<const Index> representatives)
{
    const auto triangleCount = static_cast<Index>(mesh.indices.size() / 3);
    Indices parents(triangleCount);
    std::iota(parents.begin(), parents.end(), 0u);
    const auto root = [&](Index triangle) {
        while (parents[triangle] != triangle)
        {
            parents[triangle] = parents[parents[triangle]];
            triangle = parents[triangle];
        }
        return triangle;
    };

    std::map<Edge, Index> firstTriangle;
    for (Index triangle = 0; triangle < triangleCount; ++triangle)
    {
        const auto* corners = &mesh.indices[triangle * 3];
        const auto a = XMLoadFloat3(&mesh.vertices[corners[0]].position);
        const auto b = XMLoadFloat3(&mesh.vertices[corners[1]].position);
        const auto c = XMLoadFloat3(&mesh.vertices[corners[2]].position);
        if (XMVectorGetX(XMVector3LengthSq(XMVector3Cross(b - a, c - a))) == 0)
        {
            throw std::runtime_error("The selected mesh contains a degenerate triangle.");
        }
        for (unsigned corner = 0; corner < 3; ++corner)
        {
            auto first = corners[corner];
            auto second = corners[(corner + 1) % 3];
            if (!representatives.empty())
            {
                first = representatives[first];
                second = representatives[second];
            }
            const auto [edge, inserted] = firstTriangle.try_emplace(std::minmax(first, second), triangle);
            if (!inserted) parents[root(triangle)] = root(edge->second);
        }
    }

    std::vector<Component> components;
    Indices componentIds(triangleCount, std::numeric_limits<Index>::max());
    for (Index triangle = 0; triangle < triangleCount; ++triangle)
    {
        auto& id = componentIds[root(triangle)];
        if (id == std::numeric_limits<Index>::max())
        {
            id = static_cast<Index>(components.size());
            components.emplace_back();
        }
        auto& indices = components[id].indices;
        const auto* corners = &mesh.indices[triangle * 3];
        indices.insert(indices.end(), corners, corners + 3);
    }
    for (auto& component : components) component.bounds = boundsOf(mesh.vertices, component.indices);
    return components;
}

void partitionGroups(std::span<Index> ids, std::size_t count, const std::vector<Component>& components,
                     std::vector<std::span<const Index>>& groups)
{
    if (count == 1)
    {
        groups.emplace_back(ids);
        return;
    }
    Bounds centers{components[ids.front()].bounds.center, {}};
    for (auto id : ids) centers = mergeBounds(centers, {components[id].bounds.center, {}});
    const auto half = centers.halfExtent;
    const auto longest = half.x >= half.y && half.x >= half.z ? &XMFLOAT3::x :
                         half.y >= half.z ? &XMFLOAT3::y : &XMFLOAT3::z;
    std::sort(ids.begin(), ids.end(), [&](auto a, auto b) {
        const float x = components[a].bounds.center.*longest;
        const float y = components[b].bounds.center.*longest;
        return x == y ? a < b : x < y;
    });
    const auto leftCount = count / 2;
    const auto split = ids.size() * leftCount / count;
    partitionGroups(ids.first(split), leftCount, components, groups);
    partitionGroups(ids.subspan(split), count - leftCount, components, groups);
}

Indices remapIndices(Indices& indices)
{
    Indices vertices = indices;
    std::sort(vertices.begin(), vertices.end());
    vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
    for (auto& index : indices)
    {
        index = static_cast<Index>(std::lower_bound(vertices.begin(), vertices.end(), index) - vertices.begin());
    }
    return vertices;
}

std::vector<Edge> borderEdges(std::span<const Index> indices)
{
    struct EdgeUse { unsigned count; Edge directed; };
    std::map<Edge, EdgeUse> uses;
    for (std::size_t i = 0; i < indices.size(); i += 3)
    {
        for (unsigned corner = 0; corner < 3; ++corner)
        {
            const auto a = indices[i + corner];
            const auto b = indices[i + (corner + 1) % 3];
            auto [entry, inserted] = uses.try_emplace(std::minmax(a, b), EdgeUse{0, {a, b}});
            ++entry->second.count;
        }
    }
    std::vector<Edge> border;
    for (const auto& [key, use] : uses)
    {
        if (use.count == 1) border.push_back(use.directed);
    }
    return border;
}

Edge rimAnchors(const std::vector<Edge>& border, const Indices& vertices,
                const std::vector<XMFLOAT3>& positions, const MeshData& mesh, bool useUvEnds)
{
    Indices rim;
    for (const auto& edge : border) rim.push_back(edge.first);
    Edge anchors{};
    if (useUvEnds)
    {
        const auto uv = [&](Index index) { return mesh.vertices[vertices[index]].texcoords[0]; };
        const auto endOrder = [&](bool highV, Index a, Index b) {
            const auto u = uv(a);
            const auto v = uv(b);
            if (u.y != v.y) return highV ? u.y > v.y : u.y < v.y;
            return std::abs(u.x - 0.5f) < std::abs(v.x - 0.5f);
        };
        anchors.first = *std::min_element(rim.begin(), rim.end(), [&](auto a, auto b) { return endOrder(false, a, b); });
        anchors.second = *std::min_element(rim.begin(), rim.end(), [&](auto a, auto b) { return endOrder(true, a, b); });
    }
    else
    {
        float longest = 0;
        for (auto a : rim)
        {
            const auto point = XMLoadFloat3(&positions[a]);
            for (auto b : rim)
            {
                const auto distance = XMVectorGetX(XMVector3LengthSq(point - XMLoadFloat3(&positions[b])));
                if (distance > longest)
                {
                    longest = distance;
                    anchors = {a, b};
                }
            }
        }
    }
    return anchors;
}

template<class PositionA, class PositionB>
float rimDistance(std::span<const Edge> firstRim, std::span<const Edge> secondRim,
                  const PositionA& positionA, const PositionB& positionB, unsigned subdivisions)
{
    const auto directed = [&](std::span<const Edge> from, std::span<const Edge> to,
                              const auto& fromPosition, const auto& toPosition) {
        float worst = 0;
        for (const auto& [a, b] : from)
        {
            const auto first = fromPosition(a);
            const auto last = fromPosition(b);
            for (unsigned sample = 0; sample <= subdivisions; ++sample)
            {
                const auto point = XMVectorLerp(first, last,
                                                static_cast<float>(sample) / static_cast<float>(subdivisions));
                float nearest = std::numeric_limits<float>::infinity();
                for (const auto& [c, d] : to)
                {
                    const auto start = toPosition(c);
                    nearest = std::min(nearest, segmentDistanceSquared(point - start, toPosition(d) - start));
                }
                worst = std::max(worst, nearest);
            }
        }
        return worst;
    };
    return std::sqrt(std::max(directed(firstRim, secondRim, positionA, positionB),
                              directed(secondRim, firstRim, positionB, positionA)));
}

float rimDistance(std::span<const Edge> a, std::span<const Edge> b,
                  const std::vector<XMFLOAT3>& positions, unsigned subdivisions)
{
    const auto position = [&](Index i) { return XMLoadFloat3(&positions[i]); };
    return rimDistance(a, b, position, position, subdivisions);
}

bool hasCollapsedUvTriangle(const Indices& indices, const std::vector<XMFLOAT2>& uvs)
{
    for (std::size_t i = 0; i < indices.size(); i += 3)
    {
        const auto& a = uvs[indices[i]];
        const auto& b = uvs[indices[i + 1]];
        const auto& c = uvs[indices[i + 2]];
        const double abX = static_cast<double>(b.x) - a.x;
        const double abY = static_cast<double>(b.y) - a.y;
        const double acX = static_cast<double>(c.x) - a.x;
        const double acY = static_cast<double>(c.y) - a.y;
        if (abX * acY - abY * acX == 0) return true;
    }
    return false;
}

void bakeLeafProxies(const MeshData& mesh, const Indices& vertices, std::vector<XMFLOAT3> positions,
                     const std::vector<Edge>& border, Edge anchors, const std::vector<SurfaceTriangle>& reference,
                     unsigned samples, FixedLod& lod, std::array<Indices, fixedLodLevelCount>& groupIndices,
                     FixedLodGroup& group, std::array<LevelStats, fixedLodLevelCount>& stats)
{
    // The outline is shared by both proxies. Only the four-triangle fan uses the center vertex.
    const auto& start = mesh.vertices[vertices[anchors.first]];
    const auto& end = mesh.vertices[vertices[anchors.second]];
    const auto a = XMLoadFloat3(&start.position);
    const auto b = XMLoadFloat3(&end.position);
    const auto length = XMVectorGetX(XMVector3Length(b - a));
    if (border.empty() || !(length > 0) || start.texcoords[0].y >= end.texcoords[0].y ||
        start.texcoords[0].x != 0.5f || end.texcoords[0].x != 0.5f)
    {
        throw std::runtime_error("Leaf proxies need an open UV-V strip with both centerline endpoints.");
    }
    const auto axis = (b - a) / length;
    const auto dot = [](FXMVECTOR p, FXMVECTOR q) { return XMVectorGetX(XMVector3Dot(p, q)); };
    auto areaVector = XMVectorZero();
    for (const auto& triangle : reference) areaVector += triangle.normal * 0.5f;
    areaVector -= axis * dot(areaVector, axis);
    const float sourceArea = XMVectorGetX(XMVector3Length(areaVector));
    if (!(sourceArea > 0)) throw std::runtime_error("Leaf proxy needs positive projected source area.");
    const auto normal = areaVector / sourceArea;
    auto across = XMVector3Cross(axis, normal);

    const float middleV = (start.texcoords[0].y + end.texcoords[0].y) * 0.5f;
    const auto vertexAtMiddle = [&](float u) {
        const auto distance = [&](Index i) {
            const auto uv = mesh.vertices[i].texcoords[0];
            const float du = uv.x - u;
            const float dv = uv.y - middleV;
            return du * du + dv * dv;
        };
        const auto i = *std::min_element(vertices.begin(), vertices.end(), [&](auto x, auto y) {
            return distance(x) < distance(y);
        });
        auto vertex = mesh.vertices[i];
        vertex.texcoords[0] = {u, middleV};
        return vertex;
    };
    auto left = vertexAtMiddle(0.0f);
    auto right = vertexAtMiddle(1.0f);
    auto center = vertexAtMiddle(0.5f);
    const auto mid = XMLoadFloat3(&center.position);
    const auto leftPoint = XMLoadFloat3(&left.position);
    const auto rightPoint = XMLoadFloat3(&right.position);
    const bool reverse = dot(rightPoint - leftPoint, across) < 0;
    if (reverse) across = -across;
    const float middle = dot(mid - a, axis);
    const auto chord = a + axis * (middle > 0 && middle < length ? middle : length * 0.5f);
    const float leftWidth = std::abs(dot(mid - leftPoint, across));
    const float rightWidth = std::abs(dot(rightPoint - mid, across));
    const float leftFraction = leftWidth > 0 && rightWidth > 0 ? leftWidth / (leftWidth + rightWidth) : 0.5f;
    const float totalWidth = 2.0f * sourceArea / length;
    XMStoreFloat3(&left.position, chord - across * (totalWidth * leftFraction));
    XMStoreFloat3(&right.position, chord + across * (totalWidth * (1.0f - leftFraction)));
    XMStoreFloat3(&center.position, chord + normal * dot(mid - chord, normal));

    const auto firstVertex = static_cast<Index>(lod.proxyVertices.size());
    const std::array proxy{start, left, end, right, center};
    lod.proxyVertices.insert(lod.proxyVertices.end(), proxy.begin(), proxy.end());
    const auto sourcePositionCount = static_cast<Index>(positions.size());
    for (const auto& vertex : proxy) positions.push_back(vertex.position);
    // Both variants have the same open outline, so one rim measurement suffices.
    const auto endIndex = sourcePositionCount;
    const std::vector<Edge> proxyRim{{endIndex, endIndex + 1}, {endIndex + 1, endIndex + 2},
                                    {endIndex + 2, endIndex + 3}, {endIndex + 3, endIndex}};
    const float rimError = rimDistance(border, proxyRim, positions, samples);
    const auto fitBounds = boundsOf(proxy, {0, 1, 2, 3, 4});
    group.proxyBounds = groupIndices[fixedLodFourTriangleLevel].empty()
        ? fitBounds : mergeBounds(group.proxyBounds, fitBounds);

    for (std::size_t level = fixedLodFourTriangleLevel; level < lod.levels.size(); ++level)
    {
        Indices indices = level == fixedLodFourTriangleLevel
            ? Indices{0, 4, 1, 1, 4, 2, 2, 4, 3, 3, 4, 0} : Indices{0, 2, 1, 0, 3, 2};
        if (reverse)
        {
            for (std::size_t i = 0; i < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
        }
        const auto coarse = surface(proxy, indices);
        float area = 0;
        for (const auto& triangle : coarse) area += dot(triangle.normal, normal) * 0.5f;
        auto& range = group.levels[level];
        range.sourceToLevel = std::max(range.sourceToLevel, sampledDistance(reference, coarse, samples));
        range.levelToSource = std::max(range.levelToSource, sampledDistance(coarse, reference, samples));
        range.rimDeviation = std::max(range.rimDeviation, rimError);
        auto& result = stats[level];
        result.changedRims += 1;
        result.rimEdges += proxyRim.size();
        result.sourceToLevel = std::max(result.sourceToLevel, range.sourceToLevel);
        result.levelToSource = std::max(result.levelToSource, range.levelToSource);
        result.rimDeviation = std::max(result.rimDeviation, rimError);
        result.planarAreaErrorPercent = std::max(result.planarAreaErrorPercent,
                                                  std::abs(area / sourceArea - 1.0f) * 100.0f);
        for (auto i : indices) groupIndices[level].push_back(firstVertex + i);
    }
}

void bakeComponent(const MeshData& mesh, const Component& component, const Recipe& recipe,
                   std::span<const Index> representatives, FixedLod& lod,
                   std::array<Indices, fixedLodLevelCount>& groupIndices, FixedLodGroup& group,
                   std::array<LevelStats, fixedLodLevelCount>& stats)
{
    auto input = component.indices;
    if (!representatives.empty())
        for (auto& index : input) index = representatives[index];
    const auto vertices = remapIndices(input);
    std::vector<XMFLOAT3> positions;
    positions.reserve(vertices.size());
    const bool useUv = recipe.uvWeight > 0 && (mesh.uvMask & (1u << recipe.textureCoordinateSet));
    std::vector<XMFLOAT2> uvs;
    if (useUv) uvs.reserve(vertices.size());
    for (auto index : vertices)
    {
        const auto& vertex = mesh.vertices[index];
        positions.push_back(vertex.position);
        if (useUv) uvs.push_back(vertex.texcoords[recipe.textureCoordinateSet]);
    }
    const std::array uvWeights{recipe.uvWeight, recipe.uvWeight};
    // A valid source chart can acquire zero-area UV faces even with weighted attributes.
    const bool preserveUvArea = useUv && !hasCollapsedUvTriangle(input, uvs);
    const auto border = borderEdges(input);
    const auto anchors = border.empty() ? Edge{} : rimAnchors(border, vertices, positions, mesh, recipe.useUvEnds);
    const auto anchorSpan = XMVectorGetX(XMVector3Length(XMLoadFloat3(&positions[anchors.first]) -
                                                        XMLoadFloat3(&positions[anchors.second])));
    std::vector<unsigned char> locks(vertices.size());
    locks[anchors.first] = locks[anchors.second] = meshopt_SimplifyVertex_Lock;
    const auto reference = surface(mesh.vertices, component.indices);
    const auto toSourceIndices = [&](const Indices& local) {
        Indices global;
        global.reserve(local.size());
        for (auto index : local) global.push_back(vertices[index]);
        return global;
    };
    const auto hasRimAnchors = [&](const std::vector<Edge>& edges) {
        const auto contains = [&](Index anchor) {
            return std::ranges::any_of(edges, [&](const auto& edge) {
                return edge.first == anchor || edge.second == anchor;
            });
        };
        return contains(anchors.first) && contains(anchors.second);
    };
    const auto withinRefinementError = [&](const Indices& candidate, float errorLimit, float rimLimit) {
        const auto rim = borderEdges(candidate);
        if (rim != border && (!hasRimAnchors(rim) ||
                             rimDistance(border, rim, positions, recipe.samples) > std::min(rimLimit, errorLimit)))
        {
            return false;
        }
        const auto geometry = surface(mesh.vertices, toSourceIndices(candidate));
        return std::ranges::none_of(geometry, [](const auto& triangle) { return triangle.normalSquared == 0.0f; }) &&
            sampledDistance(reference, geometry, recipe.samples) <= errorLimit &&
            sampledDistance(geometry, reference, recipe.samples) <= errorLimit;
    };
    Indices previous = input;
    auto previousBorder = border;
    float previousRimError = 0;
    float previousLibraryError = 0;
    auto& fullDetail = groupIndices.front();
    fullDetail.insert(fullDetail.end(), component.indices.begin(), component.indices.end());
    for (std::size_t level = 1; level < fixedLodIndicesOnlyCount; ++level)
    {
        const auto standardTarget = std::max<std::size_t>(1, input.size() / 3 * recipe.targets[level] / 100) * 3;
        auto target = standardTarget;
        if (level == fixedLodFarLevel && recipe.farComponentTarget)
            target = std::min(target, static_cast<std::size_t>(recipe.farComponentTarget) * 3);
        const bool refine = level == fixedLodFarLevel && recipe.farComponentErrorLimit && target < standardTarget;
        const bool simplifyRim = level >= fixedLodRimLevel;
        const float rimLimit = anchorSpan * static_cast<float>(recipe.rimTolerancePercent[level]) / 100.0f;
        auto& result = stats[level];
        Indices candidate = previous;
        float libraryError = previousLibraryError;
        if (target < previous.size() && (!simplifyRim || rimLimit > 0))
        {
            const unsigned options = meshopt_SimplifyErrorAbsolute | (simplifyRim ? 0 : meshopt_SimplifyLockBorder);
            Indices simplified(input.size());
            bool uvLimited = false;
            // A dense component can have thousands of possible triangle budgets.
            const auto retryStep = input.size() < 3072 ? 3 : std::max<std::size_t>(3, (previous.size() - target) / 36 * 3);
            for (auto budget = target; budget < previous.size();)
            {
                float error;
                simplified.resize(meshopt_simplifyWithAttributes(
                    simplified.data(), input.data(), input.size(), &positions.front().x, positions.size(), sizeof(XMFLOAT3),
                    useUv ? &uvs.front().x : nullptr, sizeof(XMFLOAT2), uvWeights.data(), useUv ? uvWeights.size() : 0,
                    simplifyRim ? locks.data() : nullptr, budget, std::numeric_limits<float>::max(), options, &error));
                const bool validUv = !preserveUvArea || !hasCollapsedUvTriangle(simplified, uvs);
                const bool refining = refine && budget < standardTarget;
                if (validUv && (!refining || (!simplified.empty() && simplified.size() < previous.size() &&
                    error <= *recipe.farComponentErrorLimit &&
                    withinRefinementError(simplified, *recipe.farComponentErrorLimit, rimLimit))))
                {
                    candidate = std::move(simplified);
                    libraryError = error;
                    break;
                }
                uvLimited |= !validUv;
                // Try a few more triangles before falling back to this component's regular LOD 4.
                budget = refining ? std::min(standardTarget, budget + 3) :
                    std::max(budget, simplified.size()) + retryStep;
                simplified.resize(input.size());
            }
            result.uvLimited += uvLimited;
        }
        auto global = toSourceIndices(candidate);
        auto coarse = surface(mesh.vertices, global);
        const bool reusePrevious = candidate == previous;
        auto candidateBorder = reusePrevious ? previousBorder : borderEdges(candidate);
        float rimError = reusePrevious ? previousRimError : 0.0f;
        bool validBorder = reusePrevious || candidateBorder == border;
        if (simplifyRim && !validBorder)
        {
            if (hasRimAnchors(candidateBorder))
            {
                rimError = rimDistance(border, candidateBorder, positions, recipe.samples);
                validBorder = rimError <= rimLimit;
            }
        }
        if (candidate.empty() || candidate.size() > previous.size() || !validBorder ||
            (simplifyRim && !reusePrevious && candidate.size() == previous.size() && candidateBorder != border) ||
            std::any_of(coarse.begin(), coarse.end(), [](const auto& triangle) { return triangle.normalSquared == 0; }))
        {
            ++result.rejected;
            candidate = previous;
            global = toSourceIndices(candidate);
            coarse = surface(mesh.vertices, global);
            libraryError = previousLibraryError;
            candidateBorder = previousBorder;
            rimError = previousRimError;
        }
        if (level == 1)
        {
            stats.front().rimEdges += border.size();
        }
        result.targetMisses += candidate.size() > target;
        result.unchanged += candidate.size() == previous.size();
        result.changedRims += candidateBorder != border;
        result.rimEdges += candidateBorder.size();
        previous = std::move(candidate);
        previousBorder = std::move(candidateBorder);
        previousRimError = rimError;
        previousLibraryError = libraryError;

        auto& range = group.levels[level];
        if (previous != input)
        {
            range.sourceToLevel = std::max(range.sourceToLevel, sampledDistance(reference, coarse, recipe.samples));
            range.levelToSource = std::max(range.levelToSource, sampledDistance(coarse, reference, recipe.samples));
        }
        range.simplifierError = std::max(range.simplifierError, libraryError);
        range.rimDeviation = std::max(range.rimDeviation, rimError);
        result.sourceToLevel = std::max(result.sourceToLevel, range.sourceToLevel);
        result.levelToSource = std::max(result.levelToSource, range.levelToSource);
        result.simplifierError = std::max(result.simplifierError, libraryError);
        result.rimDeviation = std::max(result.rimDeviation, rimError);
        auto& indices = groupIndices[level];
        indices.insert(indices.end(), global.begin(), global.end());
    }
    if (recipe.useLeafProxies)
    {
        if (previous.size() < 12)
        {
            throw std::runtime_error("Leaf proxy recipe needs at least four triangles per component at LOD 4.");
        }
        const auto uvAnchors = recipe.useUvEnds || border.empty() ? anchors
            : rimAnchors(border, vertices, positions, mesh, true);
        bakeLeafProxies(mesh, vertices, std::move(positions), border, uvAnchors, reference, recipe.samples,
                        lod, groupIndices, group, stats);
    }
}

std::vector<Edge> geometricBorder(std::span<const Vertex> vertices, std::span<const Index> indices)
{
    // Normal and texture splits do not create a new geometric rim.
    std::map<std::array<float, 3>, Index> firstAtPosition;
    Indices shared;
    shared.reserve(indices.size());
    for (auto index : indices)
    {
        const auto& p = vertices[index].position;
        shared.push_back(firstAtPosition.try_emplace({p.x, p.y, p.z}, index).first->second);
    }
    return borderEdges(shared);
}

void measureTransitions(const MeshData& mesh, const FixedLod& lod, const Recipe& recipe,
                        const std::array<Indices, fixedLodLevelCount>& groupIndices,
                        const std::array<std::size_t, fixedLodLevelCount>& componentStarts, TransitionTable& transitions)
{
    struct Geometry
    {
        std::span<const Vertex> vertices;
        std::span<const Index> indices;
        std::vector<SurfaceTriangle> triangles;
        std::vector<Edge> rim;
    };
    std::array<Geometry, fixedLodLevelCount> levels;
    const auto firstLevel = *recipe.firstTransitionLevel;
    for (std::size_t level = firstLevel; level < lod.levels.size(); ++level)
    {
        auto& geometry = levels[level];
        geometry.vertices = level < fixedLodIndicesOnlyCount ? std::span(mesh.vertices) : std::span(lod.proxyVertices);
        geometry.indices = std::span(groupIndices[level]).subspan(componentStarts[level]);
        geometry.triangles = surface(geometry.vertices, geometry.indices);
        if (!recipe.weldNormalSplits)
            geometry.rim = geometricBorder(geometry.vertices, geometry.indices);
    }
    for (std::size_t fineLevel = firstLevel; fineLevel + 1 < lod.levels.size(); ++fineLevel)
    {
        const auto& fine = levels[fineLevel];
        for (std::size_t coarseLevel = fineLevel + 1; coarseLevel < lod.levels.size(); ++coarseLevel)
        {
            const auto& coarse = levels[coarseLevel];
            auto& pair = transitions[fineLevel][coarseLevel];
            const bool sameTriangles = fine.vertices.data() == coarse.vertices.data() &&
                                       std::ranges::equal(fine.indices, coarse.indices);
            if (!sameTriangles)
            {
                pair.fineToCoarse = std::max(pair.fineToCoarse,
                    sampledDistance(fine.triangles, coarse.triangles, lod.sampleSubdivisions));
                pair.coarseToFine = std::max(pair.coarseToFine,
                    sampledDistance(coarse.triangles, fine.triangles, lod.sampleSubdivisions));
            }
            if (fine.rim.empty() != coarse.rim.empty())
            {
                ++pair.unmatchedRims;
            }
            else if (!fine.rim.empty())
            {
                ++pair.comparableRims;
                if (sameTriangles) continue;
                const auto finePosition = [&](Index i) { return XMLoadFloat3(&fine.vertices[i].position); };
                const auto coarsePosition = [&](Index i) { return XMLoadFloat3(&coarse.vertices[i].position); };
                pair.rimDeviation = std::max(pair.rimDeviation,
                    rimDistance(fine.rim, coarse.rim, finePosition, coarsePosition, lod.sampleSubdivisions));
            }
        }
    }
}

void packClusters(FixedLodLevel& level, FixedLodGroupLevel& range, const Indices& indices, unsigned cap)
{
    auto local = indices;
    const auto vertices = remapIndices(local);
    const auto sizes = scanMeshletIndexCounts(local, vertices.size(), cap);
    range.firstCluster = static_cast<Index>(level.clusters.size());
    range.clusterCount = static_cast<Index>(sizes.size());
    auto firstIndex = static_cast<Index>(level.indices.size());
    std::size_t packed = 0;
    for (auto count : sizes)
    {
        level.clusters.push_back({firstIndex, count});
        firstIndex += count;
        packed += count;
    }
    if (packed != indices.size()) throw std::runtime_error("Cluster packing did not cover the triangle stream.");
    level.indices.insert(level.indices.end(), indices.begin(), indices.end());
}

void storeSharedLevel(const MeshData& mesh, const Recipe& recipe, FixedLod& lod, std::size_t level,
                      const Indices& indices, float simplifierError, LevelStats& stats, TransitionTable& transitions)
{
    const auto geometry = surface(mesh.vertices, indices);
    if (std::ranges::any_of(geometry, [](const auto& triangle) { return triangle.normalSquared == 0.0f; }))
        throw std::runtime_error("The shared level contains degenerate triangles.");

    for (std::size_t other = 0; other < lod.levels.size(); ++other)
    {
        if (other == level) continue;
        const auto fine = std::min(other, level);
        const auto coarse = std::max(other, level);
        if (fine != 0 && (!recipe.firstTransitionLevel || fine < *recipe.firstTransitionLevel)) continue;
        const auto reference = surface(mesh.vertices, lod.levels[other].indices);
        const auto& from = fine == level ? geometry : reference;
        const auto& to = fine == level ? reference : geometry;
        auto& pair = transitions[fine][coarse];
        pair.fineToCoarse = sampledDistance(from, to, recipe.samples);
        pair.coarseToFine = sampledDistance(to, from, recipe.samples);
    }
    const auto& source = transitions[0][level];
    auto& error = lod.groups.front().levels[level];
    stats = {};
    error.sourceToLevel = stats.sourceToLevel = source.fineToCoarse;
    error.levelToSource = stats.levelToSource = source.coarseToFine;
    error.simplifierError = stats.simplifierError = simplifierError;
    error.rimDeviation = 0.0f;
    lod.levels[level] = {};
    packClusters(lod.levels[level], error, indices, recipe.triangleCap);
}

void appendSharedCoarseLevel(const MeshData& mesh, const Recipe& recipe, FixedLod& lod,
                            LevelStats& stats, TransitionTable& transitions)
{
    auto input = lod.levels.back().indices;
    const auto target = static_cast<std::size_t>(recipe.sharedCoarseTarget) * 3;
    if (target >= input.size()) throw std::runtime_error("The shared coarse target must reduce the preceding level.");

    const auto mapping = remapIndices(input);
    std::vector<XMFLOAT3> positions;
    positions.reserve(mapping.size());
    for (auto index : mapping) positions.push_back(mesh.vertices[index].position);
    Indices reduced(input.size());
    float simplifierError = 0.0f;
    reduced.resize(meshopt_simplifySloppy(reduced.data(), input.data(), input.size(), &positions.front().x,
        positions.size(), sizeof(XMFLOAT3), nullptr, target, 1.0f, &simplifierError));
    if (reduced.empty() || reduced.size() > target)
        throw std::runtime_error("The shared coarse simplifier could not meet the triangle budget.");
    for (auto& index : reduced) index = mapping[index];
    auto merged = lod.groups.front();
    for (const auto& group : std::span(lod.groups).subspan(1))
    {
        merged.bounds = mergeBounds(merged.bounds, group.bounds);
        merged.componentCount += group.componentCount;
        for (std::size_t i = 0; i < lod.levels.size(); ++i)
        {
            auto& range = merged.levels[i];
            const auto& child = group.levels[i];
            range.clusterCount += child.clusterCount;
            range.sourceToLevel = std::max(range.sourceToLevel, child.sourceToLevel);
            range.levelToSource = std::max(range.levelToSource, child.levelToSource);
            range.simplifierError = std::max(range.simplifierError, child.simplifierError);
            range.rimDeviation = std::max(range.rimDeviation, child.rimDeviation);
        }
    }

    lod.groups.assign(1, merged);
    lod.componentsPerGroup = std::max(lod.componentsPerGroup, merged.componentCount);
    lod.levels.emplace_back();
    const float scale = meshopt_simplifyScale(&positions.front().x, positions.size(), sizeof(XMFLOAT3));
    storeSharedLevel(mesh, recipe, lod, fixedLodSharedCoarseLevel, reduced, simplifierError * scale, stats, transitions);
}

void simplifySharedFarLevel(const MeshData& mesh, const Recipe& recipe, FixedLod& lod,
                            LevelStats& stats, TransitionTable& transitions)
{
    auto input = lod.levels[fixedLodFarLevel].indices;
    const auto target = static_cast<std::size_t>(recipe.sharedFarTarget) * 3;
    if (target >= input.size()) throw std::runtime_error("The shared far target must reduce the existing LOD 4.");

    const auto mapping = remapIndices(input);
    std::vector<Vertex> vertices;
    vertices.reserve(mapping.size());
    for (const auto id : mapping) vertices.push_back(mesh.vertices[id]);
    const float uvWeight = recipe.sharedFarUvWeight.value_or(recipe.uvWeight);
    const bool useUv = uvWeight > 0 && (mesh.uvMask & (1u << recipe.textureCoordinateSet));
    const auto* uv = useUv ? &vertices.front().texcoords[recipe.textureCoordinateSet].x : nullptr;
    const std::array uvWeights{uvWeight, uvWeight};
    Indices reduced(input.size());
    float simplifierError = 0.0f;
    reduced.resize(meshopt_simplifyWithAttributes(reduced.data(), input.data(), input.size(), &vertices.front().position.x,
        vertices.size(), sizeof(Vertex), uv, sizeof(Vertex), uvWeights.data(), useUv ? uvWeights.size() : 0, nullptr,
        target, std::numeric_limits<float>::max(), meshopt_SimplifyErrorAbsolute, &simplifierError));
    if (reduced.size() <= lod.levels[fixedLodSharedCoarseLevel].indices.size() || reduced.size() > target)
        throw std::runtime_error("The shared far simplifier could not reach the target above the coarse level.");

    if (useUv)
    {
        std::vector<XMFLOAT2> uvs;
        uvs.reserve(vertices.size());
        for (const auto& vertex : vertices) uvs.push_back(vertex.texcoords[recipe.textureCoordinateSet]);
        if (!hasCollapsedUvTriangle(input, uvs) && hasCollapsedUvTriangle(reduced, uvs))
            throw std::runtime_error("The shared far level collapsed a UV triangle.");
    }
    for (auto& index : reduced) index = mapping[index];
    const auto prior = lod.groups.front().levels[fixedLodFarLevel];
    storeSharedLevel(mesh, recipe, lod, fixedLodFarLevel, reduced, simplifierError, stats, transitions);
    if (std::max({stats.sourceToLevel, stats.levelToSource, stats.simplifierError}) >
        std::max({prior.surfaceError(), prior.rimDeviation, prior.simplifierError}))
    {
        throw std::runtime_error("The shared far level increased LOD 4's measured selection error.");
    }
}

struct LocalCandidate
{
    Indices indices;
    float sourceError = 0;
    float referenceError = 0;
    std::uint32_t tint;
    bool fitted = false;
};

struct LocalSurface
{
    std::span<const Vertex> vertices;
    std::vector<SurfaceTriangle> triangles;
    std::vector<Edge> rim;
};

LocalSurface localSurface(std::span<const Vertex> vertices, const Indices& indices, bool measureRim)
{
    return {vertices, surface(vertices, indices), measureRim ? geometricBorder(vertices, indices) : std::vector<Edge>{}};
}

float localError(const LocalSurface& a, const LocalSurface& b, unsigned samples)
{
    float error = std::max(sampledDistance(a.triangles, b.triangles, samples),
                           sampledDistance(b.triangles, a.triangles, samples));
    if (!a.rim.empty() && !b.rim.empty())
    {
        const auto positionA = [&](Index i) { return XMLoadFloat3(&a.vertices[i].position); };
        const auto positionB = [&](Index i) { return XMLoadFloat3(&b.vertices[i].position); };
        error = std::max(error, rimDistance(a.rim, b.rim, positionA, positionB, samples));
    }
    return error;
}

Indices reduceLocal(const MeshData& mesh, const Recipe& recipe, const Indices& input,
                    std::size_t target, bool leaf)
{
    if (input.size() <= target * 3) return input;
    auto indices = input;
    const auto mapping = remapIndices(indices);
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT2> uvs;
    for (auto i : mapping)
    {
        positions.push_back(mesh.vertices[i].position);
        uvs.push_back(mesh.vertices[i].texcoords[recipe.textureCoordinateSet]);
    }
    std::vector<unsigned char> locks(mapping.size());
    const auto border = leaf ? borderEdges(indices) : std::vector<Edge>{};
    if (!border.empty())
    {
        const auto anchors = rimAnchors(border, mapping, positions, mesh, recipe.useUvEnds);
        locks[anchors.first] = locks[anchors.second] = meshopt_SimplifyVertex_Lock;
    }
    const std::array weights{recipe.uvWeight, recipe.uvWeight};
    const bool useUv = recipe.uvWeight > 0 && (mesh.uvMask & (1u << recipe.textureCoordinateSet));
    Indices out(indices.size());
    out.resize(meshopt_simplifyWithAttributes(out.data(), indices.data(), indices.size(), &positions.front().x,
        positions.size(), sizeof(XMFLOAT3), useUv ? &uvs.front().x : nullptr, sizeof(XMFLOAT2), weights.data(),
        useUv ? weights.size() : 0, locks.data(), target * 3, std::numeric_limits<float>::max(),
        meshopt_SimplifyErrorAbsolute, nullptr));
    if (out.empty() || out.size() >= input.size() ||
        (useUv && !hasCollapsedUvTriangle(indices, uvs) && hasCollapsedUvTriangle(out, uvs))) return input;
    for (auto& index : out) index = mapping[index];
    if (std::ranges::any_of(surface(mesh.vertices, out), [](const auto& face) { return face.normalSquared == 0; }))
        return input;
    return out;
}

void addLocalPart(LocalCandidate& cut, const Indices& indices, std::span<const Vertex> vertices,
                  const LocalSurface& source, const LocalSurface& reference, const Recipe& recipe, bool leaf)
{
    if (indices.empty()) return;
    const auto geometry = localSurface(vertices, indices, leaf);
    cut.sourceError = std::max(cut.sourceError, localError(source, geometry, recipe.samples));
    if (cut.tint != fixedLodFarLevel)
        cut.referenceError = std::max(cut.referenceError, localError(reference, geometry, recipe.samples));
    cut.indices.insert(cut.indices.end(), indices.begin(), indices.end());
}

void appendLocalCandidate(LocalCandidate& group, const LocalCandidate& leaf)
{
    group.sourceError = std::max(group.sourceError, leaf.sourceError);
    group.referenceError = std::max(group.referenceError, leaf.referenceError);
    group.indices.insert(group.indices.end(), leaf.indices.begin(), leaf.indices.end());
}

// Explore an additional sequence of edge collapses on the small LOD 4 leaf.
// Score the completed outline and surface at each step. This lets the baker
// remove interior tessellation or a shallow rim bend before sacrificing a lobe.
std::vector<Indices> reduceLeafOutline(const MeshData& mesh, const Recipe& recipe, const Indices& base,
                                       const LocalSurface& reference,
                                       const std::array<leaf_outline::Projection, leaf_outline::directions>& views,
                                       bool optimizeCoverage)
{
    std::vector<Indices> sequence;
    if (base.size() > 64 * 3 || reference.rim.empty()) return sequence;
    auto current = base;
    const auto vertices = remapIndices(current);
    std::vector<XMFLOAT3> positions;
    for (auto index : vertices) positions.push_back(mesh.vertices[index].position);
    const auto anchors = rimAnchors(borderEdges(current), vertices, positions, mesh, recipe.useUvEnds);
    const auto referenceMask = optimizeCoverage ? leaf_outline::raster(reference.triangles, views, 2) : leaf_outline::Mask{};
    const auto uvArea = [&](Index a, Index b, Index c) {
        const auto uv = [&](Index i) { return mesh.vertices[vertices[i]].texcoords[recipe.textureCoordinateSet]; };
        const auto p = uv(a), q = uv(b), r = uv(c);
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    while (current.size() > 3)
    {
        std::map<Edge, unsigned> edges;
        std::vector<std::vector<bool>> neighbor(vertices.size(), std::vector<bool>(vertices.size()));
        for (std::size_t i = 0; i < current.size(); i += 3)
            for (unsigned j = 0; j < 3; ++j)
            {
                const auto a = current[i+j], b = current[i+(j+1)%3];
                ++edges[std::minmax(a,b)];
                neighbor[a][b] = neighbor[b][a] = true;
            }
        std::vector<bool> rim(vertices.size());
        for (auto [edge, uses] : edges)
            if (uses == 1) rim[edge.first] = rim[edge.second] = true;
        float best = std::numeric_limits<float>::infinity();
        unsigned bestPixels = std::numeric_limits<unsigned>::max();
        Indices bestCut;
        for (const auto& [edge, uses] : edges)
        {
            unsigned common = 0;
            for (std::size_t i = 0; i < vertices.size(); ++i)
                common += neighbor[edge.first][i] && neighbor[edge.second][i];
            if (common != uses || uses > 2) continue;
            for (auto from : {edge.first, edge.second})
            {
                const auto to = from == edge.first ? edge.second : edge.first;
                if (from == anchors.first || from == anchors.second || (rim[from] && uses != 1)) continue;
                Indices next, global;
                bool valid = true;
                for (std::size_t i = 0; i < current.size(); i += 3)
                {
                    std::array<Index,3> face{current[i],current[i+1],current[i+2]};
                    if (std::ranges::find(face, from) != face.end())
                    {
                        if (std::ranges::find(face, to) != face.end()) continue;
                        const float before = uvArea(face[0],face[1],face[2]);
                        const auto normal = [&](const auto& f) {
                            const auto a = XMLoadFloat3(&positions[f[0]]);
                            return XMVector3Cross(XMLoadFloat3(&positions[f[1]])-a, XMLoadFloat3(&positions[f[2]])-a);
                        };
                        const auto oldNormal = normal(face);
                        std::ranges::replace(face, from, to);
                        if (before * uvArea(face[0],face[1],face[2]) <= 0 ||
                            XMVectorGetX(XMVector3Dot(oldNormal,normal(face))) <= 0) { valid = false; break; }
                    }
                    next.insert(next.end(),face.begin(),face.end());
                    for (auto v : face) global.push_back(vertices[v]);
                }
                if (!valid || next.empty()) continue;
                const auto candidate = localSurface(mesh.vertices, global, true);
                const unsigned pixels = optimizeCoverage ? leaf_outline::difference(referenceMask,
                    leaf_outline::raster(candidate.triangles, views, 2)) : 0;
                if (pixels > bestPixels) continue;
                const float score = localError(reference, candidate, 2);
                if (pixels < bestPixels || score < best || (score == best && next.size() < bestCut.size()))
                {
                    bestPixels = pixels;
                    best = score;
                    bestCut = std::move(next);
                }
            }
        }
        if (bestCut.empty()) break;
        current = std::move(bestCut);
        auto& reduced = sequence.emplace_back();
        for (auto id : current) reduced.push_back(vertices[id]);
    }
    return sequence;
}

std::vector<LocalCandidate> bakeLeafGroup(const MeshData& mesh, const Recipe& recipe, const FixedLod& lod,
                                         std::span<const std::array<Indices, fixedLodLevelCount>> parts,
                                         std::span<const Index> group)
{
    constexpr std::array fractions{0.12f, 0.24f, 0.36f, 0.48f, 0.60f, 0.72f, 0.84f, 0.91f, 0.96f};
    std::vector<LocalCandidate> cuts{{{},0,0,3}, {{},0,0,4}, {{},0,0,6,true}};
    constexpr std::array tints{7u, 7u, 7u, 8u, 8u, 9u, 9u, 9u, 9u};
    for (unsigned set = 0; set < 2; ++set)
        for (auto tint : tints) cuts.push_back({{}, 0, 0, tint});
    constexpr std::array coveragePermille{2u, 5u, 10u, 20u};
    const auto fineFirst = cuts.size();
    for (std::size_t i = 0; i < coveragePermille.size(); ++i) cuts.push_back({{}, 0, 0, 7});
    for (auto id : group)
    {
        const auto& part = parts[id];
        const auto source = localSurface(mesh.vertices, part[0], true);
        const auto reference = localSurface(mesh.vertices, part[4], true);
        LocalCandidate baseCut{{}, 0, 0, 4};
        LocalCandidate fitted{{}, 0, 0, 6, true};
        addLocalPart(cuts[0], part[3], mesh.vertices, source, reference, recipe, true);
        addLocalPart(baseCut, part[4], mesh.vertices, source, reference, recipe, true);
        addLocalPart(fitted, part[6], lod.proxyVertices, source, reference, recipe, true);
        appendLocalCandidate(cuts[1], baseCut);
        appendLocalCandidate(cuts[2], fitted);
        const auto views = leaf_outline::projections(source.triangles);
        const auto sourceMask = leaf_outline::raster(source.triangles, views);
        const auto referenceMask = leaf_outline::raster(reference.triangles, views);
        struct Choice { LocalCandidate cut; unsigned sourcePixels, referencePixels; };
        std::vector<Choice> choices{{baseCut, leaf_outline::difference(sourceMask, referenceMask), 0}};
        const auto consider = [&](const Indices& indices) {
            if (indices.size() >= part[4].size() || std::ranges::any_of(choices,
                [&](const auto& c) { return c.cut.indices == indices; })) return;
            LocalCandidate cut{{}, 0, 0, 7};
            addLocalPart(cut, indices, mesh.vertices, source, reference, recipe, true);
            const auto mask = leaf_outline::raster(surface(mesh.vertices, indices), views);
            choices.push_back({std::move(cut), leaf_outline::difference(mask, sourceMask),
                               leaf_outline::difference(mask, referenceMask)});
        };
        for (unsigned target : {4u,6u,8u,10u,12u,14u,16u,18u,20u,24u,28u,32u})
            consider(reduceLocal(mesh, recipe, part[4], target, true));
        const auto previousCount = choices.size();
        for (bool coverage : {false, true})
            for (const auto& indices : reduceLeafOutline(mesh, recipe, part[4], reference, views, coverage))
                consider(indices);
        unsigned areaPixels = 0;
        for (auto word : referenceMask) areaPixels += std::popcount(word);
        for (std::size_t j = 0; j < coveragePermille.size(); ++j)
        {
            const unsigned allowance = areaPixels * coveragePermille[j] / 1000;
            const auto* best = &choices.front();
            for (const auto& c : choices)
                if (c.referencePixels <= allowance && c.sourcePixels <= choices.front().sourcePixels + allowance &&
                    c.cut.indices.size() < best->cut.indices.size()) best = &c;
            appendLocalCandidate(cuts[fineFirst + j], best->cut);
        }
        const float start = baseCut.sourceError;
        const float end = std::max({start, fitted.sourceError, fitted.referenceError});
        for (std::size_t j = 0; j < fractions.size(); ++j)
        {
            const float limit = std::lerp(start, end, fractions[j]);
            const auto* baseline = &choices.front();
            for (const auto& c : std::span(choices).first(previousCount))
                if (std::max(c.cut.sourceError, c.cut.referenceError) <= limit &&
                    c.cut.indices.size() < baseline->cut.indices.size()) baseline = &c;
            appendLocalCandidate(cuts[3 + j], baseline->cut);
            const auto* best = baseline;
            for (const auto& c : choices)
            {
                if (c.sourcePixels > baseline->sourcePixels || c.referencePixels > baseline->referencePixels ||
                    c.cut.indices.size() >= best->cut.indices.size()) continue;
                best = &c;
            }
            auto& groupCut = cuts[3 + fractions.size() + j];
            appendLocalCandidate(groupCut, best->cut);
            // The replacement must also wait until its visual control was eligible.
            groupCut.sourceError = std::max(groupCut.sourceError, baseline->cut.sourceError);
            groupCut.referenceError = std::max(groupCut.referenceError, baseline->cut.referenceError);
        }
    }
    return cuts;
}

void bakeLocalSprayGroups(const MeshData& mesh, const Recipe& recipe, const std::vector<Component>& components,
                          std::span<const Index> representatives, FixedLod& lod)
{
    // A shared legacy cut can join twigs. Keep the components it joins in the same local group.
    Indices parent(components.size());
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](Index id) {
        while (parent[id] != id)
        {
            parent[id] = parent[parent[id]];
            id = parent[id];
        }
        return id;
    };
    constexpr auto unassigned = std::numeric_limits<Index>::max();
    Indices owner(mesh.vertices.size(), unassigned);
    const auto vertexKey = [&](Index index) { return representatives.empty() ? index : representatives[index]; };
    for (Index id = 0; id < components.size(); ++id)
        for (auto index : components[id].indices)
        {
            auto& entry = owner[vertexKey(index)];
            if (entry == unassigned) entry = id;
            else parent[root(entry)] = root(id);
        }
    for (std::size_t level = 1; level < lod.levels.size() && !lod.usesProxyVertices(level); ++level)
    {
        const auto& indices = lod.levels[level].indices;
        for (std::size_t i = 0; i < indices.size(); i += 3)
        {
            const auto group = root(owner[vertexKey(indices[i])]);
            parent[root(owner[vertexKey(indices[i + 1])])] = group;
            parent[root(owner[vertexKey(indices[i + 2])])] = group;
        }
    }

    std::vector<Component> merged;
    Indices idByRoot(components.size(), unassigned);
    for (Index id = 0; id < components.size(); ++id)
    {
        auto& joined = idByRoot[root(id)];
        if (joined == unassigned)
        {
            joined = static_cast<Index>(merged.size());
            merged.push_back(components[id]);
        }
        else
        {
            auto& part = merged[joined];
            part.indices.insert(part.indices.end(), components[id].indices.begin(), components[id].indices.end());
            part.bounds = mergeBounds(part.bounds, components[id].bounds);
        }
    }
    std::vector<std::array<Indices, fixedLodLevelCount>> parts(merged.size());
    for (std::size_t level = 0; level < lod.levels.size(); ++level)
    {
        const auto& indices = lod.levels[level].indices;
        for (std::size_t i = 0; i < indices.size(); i += 3)
        {
            const auto source = lod.usesProxyVertices(level) ? lod.components[indices[i] / fixedLodProxyVerticesPerComponent]
                                                          : owner[vertexKey(indices[i])];
            auto& part = parts[idByRoot[root(source)]][level];
            part.insert(part.end(), indices.begin() + i, indices.begin() + i + 3);
        }
    }
    Indices ids(merged.size());
    std::iota(ids.begin(), ids.end(), 0u);
    std::vector<std::span<const Index>> groups;
    partitionGroups(ids, std::min(ids.size(), static_cast<std::size_t>(recipe.sprayGroupCount)), merged, groups);
    for (const auto group : groups)
    {
        std::vector<LocalCandidate> cuts;
        const auto add = [&](std::uint32_t tint, bool fitted = false) {
            cuts.push_back({{}, 0, 0, tint, fitted});
        };
        if (recipe.useLeafProxies)
        {
            cuts = bakeLeafGroup(mesh, recipe, lod, parts, group);
        }
        else
        {
            std::array<Indices, fixedLodLevelCount> combined;
            for (auto id : group)
                for (std::size_t i = 0; i < lod.levels.size(); ++i)
                    combined[i].insert(combined[i].end(), parts[id][i].begin(), parts[id][i].end());
            if (combined[4].empty())
                throw std::runtime_error("A local twig group lost its entire LOD 4 reference.");
            const auto source = localSurface(mesh.vertices, combined[0], false);
            const auto reference = localSurface(mesh.vertices, combined[4], false);
            for (std::uint32_t i = 0; i < lod.levels.size(); ++i)
            {
                add(i);
                addLocalPart(cuts.back(), combined[i], mesh.vertices, source, reference, recipe, false);
            }
            for (std::uint32_t i = 3; i <= 5; ++i)
            {
                add(i + 4);
                const auto reduced = reduceLocal(mesh, recipe, combined[i], std::max<std::size_t>(1, combined[i].size() / 6), false);
                addLocalPart(cuts.back(), reduced, mesh.vertices, source, reference, recipe, false);
            }
            add(8);
            const auto milder = reduceLocal(mesh, recipe, combined[4], std::max<std::size_t>(1, combined[4].size() / 4), false);
            addLocalPart(cuts.back(), milder, mesh.vertices, source, reference, recipe, false);
        }
        std::stable_sort(cuts.begin(), cuts.end(), [](const auto& a, const auto& b) {
            return a.indices.size() < b.indices.size();
        });
        auto& output = lod.sprayGroups.emplace_back();
        for (const auto& cut : cuts)
        {
            if (cut.indices.empty()) continue;
            if (!output.empty())
            {
                const auto& previous = output.back();
                if (cut.sourceError == previous.sourceError && cut.referenceError == previous.referenceError &&
                    cut.fitted == (previous.fittedVertices != 0) && cut.indices.size() == previous.range.indexCount &&
                    std::ranges::equal(cut.indices, std::span(lod.sprayGroupIndices).subspan(previous.range.firstIndex,
                                                                                          previous.range.indexCount)))
                    continue;
            }
            const auto first = static_cast<Index>(lod.sprayGroupIndices.size());
            lod.sprayGroupIndices.insert(lod.sprayGroupIndices.end(), cut.indices.begin(), cut.indices.end());
            output.push_back({{first, static_cast<Index>(cut.indices.size())}, cut.sourceError, cut.referenceError,
                              cut.tint, cut.fitted});
        }
    }
}

void bakeWholeTwigCuts(const MeshData& mesh, const Recipe& recipe, FixedLod& lod)
{
    using namespace surface_distance;
    const auto& base = lod.levels[fixedLodFarLevel].indices;
    const auto& coarse = lod.levels.back().indices;
    const auto reference = localSurface(mesh.vertices, base, false);
    const auto source = localSurface(mesh.vertices, lod.levels[0].indices, false);
    const SurfaceSearch referenceSearch(reference.triangles);
    const float extent = meshopt_simplifyScale(&mesh.vertices.front().position.x, mesh.vertices.size(), sizeof(Vertex));
    if (!(extent > 0)) return;
    std::vector<LocalCandidate> candidates;
    const auto consider = [&](const Indices& indices) {
        if (indices.empty() || indices.size() >= base.size() ||
            std::ranges::any_of(candidates, [&](const auto& item) { return item.indices == indices; })) return;
        LocalCandidate cut{{}, 0, 0, 9};
        addLocalPart(cut, indices, mesh.vertices, source, reference, recipe, false);
        candidates.push_back(std::move(cut));
    };
    for (float fraction : {0.85f, 0.70f, 0.55f, 0.40f, 0.30f, 0.23f})
        consider(reduceLocal(mesh, recipe, base, static_cast<std::size_t>(base.size() / 3 * fraction), false));

    // Measure the complete spray. A piece of retained wood can cover a nearby face from a different group.
    for (const auto* input : {&base, &coarse})
    {
        for (const float cellFraction : {0.010f, 0.018f, 0.028f})
        {
            Indices simplified(input->size());
            simplified.resize(meshopt_simplifySloppy(simplified.data(), input->data(), input->size(),
                &mesh.vertices.front().position.x, mesh.vertices.size(), sizeof(Vertex), nullptr, 3, cellFraction, nullptr));
            if (simplified.empty()) continue;
            const auto faces = surface(mesh.vertices, simplified);
            for (const float limitFraction : {0.004f, 0.007f, 0.011f, 0.016f, 0.023f})
            {
                const float limit = extent * limitFraction;
                const float limitSquared = limit * limit;
                Indices rebuilt;
                std::vector<SurfaceTriangle> accepted;
                for (std::size_t i = 0; i < faces.size(); ++i)
                {
                    if (!(sampledDistanceSquared(faces[i], referenceSearch, recipe.samples) <= limitSquared)) continue;
                    accepted.push_back(faces[i]);
                    const auto first = simplified.begin() + i * 3;
                    rebuilt.insert(rebuilt.end(), first, first + 3);
                }
                if (accepted.empty()) continue;
                const SurfaceSearch proxySearch(accepted);
                for (std::size_t i = 0; i < reference.triangles.size(); ++i)
                {
                    if (sampledDistanceSquared(reference.triangles[i], proxySearch, recipe.samples) <= limitSquared) continue;
                    const auto first = base.begin() + i * 3;
                    rebuilt.insert(rebuilt.end(), first, first + 3);
                }
                consider(rebuilt);
            }
        }
    }
    std::ranges::sort(candidates, {}, [](const auto& item) { return item.indices.size(); });
    float bestSource = std::numeric_limits<float>::infinity();
    float bestReference = bestSource;
    for (const auto& cut : candidates)
    {
        if (cut.sourceError >= bestSource && cut.referenceError >= bestReference) continue;
        if (lod.wholeSprayCuts.size() == maxWholeSprayCuts) break;
        bestSource = std::min(bestSource, cut.sourceError);
        bestReference = std::min(bestReference, cut.referenceError);
        const auto first = static_cast<Index>(lod.sprayGroupIndices.size());
        lod.sprayGroupIndices.insert(lod.sprayGroupIndices.end(), cut.indices.begin(), cut.indices.end());
        lod.wholeSprayCuts.push_back({{first, static_cast<Index>(cut.indices.size())}, cut.sourceError,
                                    cut.referenceError, cut.tint, 0});
        std::cout << "Whole twig cut " << cut.indices.size() / 3 << " triangles, source " << cut.sourceError
                  << ", LOD 4 " << cut.referenceError << '\n';
    }
}

FixedLod bake(const MeshData& mesh, Index meshIndex, const Recipe& recipe,
             std::array<LevelStats, fixedLodLevelCount>& stats, TransitionTable& transitions)
{
    const auto representatives = recipe.weldNormalSplits ? vertexRepresentatives(mesh) : Indices{};
    const auto components = findComponents(mesh, representatives);
    FixedLod lod{};
    lod.meshIndex = meshIndex;
    lod.triangleCap = recipe.triangleCap;
    lod.componentsPerGroup = recipe.groupSize;
    lod.sampleSubdivisions = recipe.samples;
    lod.levels.resize(recipe.useLeafProxies ? fixedLodLevelCount : fixedLodIndicesOnlyCount);
    if (recipe.useLeafProxies)
    {
        const auto vertexCount = components.size() * fixedLodProxyVerticesPerComponent;
        if (vertexCount > std::numeric_limits<std::uint32_t>::max() / sizeof(Vertex))
        {
            throw std::runtime_error("Leaf proxy vertex buffer is too large.");
        }
        lod.proxyVertices.reserve(vertexCount);
    }
    lod.components.resize(components.size());
    std::iota(lod.components.begin(), lod.components.end(), 0u);
    std::vector<std::span<const Index>> groups;
    partitionGroups(lod.components, (components.size() + recipe.groupSize - 1) / recipe.groupSize, components, groups);
    for (const auto ids : groups)
    {
        FixedLodGroup group{};
        group.componentCount = static_cast<Index>(ids.size());
        group.bounds = components[ids.front()].bounds;
        std::array<Indices, fixedLodLevelCount> groupIndices;
        for (auto id : ids)
        {
            group.bounds = mergeBounds(group.bounds, components[id].bounds);
            std::array<std::size_t, fixedLodLevelCount> componentStarts{};
            if (recipe.firstTransitionLevel)
            {
                for (std::size_t level = *recipe.firstTransitionLevel; level < lod.levels.size(); ++level)
                    componentStarts[level] = groupIndices[level].size();
            }
            bakeComponent(mesh, components[id], recipe, representatives, lod, groupIndices, group, stats);
            if (recipe.firstTransitionLevel)
                measureTransitions(mesh, lod, recipe, groupIndices, componentStarts, transitions);
        }
        if (recipe.useLeafProxies) group.proxyBounds = mergeBounds(group.bounds, group.proxyBounds);
        for (std::size_t level = 0; level < lod.levels.size(); ++level)
        {
            packClusters(lod.levels[level], group.levels[level], groupIndices[level], recipe.triangleCap);
        }
        lod.groups.push_back(group);
    }
    if (recipe.sharedCoarseTarget)
        appendSharedCoarseLevel(mesh, recipe, lod, stats[fixedLodSharedCoarseLevel], transitions);
    // Build the coarse level from the original LOD 4 so the far optimization cannot change it.
    if (recipe.sharedFarTarget)
        simplifySharedFarLevel(mesh, recipe, lod, stats[fixedLodFarLevel], transitions);
    if (recipe.sprayGroupCount)
    {
        bakeLocalSprayGroups(mesh, recipe, components, representatives, lod);
        if (recipe.sharedCoarseTarget) bakeWholeTwigCuts(mesh, recipe, lod);
    }
    return lod;
}

void report(std::ostream& out, const MeshData& mesh, const Recipe& recipe, const FixedLod& lod,
            std::uint64_t fingerprint, const std::array<LevelStats, fixedLodLevelCount>& stats,
            const TransitionTable& transitions, double milliseconds, std::uintmax_t bytes)
{
    out << "Fixed LOD bake (meshoptimizer 1.3)\nMesh: " << mesh.name
        << "\nGLB fingerprint (FNV-1a): " << std::hex << fingerprint << std::dec
        << "\nComponents: " << lod.components.size() << " across " << lod.groups.size()
        << " groups (cap " << lod.componentsPerGroup << ')'
        << "\nTriangle cap: " << recipe.triangleCap << ", cluster vertex cap: 256"
        << "\nSampling: " << recipe.samples << " subdivisions per triangle edge"
        << "\nUV simplification: TEXCOORD_" << recipe.textureCoordinateSet
        << ", weight per coordinate " << ((mesh.uvMask & (1u << recipe.textureCoordinateSet)) ? recipe.uvWeight : 0.0f)
        << "\nRim anchors: " << (recipe.useUvEnds ? "UV V-end midpoints" : "farthest rim pair")
        << "\nWeld normal splits for reduced levels: " << (recipe.weldNormalSplits ? "yes" : "no")
        << "\nBake time: " << milliseconds << " ms, payload: " << bytes << " bytes\n";
    for (std::size_t i = 0; i < lod.levels.size(); ++i)
    {
        const auto& level = lod.levels[i];
        auto indices = level.indices;
        const auto usedVertices = remapIndices(indices).size();
        const auto triangles = level.indices.size() / 3;
        const auto occupancy = 100.0 * triangles / (level.clusters.size() * recipe.triangleCap);
        const auto& s = stats[i];
        out << "\nLOD " << i << " (";
        const auto sharedTarget = i == fixedLodSharedCoarseLevel ? recipe.sharedCoarseTarget :
            i == fixedLodFarLevel ? recipe.sharedFarTarget : 0;
        if (sharedTarget)
        {
            out << "shared target " << sharedTarget << " triangles";
            if (i == fixedLodFarLevel && recipe.sharedFarUvWeight)
                out << ", UV weight " << *recipe.sharedFarUvWeight;
        }
        else if (lod.usesProxyVertices(i))
        {
            out << (i == fixedLodFourTriangleLevel ? 4 : 2) << " triangles per component, fitted area";
        }
        else
        {
            out << "target " << recipe.targets[i] << "% of component triangles";
            if (i == fixedLodFarLevel && recipe.farComponentTarget)
                out << ", " << recipe.farComponentTarget << " triangles per component";
            if (i == fixedLodFarLevel && recipe.farComponentErrorLimit)
                out << ", refinement error limit " << *recipe.farComponentErrorLimit;
            if (i >= fixedLodRimLevel)
            {
                out << ", sampled rim limit " << recipe.rimTolerancePercent[i] << "% of anchor span";
            }
        }
        out << ")\n  " << triangles << " triangles, " << usedVertices << " referenced vertices, "
            << level.clusters.size() << " clusters, " << std::fixed << std::setprecision(2) << occupancy
            << "% mean triangle occupancy\n";
        if (!sharedTarget)
            out << "  " << s.targetMisses << " components above target, "
                << s.unchanged << " unchanged from preceding level, " << s.rejected << " rejected candidates"
                << "\n  " << s.rimEdges << " rim edges, " << s.changedRims << " components with changed rims"
                << "\n  " << s.uvLimited << " components retried to preserve UV area\n";
        out << "  Max estimated error (mesh-local units): " << std::setprecision(7) << s.sourceToLevel
            << " source-to-level, " << s.levelToSource << " level-to-source, "
            << s.rimDeviation << " rim deviation\n";
        if (lod.usesProxyVertices(i))
        {
            out << "  Max leaf projected area error in fitting direction: " << s.planarAreaErrorPercent << "%\n";
        }
        else
        {
            out << "  Max simplifier cost estimate: " << s.simplifierError << '\n';
        }
    }
    if (recipe.firstTransitionLevel)
    {
        out << "\nDirect LOD transitions (maximum sampled distances in mesh-local units)\n"
            << "Each component is measured against its geometry at the other level.\n";
        if (recipe.weldNormalSplits)
            out << "Welded geometry uses surface distances. Open edges can move as seams and joints simplify.\n";
        else
            out << "Rim distance uses open edges after joining coincident positions within each component.\n"
                << "Unmatched rims count components with open edges at only one of the two levels.\n";
        if (recipe.sharedCoarseTarget)
            out << "Pairs with a shared level sample the whole mesh in both directions.\n";
        out << "\nFine  Coarse   Surface fine->coarse   Surface coarse->fine";
        if (!recipe.weldNormalSplits) out << "   Rim symmetric   Rim pairs   Unmatched rims";
        out << '\n' << std::fixed << std::setprecision(7);
        for (std::size_t fine = *recipe.firstTransitionLevel; fine + 1 < lod.levels.size(); ++fine)
        {
            for (std::size_t coarse = fine + 1; coarse < lod.levels.size(); ++coarse)
            {
                const auto& pair = transitions[fine][coarse];
                out << std::setw(4) << fine << std::setw(8) << coarse << std::setw(23) << pair.fineToCoarse
                    << std::setw(23) << pair.coarseToFine;
                if (!recipe.weldNormalSplits)
                {
                    out << std::setw(16);
                    if (pair.comparableRims) out << pair.rimDeviation;
                    else out << "n/a";
                    out << std::setw(12) << pair.comparableRims << std::setw(17) << pair.unmatchedRims;
                }
                out << '\n';
            }
        }
    }
}

unsigned optionValue(std::wstring_view value, unsigned maximum)
{
    const auto string = std::wstring(value);
    std::size_t consumed;
    const auto result = std::stoul(string, &consumed);
    if (!result || result > maximum || consumed != value.size())
    {
        throw std::runtime_error("Bake option must be a positive integer within its supported range.");
    }
    return result;
}

float nonnegativeValue(std::wstring_view value, const char* option)
{
    std::size_t consumed;
    const float result = std::stof(std::wstring(value), &consumed);
    if (!std::isfinite(result) || result < 0 || consumed != value.size())
    {
        throw std::runtime_error(std::string(option) + " must be finite and nonnegative.");
    }
    return result;
}
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        if (argc < 4 || (argc - 4) % 2 != 0)
        {
            throw std::runtime_error("Usage: BakeFixedLod source.glb \"mesh name\" output.fixedlod "
                                     "[--triangle-cap N] [--group-size N] [--samples N] [--lod1 N] [--lod2 N] "
                                     "[--lod3 N] [--lod4 N] [--lod4-component-triangles N] [--lod4-refinement-error F] "
                                     "[--rim-anchors uv-v|diameter] "
                                     "[--rim-tolerance N] [--far-rim-tolerance N] [--uv-weight N] [--leaf-proxies uv-v] "
                                     "[--weld-normal-splits on] [--transition-min-lod N] [--shared-coarse-triangles N] "
                                     "[--shared-far-triangles N] [--shared-far-uv-weight F] [--spray-groups N]");
        }
        Recipe recipe;
        bool hasFarTarget = false;
        for (int i = 4; i < argc; i += 2)
        {
            const std::wstring_view option = argv[i];
            if (option == L"--triangle-cap") recipe.triangleCap = optionValue(argv[i + 1], 512);
            else if (option == L"--group-size") recipe.groupSize = optionValue(argv[i + 1], 1024);
            else if (option == L"--spray-groups") recipe.sprayGroupCount = optionValue(argv[i + 1], maxSprayLodGroups);
            else if (option == L"--samples") recipe.samples = optionValue(argv[i + 1], 16);
            else if (option == L"--lod1") recipe.targets[1] = optionValue(argv[i + 1], 100);
            else if (option == L"--lod2") recipe.targets[2] = optionValue(argv[i + 1], 100);
            else if (option == L"--lod3") recipe.targets[fixedLodRimLevel] = optionValue(argv[i + 1], 100);
            else if (option == L"--lod4")
            {
                recipe.targets[fixedLodFarLevel] = optionValue(argv[i + 1], 100);
                hasFarTarget = true;
            }
            else if (option == L"--lod4-component-triangles")
                recipe.farComponentTarget = optionValue(argv[i + 1], std::numeric_limits<Index>::max() / 3);
            else if (option == L"--lod4-refinement-error")
                recipe.farComponentErrorLimit = nonnegativeValue(argv[i + 1], "LOD 4 refinement error");
            else if (option == L"--rim-tolerance")
                recipe.rimTolerancePercent[fixedLodRimLevel] = optionValue(argv[i + 1], 100);
            else if (option == L"--far-rim-tolerance")
                recipe.rimTolerancePercent[fixedLodFarLevel] = optionValue(argv[i + 1], 100);
            else if (option == L"--uv-weight") recipe.uvWeight = nonnegativeValue(argv[i + 1], "UV weight");
            else if (option == L"--leaf-proxies" && std::wstring_view(argv[i + 1]) == L"uv-v")
                recipe.useLeafProxies = true;
            else if (option == L"--weld-normal-splits" && std::wstring_view(argv[i + 1]) == L"on")
                recipe.weldNormalSplits = true;
            else if (option == L"--transition-min-lod")
            {
                const std::wstring_view level = argv[i + 1];
                recipe.firstTransitionLevel = level == L"0" ? 0u : optionValue(level, fixedLodLevelCount - 2);
            }
            else if (option == L"--shared-coarse-triangles")
                recipe.sharedCoarseTarget = optionValue(argv[i + 1], std::numeric_limits<Index>::max() / 3);
            else if (option == L"--shared-far-triangles")
                recipe.sharedFarTarget = optionValue(argv[i + 1], std::numeric_limits<Index>::max() / 3);
            else if (option == L"--shared-far-uv-weight")
                recipe.sharedFarUvWeight = nonnegativeValue(argv[i + 1], "Shared far UV weight");
            else if (option == L"--rim-anchors")
            {
                const std::wstring_view mode = argv[i + 1];
                if (mode == L"uv-v") recipe.useUvEnds = true;
                else if (mode == L"diameter") recipe.useUvEnds = false;
                else throw std::runtime_error("Rim anchors must be uv-v or diameter.");
            }
            else throw std::runtime_error("Unknown bake option.");
        }
        if (!hasFarTarget)
        {
            auto& farTarget = recipe.targets[fixedLodFarLevel];
            farTarget = std::min(farTarget, recipe.targets[fixedLodRimLevel]);
        }
        if (!std::is_sorted(recipe.targets.rbegin(), recipe.targets.rend()))
        {
            throw std::runtime_error("LOD targets must decrease or stay equal from one level to the next.");
        }
        if (recipe.sharedCoarseTarget && recipe.useLeafProxies)
            throw std::runtime_error("The shared index-only level and fitted leaf proxies are alternate bake recipes.");
        if (recipe.sharedFarTarget && (!recipe.weldNormalSplits || !recipe.sharedCoarseTarget ||
                                      recipe.sharedFarTarget <= recipe.sharedCoarseTarget))
        {
            throw std::runtime_error("The shared far target needs welded geometry and a smaller shared coarse target.");
        }
        if (recipe.sharedFarUvWeight && !recipe.sharedFarTarget)
            throw std::runtime_error("A shared far UV weight requires a shared far target.");
        if (recipe.sprayGroupCount && !recipe.useLeafProxies && !recipe.sharedCoarseTarget)
            throw std::runtime_error("Spray groups need fitted leaf proxies or a shared coarse twig level.");
        if (recipe.farComponentErrorLimit && !recipe.farComponentTarget)
            throw std::runtime_error("LOD 4 refinement needs a per-component triangle target.");
        const auto levels = recipe.useLeafProxies ? fixedLodLevelCount :
            recipe.sharedCoarseTarget ? fixedLodSharedCoarseLevelCount : fixedLodIndicesOnlyCount;
        if (recipe.firstTransitionLevel && *recipe.firstTransitionLevel + 1 >= levels)
            throw std::runtime_error("Transition analysis needs at least two baked levels.");
        const std::filesystem::path source = argv[1];
        const std::filesystem::path output = argv[3];
        if (output.extension() != L".fixedlod") throw std::runtime_error("Output must have a .fixedlod extension.");
        const auto asset = loadGlb(source);
        const auto name = std::filesystem::path(argv[2]).u8string();
        const std::string_view meshName(reinterpret_cast<const char*>(name.data()), name.size());
        const auto mesh = std::find_if(asset.meshes.begin(), asset.meshes.end(),
                                       [&](const MeshData& m) { return m.name == meshName; });
        if (mesh == asset.meshes.end()) throw std::runtime_error("The named source mesh was not found.");
        if (recipe.useUvEnds && !(mesh->uvMask & 1)) throw std::runtime_error("UV rim anchors need TEXCOORD_0.");
        recipe.textureCoordinateSet = asset.materials[mesh->material].textureCoordinateSet;
        if (recipe.useLeafProxies && (!(mesh->uvMask & 1) || recipe.textureCoordinateSet != 0))
        {
            throw std::runtime_error("UV-V leaf proxies need TEXCOORD_0 for the material.");
        }
        if (std::count_if(asset.meshes.begin(), asset.meshes.end(),
                          [&](const MeshData& m) { return m.name == meshName; }) != 1)
        {
            throw std::runtime_error("The source mesh name must be unique.");
        }

        const auto fingerprint = fixedLodSourceFingerprint(source);
        const auto started = std::chrono::steady_clock::now();
        std::array<LevelStats, fixedLodLevelCount> stats{};
        TransitionTable transitions{};
        const auto lod = bake(*mesh, static_cast<Index>(mesh - asset.meshes.begin()), recipe, stats, transitions);
        const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
        auto temporary = output;
        temporary += L".tmp";
        writeFixedLod(temporary, fingerprint, lod);
        auto reportPath = output;
        reportPath.replace_extension(L".fixedlod.txt");
        std::ofstream reportFile(reportPath, std::ios::trunc);
        reportFile.exceptions(std::ios::failbit | std::ios::badbit);
        const auto bytes = std::filesystem::file_size(temporary);
        report(reportFile, *mesh, recipe, lod, fingerprint, stats, transitions, milliseconds.count(), bytes);
        reportFile.close();
        std::filesystem::rename(temporary, output);
        report(std::cout, *mesh, recipe, lod, fingerprint, stats, transitions, milliseconds.count(), bytes);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "BakeFixedLod: " << e.what() << '\n';
        return 1;
    }
}
