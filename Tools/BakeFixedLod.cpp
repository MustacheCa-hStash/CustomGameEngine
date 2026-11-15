#include "FixedLod.h"
#include "GltfLoader.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace DirectX;

namespace
{
using Index = std::uint32_t;
using Indices = std::vector<Index>;
using Edge = std::pair<Index, Index>;

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

std::vector<Component> findComponents(const MeshData& mesh)
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
            const auto [edge, inserted] = firstTriangle.try_emplace(
                std::minmax(corners[corner], corners[(corner + 1) % 3]), triangle);
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

std::vector<Edge> borderEdges(const Indices& indices)
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

struct SurfaceTriangle
{
    XMVECTOR a, ab, ac, normal;
    float normalSquared;
};

std::vector<SurfaceTriangle> surface(std::span<const Vertex> vertices, const Indices& indices)
{
    std::vector<SurfaceTriangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i < indices.size(); i += 3)
    {
        const auto a = XMLoadFloat3(&vertices[indices[i]].position);
        const auto ab = XMLoadFloat3(&vertices[indices[i + 1]].position) - a;
        const auto ac = XMLoadFloat3(&vertices[indices[i + 2]].position) - a;
        const auto normal = XMVector3Cross(ab, ac);
        triangles.push_back({a, ab, ac, normal, XMVectorGetX(XMVector3LengthSq(normal))});
    }
    return triangles;
}

float segmentDistanceSquared(FXMVECTOR point, FXMVECTOR edge)
{
    const float t = std::clamp(XMVectorGetX(XMVector3Dot(point, edge)) /
                              XMVectorGetX(XMVector3LengthSq(edge)), 0.0f, 1.0f);
    return XMVectorGetX(XMVector3LengthSq(point - t * edge));
}

float rimDistance(const std::vector<Edge>& i, const std::vector<Edge>& j,
                  const std::vector<XMFLOAT3>& positions, unsigned subdivisions)
{
    const auto directed = [&](const std::vector<Edge>& from, const std::vector<Edge>& to) {
        float worst = 0;
        for (const auto& [a, b] : from)
        {
            const auto first = XMLoadFloat3(&positions[a]);
            const auto last = XMLoadFloat3(&positions[b]);
            for (unsigned sample = 0; sample <= subdivisions; ++sample)
            {
                const auto point = XMVectorLerp(first, last,
                                                static_cast<float>(sample) / static_cast<float>(subdivisions));
                float nearest = std::numeric_limits<float>::infinity();
                for (const auto& [c, d] : to)
                {
                    const auto start = XMLoadFloat3(&positions[c]);
                    nearest = std::min(nearest, segmentDistanceSquared(point - start,
                                                                       XMLoadFloat3(&positions[d]) - start));
                }
                worst = std::max(worst, nearest);
            }
        }
        return worst;
    };
    return std::sqrt(std::max(directed(i, j), directed(j, i)));
}

float triangleDistanceSquared(FXMVECTOR sample, const SurfaceTriangle& t)
{
    const auto relative = sample - t.a;
    const float b = XMVectorGetX(XMVector3Dot(XMVector3Cross(relative, t.ac), t.normal));
    const float c = XMVectorGetX(XMVector3Dot(XMVector3Cross(t.ab, relative), t.normal));
    if (b >= 0 && c >= 0 && b + c <= t.normalSquared)
    {
        const float height = XMVectorGetX(XMVector3Dot(relative, t.normal));
        return height * height / t.normalSquared;
    }
    return std::min({segmentDistanceSquared(relative, t.ab), segmentDistanceSquared(relative, t.ac),
                     segmentDistanceSquared(relative - t.ab, t.ac - t.ab)});
}

float sampledDistance(const std::vector<SurfaceTriangle>& from, const std::vector<SurfaceTriangle>& to,
                      unsigned subdivisions)
{
    float worst = 0;
    const auto step = 1.0f / static_cast<float>(subdivisions);
    for (const auto& triangle : from)
    {
        for (unsigned u = 0; u <= subdivisions; ++u)
        {
            for (unsigned v = 0; v <= subdivisions - u; ++v)
            {
                const auto p = triangle.a + (static_cast<float>(u) * step) * triangle.ab +
                               (static_cast<float>(v) * step) * triangle.ac;
                float nearest = std::numeric_limits<float>::infinity();
                for (const auto& target : to) nearest = std::min(nearest, triangleDistanceSquared(p, target));
                worst = std::max(worst, nearest);
            }
        }
    }
    return std::sqrt(worst);
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

void bakeComponent(const MeshData& mesh, const Component& component, const Recipe& recipe, FixedLod& lod,
                   std::array<Indices, fixedLodLevelCount>& groupIndices, FixedLodGroup& group,
                   std::array<LevelStats, fixedLodLevelCount>& stats)
{
    auto input = component.indices;
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
    Indices previous = input;
    auto previousBorder = border;
    float previousRimError = 0;
    float previousLibraryError = 0;
    auto& fullDetail = groupIndices.front();
    fullDetail.insert(fullDetail.end(), component.indices.begin(), component.indices.end());
    for (std::size_t level = 1; level < fixedLodIndicesOnlyCount; ++level)
    {
        const auto target = std::max<std::size_t>(1, input.size() / 3 * recipe.targets[level] / 100) * 3;
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
            for (auto budget = target; budget < previous.size();)
            {
                float error;
                simplified.resize(meshopt_simplifyWithAttributes(
                    simplified.data(), input.data(), input.size(), &positions.front().x, positions.size(), sizeof(XMFLOAT3),
                    useUv ? &uvs.front().x : nullptr, sizeof(XMFLOAT2), uvWeights.data(), useUv ? uvWeights.size() : 0,
                    simplifyRim ? locks.data() : nullptr, budget, std::numeric_limits<float>::max(), options, &error));
                if (!preserveUvArea || !hasCollapsedUvTriangle(simplified, uvs))
                {
                    candidate = std::move(simplified);
                    libraryError = error;
                    break;
                }
                uvLimited = true;
                budget = std::max(budget, simplified.size()) + 3;
                simplified.resize(input.size());
            }
            result.uvLimited += uvLimited;
        }
        auto toSourceIndices = [&](const Indices& local) {
            Indices global;
            global.reserve(local.size());
            for (auto index : local) global.push_back(vertices[index]);
            return global;
        };
        auto global = toSourceIndices(candidate);
        auto coarse = surface(mesh.vertices, global);
        const bool reusePrevious = candidate == previous;
        auto candidateBorder = reusePrevious ? previousBorder : borderEdges(candidate);
        float rimError = reusePrevious ? previousRimError : 0.0f;
        bool validBorder = reusePrevious || candidateBorder == border;
        if (simplifyRim && !validBorder)
        {
            const auto onRim = [&](Index anchor) {
                return std::any_of(candidateBorder.begin(), candidateBorder.end(), [&](const auto& edge) {
                    return edge.first == anchor || edge.second == anchor;
                });
            };
            if (onRim(anchors.first) && onRim(anchors.second))
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

void packClusters(FixedLodLevel& level, FixedLodGroupLevel& range, const Indices& indices, unsigned cap)
{
    auto local = indices;
    const auto vertices = remapIndices(local);
    constexpr unsigned vertexCap = 256;
    std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(indices.size(), vertexCap, cap));
    Indices meshletVertices(indices.size());
    std::vector<unsigned char> meshletTriangles(indices.size());
    const auto count = meshopt_buildMeshletsScan(meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
                                               local.data(), local.size(), vertices.size(), vertexCap, cap);
    range.firstCluster = static_cast<Index>(level.clusters.size());
    range.clusterCount = static_cast<Index>(count);
    const auto firstIndex = level.indices.size();
    for (const auto& cluster : std::span(meshlets).first(count))
    {
        const auto indexCount = cluster.triangle_count * 3;
        level.clusters.push_back({static_cast<Index>(level.indices.size()), indexCount});
        for (std::size_t i = 0; i < indexCount; ++i)
        {
            const auto vertex = meshletTriangles[cluster.triangle_offset + i];
            level.indices.push_back(vertices[meshletVertices[cluster.vertex_offset + vertex]]);
        }
    }
    if (level.indices.size() - firstIndex != indices.size() ||
        !std::equal(indices.begin(), indices.end(), level.indices.begin() + firstIndex))
    {
        throw std::runtime_error("Cluster packing changed the triangle stream.");
    }
}

FixedLod bake(const MeshData& mesh, Index meshIndex, const Recipe& recipe,
             std::array<LevelStats, fixedLodLevelCount>& stats)
{
    const auto components = findComponents(mesh);
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
            bakeComponent(mesh, components[id], recipe, lod, groupIndices, group, stats);
        }
        if (recipe.useLeafProxies) group.proxyBounds = mergeBounds(group.bounds, group.proxyBounds);
        for (std::size_t level = 0; level < lod.levels.size(); ++level)
        {
            packClusters(lod.levels[level], group.levels[level], groupIndices[level], recipe.triangleCap);
        }
        lod.groups.push_back(group);
    }
    return lod;
}

void report(std::ostream& out, const MeshData& mesh, const Recipe& recipe, const FixedLod& lod,
            std::uint64_t fingerprint, const std::array<LevelStats, fixedLodLevelCount>& stats, double milliseconds,
            std::uintmax_t bytes)
{
    out << "Fixed LOD bake (meshoptimizer 1.3)\nMesh: " << mesh.name
        << "\nGLB fingerprint (FNV-1a): " << std::hex << fingerprint << std::dec
        << "\nComponents: " << lod.components.size() << " across " << lod.groups.size()
        << " groups (cap " << recipe.groupSize << ')'
        << "\nTriangle cap: " << recipe.triangleCap << ", cluster vertex cap: 256"
        << "\nSampling: " << recipe.samples << " subdivisions per triangle edge"
        << "\nUV simplification: TEXCOORD_" << recipe.textureCoordinateSet
        << ", weight per coordinate " << ((mesh.uvMask & (1u << recipe.textureCoordinateSet)) ? recipe.uvWeight : 0.0f)
        << "\nRim anchors: " << (recipe.useUvEnds ? "UV V-end midpoints" : "farthest rim pair")
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
        if (i >= fixedLodIndicesOnlyCount)
        {
            out << (i == fixedLodFourTriangleLevel ? 4 : 2) << " triangles per component, fitted area";
        }
        else
        {
            out << "target " << recipe.targets[i] << "% of component triangles";
            if (i >= fixedLodRimLevel)
            {
                out << ", sampled rim limit " << recipe.rimTolerancePercent[i] << "% of anchor span";
            }
        }
        out << ")\n  " << triangles << " triangles, " << usedVertices << " referenced vertices, "
            << level.clusters.size() << " clusters, " << std::fixed << std::setprecision(2) << occupancy
            << "% mean triangle occupancy\n  " << s.targetMisses << " components above target, "
            << s.unchanged << " unchanged from preceding level, " << s.rejected << " rejected candidates"
            << "\n  " << s.rimEdges << " rim edges, " << s.changedRims << " components with changed rims"
            << "\n  " << s.uvLimited << " components retried to preserve UV area"
            << "\n  Max estimated error (mesh-local units): " << std::setprecision(7) << s.sourceToLevel
            << " source-to-level, " << s.levelToSource << " level-to-source, "
            << s.rimDeviation << " rim deviation\n";
        if (i >= fixedLodIndicesOnlyCount)
        {
            out << "  Max leaf projected area error in fitting direction: " << s.planarAreaErrorPercent << "%\n";
        }
        else
        {
            out << "  Max simplifier cost estimate: " << s.simplifierError << '\n';
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

float uvWeightValue(std::wstring_view value)
{
    std::size_t consumed;
    const float result = std::stof(std::wstring(value), &consumed);
    if (!std::isfinite(result) || result < 0 || consumed != value.size())
    {
        throw std::runtime_error("UV weight must be finite and nonnegative.");
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
                                     "[--lod3 N] [--lod4 N] [--rim-anchors uv-v|diameter] "
                                     "[--rim-tolerance N] [--far-rim-tolerance N] [--uv-weight N] [--leaf-proxies uv-v]");
        }
        Recipe recipe;
        bool hasFarTarget = false;
        for (int i = 4; i < argc; i += 2)
        {
            const std::wstring_view option = argv[i];
            if (option == L"--triangle-cap") recipe.triangleCap = optionValue(argv[i + 1], 512);
            else if (option == L"--group-size") recipe.groupSize = optionValue(argv[i + 1], 1024);
            else if (option == L"--samples") recipe.samples = optionValue(argv[i + 1], 16);
            else if (option == L"--lod1") recipe.targets[1] = optionValue(argv[i + 1], 100);
            else if (option == L"--lod2") recipe.targets[2] = optionValue(argv[i + 1], 100);
            else if (option == L"--lod3") recipe.targets[fixedLodRimLevel] = optionValue(argv[i + 1], 100);
            else if (option == L"--lod4")
            {
                recipe.targets[fixedLodFarLevel] = optionValue(argv[i + 1], 100);
                hasFarTarget = true;
            }
            else if (option == L"--rim-tolerance")
                recipe.rimTolerancePercent[fixedLodRimLevel] = optionValue(argv[i + 1], 100);
            else if (option == L"--far-rim-tolerance")
                recipe.rimTolerancePercent[fixedLodFarLevel] = optionValue(argv[i + 1], 100);
            else if (option == L"--uv-weight") recipe.uvWeight = uvWeightValue(argv[i + 1]);
            else if (option == L"--leaf-proxies" && std::wstring_view(argv[i + 1]) == L"uv-v")
                recipe.useLeafProxies = true;
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
        const auto lod = bake(*mesh, static_cast<Index>(mesh - asset.meshes.begin()), recipe, stats);
        const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
        auto temporary = output;
        temporary += L".tmp";
        writeFixedLod(temporary, fingerprint, lod);
        auto reportPath = output;
        reportPath.replace_extension(L".fixedlod.txt");
        std::ofstream reportFile(reportPath, std::ios::trunc);
        reportFile.exceptions(std::ios::failbit | std::ios::badbit);
        const auto bytes = std::filesystem::file_size(temporary);
        report(reportFile, *mesh, recipe, lod, fingerprint, stats, milliseconds.count(), bytes);
        reportFile.close();
        std::filesystem::rename(temporary, output);
        report(std::cout, *mesh, recipe, lod, fingerprint, stats, milliseconds.count(), bytes);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "BakeFixedLod: " << e.what() << '\n';
        return 1;
    }
}
