#include "GltfLoader.h"
#include "SourceFingerprint.h"
#include "SprayBough.h"
#include "SurfaceDistance.h"
#include "TreeHierarchy.h"
#include "TwigRepair.h"
#include "BranchConnectedCut.h"

#include <DirectXCollision.h>
#include <meshoptimizer.h>

#include <cmath>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

using namespace DirectX;

namespace
{
void compact(MeshData& mesh)
{
    std::vector<std::uint32_t> map(mesh.vertices.size(), std::numeric_limits<std::uint32_t>::max());
    std::vector<Vertex> vertices;
    for (auto& index : mesh.indices)
    {
        auto& remapped = map[index];
        if (remapped == std::numeric_limits<std::uint32_t>::max())
        {
            remapped = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back(mesh.vertices[index]);
        }
        index = remapped;
    }
    mesh.vertices = std::move(vertices);
}

std::vector<std::uint32_t> boughPlacements(const std::filesystem::path& list, std::uint64_t hash, std::size_t count)
{
    std::ifstream input(list);
    std::uint64_t expected;
    if (!(input >> std::hex >> expected >> std::dec) || hash != expected)
        throw std::runtime_error("Bough membership list does not match the source GLB.");
    std::vector<std::uint32_t> ids;
    for (std::uint32_t id; input >> id;) ids.push_back(id);
    if (!input.eof() || ids.empty() || ids.size() >= count || ids.back() >= count ||
        !std::ranges::is_sorted(ids) || std::ranges::adjacent_find(ids) != ids.end())
        throw std::runtime_error("Bough membership must contain sorted unique spray indices.");
    return ids;
}

MeshData expandCut(const MeshData& mesh, const MeshData& base, std::span<const std::uint32_t> placements,
                   float& error, float* scale = nullptr)
{
    if (base.vertices.size() * placements.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(Vertex) ||
        base.indices.size() * placements.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(std::uint32_t))
        throw std::runtime_error("Expanded spray bough exceeds the supported buffer size.");
    MeshData expanded;
    expanded.vertices.reserve(base.vertices.size() * placements.size());
    expanded.indices.reserve(base.indices.size() * placements.size());
    float maxScale = 0.0f;
    for (auto id : placements)
    {
        const auto matrix = (*mesh.instances)[id].matrix() * XMLoadFloat4x4(&mesh.localToWorld);
        XMVECTOR determinant;
        const auto normalMatrix = XMMatrixTranspose(XMMatrixInverse(&determinant, matrix));
        const bool mirrored = XMVectorGetX(determinant) < 0.0f;
        for (unsigned row = 0; row < 3; ++row)
        {
            float sum = 0.0f;
            for (unsigned col = 0; col < 3; ++col)
                sum += std::abs(XMVectorGetX(XMVector3Dot(matrix.r[row], matrix.r[col])));
            maxScale = std::max(maxScale, std::sqrt(sum));
        }
        const auto start = static_cast<std::uint32_t>(expanded.vertices.size());
        for (auto vertex : base.vertices)
        {
            XMStoreFloat3(&vertex.position, XMVector3TransformCoord(XMLoadFloat3(&vertex.position), matrix));
            XMStoreFloat3(&vertex.normal, XMVector3Normalize(
                XMVector3TransformNormal(XMLoadFloat3(&vertex.normal), normalMatrix)));
            expanded.vertices.push_back(vertex);
        }
        for (std::size_t i = 0; i < base.indices.size(); i += 3)
        {
            expanded.indices.push_back(start + base.indices[i]);
            expanded.indices.push_back(start + base.indices[i + (mirrored ? 2 : 1)]);
            expanded.indices.push_back(start + base.indices[i + (mirrored ? 1 : 2)]);
        }
    }
    error *= maxScale;
    if (scale) *scale = maxScale;
    return expanded;
}

MeshData expandPart(const MeshData& mesh, const FixedLod& lod, std::size_t level,
                    std::span<const std::uint32_t> placements, float& error, float* scale = nullptr)
{
    for (const auto& group : lod.groups)
    {
        const auto& stats = group.levels[level];
        error = std::max({error, stats.surfaceError(), stats.rimDeviation, stats.simplifierError});
    }
    MeshData base;
    base.vertices = lod.usesProxyVertices(level) ? lod.proxyVertices : mesh.vertices;
    base.indices = lod.levels[level].indices;
    compact(base);
    return expandCut(mesh, base, placements, error, scale);
}

float dot(FXMVECTOR a, FXMVECTOR b)
{
    return XMVectorGetX(XMVector3Dot(a, b));
}

float leafErrorSquared(const std::array<XMVECTOR, 4>& quad, const std::array<XMVECTOR, 3>& proxy)
{
    using namespace surface_distance;
    const auto originalA = triangle(quad[0], quad[1], quad[2]);
    const auto originalB = triangle(quad[0], quad[2], quad[3]);
    const auto reduced = triangle(proxy[0], proxy[1], proxy[2]);
    // Squared distance to a convex planar leaf or triangle is convex, so the vertices bound it.
    float error = 0.0f;
    for (const auto& point : quad)
        error = std::max(error, triangleDistanceSquared(point, reduced));
    for (const auto& point : proxy)
        error = std::max(error, std::min(triangleDistanceSquared(point, originalA),
                                         triangleDistanceSquared(point, originalB)));
    return error;
}

float reduceLeaves(MeshData& mesh, float limit, bool preserveArea)
{
    if (mesh.indices.size() % 6) throw std::runtime_error("Bough leaf reduction requires fitted two-triangle leaves.");
    std::vector<bool> seen(mesh.vertices.size());
    std::vector<std::uint32_t> reduced;
    reduced.reserve(mesh.indices.size());
    float maxError = 0.0f;
    double originalArea = 0;
    double reducedArea = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 6)
    {
        const auto face = std::span(mesh.indices).subspan(i, 6);
        const bool forward = face[2] == face[4];
        if (face[0] != face[3] || (!forward && face[1] != face[5]))
            throw std::runtime_error("Bough leaf triangles must share a diagonal.");
        const std::array ids{face[0], face[forward ? 1 : 2], face[forward ? 2 : 1], face[forward ? 5 : 4]};
        for (auto id : ids)
        {
            if (seen[id]) throw std::runtime_error("Fitted bough leaves must use independent vertices.");
            seen[id] = true;
        }
        const std::array quad{XMLoadFloat3(&mesh.vertices[ids[0]].position),
                              XMLoadFloat3(&mesh.vertices[ids[1]].position),
                              XMLoadFloat3(&mesh.vertices[ids[2]].position),
                              XMLoadFloat3(&mesh.vertices[ids[3]].position)};
        const auto diagonal = quad[2] - quad[0];
        const float length = XMVectorGetX(XMVector3Length(diagonal));
        if (!(length > 0)) throw std::runtime_error("Fitted bough leaves must have positive length.");
        const auto axis = diagonal / length;
        const auto side = quad[1] - quad[0];
        const auto across = XMVector3Normalize(side - axis * dot(side, axis));
        const float left = dot(side, across);
        const float right = dot(quad[0] - quad[3], across);
        const auto normal = XMVector3Cross(axis, across);
        if (!(left > 0 && right > 0) || std::abs(dot(quad[3] - quad[0], normal)) > 0.0001f)
            throw std::runtime_error("Bough leaf reduction requires convex planar leaves.");
        const float width = left + right;
        const float area = length * width * 0.5f;
        originalArea += area;

        std::array<XMVECTOR, 3> best{};
        std::size_t bestSide = 1;
        float bestError = std::numeric_limits<float>::infinity();
        for (const auto sideIndex : {std::size_t{1}, std::size_t{3}})
        {
            const auto direction = sideIndex == 1 ? across : -across;
            const float originalWidth = sideIndex == 1 ? left : right;
            // Make the surviving triangle span the full original area while keeping its longitudinal extent.
            for (int sample = 0; sample <= 8; ++sample)
            {
                const float shift = width * (0.15f + 0.075f * static_cast<float>(sample));
                const std::array candidate{quad[0] - shift * direction, quad[2] - shift * direction,
                                           quad[sideIndex] + (width - shift - originalWidth) * direction};
                const float error = leafErrorSquared(quad, candidate);
                if (error < bestError)
                {
                    best = candidate;
                    bestSide = sideIndex;
                    bestError = error;
                }
            }
        }
        if (bestError > limit * limit)
        {
            reduced.insert(reduced.end(), face.begin(), face.end());
            reducedArea += area;
            continue;
        }
        if (!preserveArea)
        {
            best = {quad[0], quad[2], quad[bestSide]};
            bestError = leafErrorSquared(quad, best);
        }
        XMStoreFloat3(&mesh.vertices[ids[0]].position, best[0]);
        XMStoreFloat3(&mesh.vertices[ids[2]].position, best[1]);
        XMStoreFloat3(&mesh.vertices[ids[bestSide]].position, best[2]);
        const auto chosen = face.subspan(bestSide == 1 ? 0 : 3, 3);
        reduced.insert(reduced.end(), chosen.begin(), chosen.end());
        reducedArea += preserveArea ? area : length * (bestSide == 1 ? left : right) * 0.5f;
        maxError = std::max(maxError, std::sqrt(bestError));
    }
    std::cout << "Bough leaves " << mesh.indices.size() / 3 << " -> " << reduced.size() / 3
              << " triangles, " << 100.0 * reducedArea / originalArea << "% leaf surface area retained\n";
    mesh.indices = std::move(reduced);
    compact(mesh);
    return maxError;
}

float parseDistance(const wchar_t* value)
{
    wchar_t* end = nullptr;
    const float distance = std::wcstof(value, &end);
    if (end == value || *end || !std::isfinite(distance) || distance <= 0.0f)
        throw std::runtime_error("Bough recipe distances must be positive.");
    return distance;
}

void limitWoodTransition(const MeshData& source, const std::vector<surface_distance::SurfaceTriangle>& reference,
                         std::vector<std::uint32_t>& reduced, float limit, std::size_t* repairStart = nullptr)
{
    using namespace surface_distance;
    constexpr unsigned subdivisions = 4;
    const float limitSquared = limit * limit;
    const auto candidates = surface(source.vertices, reduced);
    const SurfaceSearch referenceSearch(reference);
    std::vector<std::uint32_t> accepted;
    accepted.reserve(reduced.size());
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        if (sampledDistanceSquared(candidates[i], referenceSearch, subdivisions) > limitSquared) continue;
        const auto first = reduced.begin() + 3 * i;
        accepted.insert(accepted.end(), first, first + 3);
    }
    if (accepted.empty()) throw std::runtime_error("The wood transition allowance rejected every proxy triangle.");
    std::cout << "Rejected " << (reduced.size() - accepted.size()) / 3 << " wood triangles outside the source\n";
    reduced = std::move(accepted);
    const auto proxy = surface(source.vertices, reduced);
    const SurfaceSearch search(proxy);
    const auto initial = reduced.size();
    if (repairStart) *repairStart = initial;
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        if (sampledDistanceSquared(reference[i], search, subdivisions) <= limitSquared) continue;
        const auto first = source.indices.begin() + 3 * i;
        reduced.insert(reduced.end(), first, first + 3);
    }
    std::cout << "Restored " << (reduced.size() - initial) / 3 << " wood triangles outside the transition allowance\n";
}

Bounds mergedBounds(const Bounds& a, const BoundingBox& b)
{
    BoundingBox result;
    BoundingBox::CreateMerged(result, BoundingBox(a.center, a.halfExtent), b);
    return {result.Center, result.Extents};
}

float transformStretch(FXMMATRIX matrix)
{
    float stretch = 0;
    for (unsigned row = 0; row < 3; ++row)
    {
        float sum = 0;
        for (unsigned col = 0; col < 3; ++col)
            sum += std::abs(dot(matrix.r[row], matrix.r[col]));
        stretch = std::max(stretch, std::sqrt(sum));
    }
    return stretch;
}

void addGeometry(TreeLodPart& part, TreeLodNode& node, const MeshData& proxy, float error, float referenceError,
                 FXMMATRIX transform)
{
    const auto vertexCap = std::numeric_limits<std::uint32_t>::max() / sizeof(Vertex);
    const auto indexCap = std::numeric_limits<std::uint32_t>::max() / sizeof(std::uint32_t);
    if (part.geometry.vertices.size() + proxy.vertices.size() > vertexCap ||
        part.geometry.indices.size() + proxy.indices.size() > indexCap)
        throw std::runtime_error("Tree hierarchy geometry exceeds the supported buffer size.");
    const auto base = static_cast<std::uint32_t>(part.geometry.vertices.size());
    node.geometry = {static_cast<std::uint32_t>(part.geometry.indices.size()),
                     static_cast<std::uint32_t>(proxy.indices.size())};
    node.error = error;
    node.referenceError = referenceError;
    part.geometry.vertices.insert(part.geometry.vertices.end(), proxy.vertices.begin(), proxy.vertices.end());
    for (auto index : proxy.indices) part.geometry.indices.push_back(base + index);
    BoundingBox bounds;
    BoundingBox::CreateFromPoints(bounds, proxy.vertices.size(), &proxy.vertices.front().position, sizeof(Vertex));
    bounds.Transform(bounds, transform);
    node.bounds = mergedBounds(node.bounds, bounds);
}

MeshData reduceWood(const MeshData& source, float fraction, float error)
{
    MeshData proxy;
    proxy.vertices = source.vertices;
    proxy.indices = source.indices;
    const auto target = std::max<std::size_t>(3, static_cast<std::size_t>(source.indices.size() / 3 * fraction) * 3);
    constexpr std::array weights{0.015f, 0.015f, 0.015f, 0.06f, 0.06f, 0.06f, 0.06f};
    static_assert(offsetof(Vertex, texcoords) == offsetof(Vertex, normal) + 3 * sizeof(float));
    proxy.indices.resize(meshopt_simplifyWithUpdate(proxy.indices.data(), proxy.indices.size(),
        &proxy.vertices.front().position.x, proxy.vertices.size(), sizeof(Vertex),
        &proxy.vertices.front().normal.x, sizeof(Vertex), weights.data(), weights.size(), nullptr,
        target, error, meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive, nullptr));
    if (!proxy.indices.empty()) compact(proxy);
    return proxy;
}

std::vector<std::uint32_t> spatialReduction(const MeshData& source, float cell)
{
    const auto* positions = &source.vertices.front().position.x;
    const auto extent = meshopt_simplifyScale(positions, source.vertices.size(), sizeof(Vertex));
    if (!(extent > 0)) return {};
    std::vector<std::uint32_t> reduced(source.indices.size());
    reduced.resize(meshopt_simplifySloppy(reduced.data(), source.indices.data(), source.indices.size(), positions,
        source.vertices.size(), sizeof(Vertex), nullptr, 3, std::min(1.0f, cell / extent), nullptr));
    return reduced;
}

float repairCut(const MeshData& source, const std::vector<surface_distance::SurfaceTriangle>& reference,
                MeshData& proxy, float limit)
{
    using namespace surface_distance;
    constexpr unsigned subdivisions = 2;
    const float limitSquared = limit * limit;
    const auto candidate = surface(proxy.vertices, proxy.indices);
    const SurfaceSearch sourceSearch(reference);
    std::vector<SurfaceTriangle> retained;
    std::vector<std::uint32_t> indices;
    float worst = 0;
    for (std::size_t i = 0; i < candidate.size(); ++i)
    {
        const float error = sampledDistanceSquared(candidate[i], sourceSearch, subdivisions);
        if (!(error <= limitSquared)) continue;
        worst = std::max(worst, error);
        retained.push_back(candidate[i]);
        const auto first = proxy.indices.begin() + 3 * i;
        indices.insert(indices.end(), first, first + 3);
    }
    if (retained.empty())
    {
        proxy.indices.clear();
        return 0;
    }
    const auto base = static_cast<std::uint32_t>(proxy.vertices.size());
    const SurfaceSearch proxySearch(retained);
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        const float error = sampledDistanceSquared(reference[i], proxySearch, subdivisions);
        if (error <= limitSquared) worst = std::max(worst, error);
        else for (unsigned corner = 0; corner < 3; ++corner) indices.push_back(base + source.indices[3 * i + corner]);
    }
    proxy.vertices.insert(proxy.vertices.end(), source.vertices.begin(), source.vertices.end());
    proxy.indices = std::move(indices);
    compact(proxy);
    return std::sqrt(worst);
}

TreeLodPart bakeTrunk(const MeshData& source, std::uint32_t meshIndex)
{
    const auto matrix = XMLoadFloat4x4(&source.localToWorld);
    BoundingBox bounds(source.localBounds.center, source.localBounds.halfExtent);
    bounds.Transform(bounds, matrix);
    const auto reference = surface_distance::surface(source.vertices, source.indices);
    struct Candidate { MeshData mesh; float error; };
    std::vector<Candidate> candidates;
    const auto evaluate = [&](MeshData proxy, float limit) {
        if (proxy.indices.empty() || proxy.indices.size() >= source.indices.size()) return;
        const auto error = repairCut(source, reference, proxy, limit) * transformStretch(matrix);
        if (proxy.indices.empty() || proxy.indices.size() >= source.indices.size()) return;
        candidates.push_back({std::move(proxy), error});
    };
    MeshData coarse;
    coarse.vertices = source.vertices;
    coarse.indices = spatialReduction(source, 0.032f);
    evaluate(std::move(coarse), 0.024f);
    evaluate(reduceWood(source, 0.085f, 0.0045f), 0.0085f);
    evaluate(reduceWood(source, 0.20f, 0.0009f), 0.0026f);

    std::ranges::sort(candidates, {}, [](const auto& candidate) { return candidate.mesh.indices.size(); });
    TreeLodPart part{meshIndex, {}, {}};
    float bestError = std::numeric_limits<float>::infinity();
    const auto addNode = [&] {
        auto& node = part.nodes.emplace_back();
        node.bounds = {bounds.Center, bounds.Extents};
        node.placementCount = 1;
    };
    for (const auto& candidate : candidates)
    {
        if (candidate.error >= bestError) continue;
        bestError = candidate.error;
        addNode();
        addGeometry(part, part.nodes.back(), candidate.mesh, candidate.error, candidate.error, matrix);
        std::cout << "Trunk level " << part.nodes.size() - 1 << ": " << candidate.mesh.indices.size() / 3
                  << " triangles, error " << candidate.error << '\n';
    }
    addNode();
    for (auto i = part.nodes.size() - 1; i > 0; --i)
    {
        const auto& child = part.nodes[i].bounds;
        part.nodes[i - 1].firstChild = static_cast<std::uint32_t>(i);
        part.nodes[i - 1].childCount = 1;
        part.nodes[i - 1].bounds = mergedBounds(part.nodes[i - 1].bounds, BoundingBox(child.center, child.halfExtent));
    }
    return part;
}

struct CanopyTopology
{
    std::vector<std::uint32_t> order;
    std::vector<TreeLodNode> nodes;
};

CanopyTopology partitionCanopy(const MeshData& leaves, const FixedLod& leafLod, const MeshData& wood, unsigned fine,
                               std::span<const unsigned> regions = {})
{
    const auto meshBounds = [](const MeshData& mesh, std::span<const Vertex> fitted) {
        BoundingBox source(mesh.localBounds.center, mesh.localBounds.halfExtent);
        if (!fitted.empty())
        {
            BoundingBox proxy;
            BoundingBox::CreateFromPoints(proxy, fitted.size(), &fitted.front().position, sizeof(Vertex));
            BoundingBox::CreateMerged(source, source, proxy);
        }
        return source;
    };
    const auto leafBounds = meshBounds(leaves, leafLod.proxyVertices);
    const BoundingBox woodBounds(wood.localBounds.center, wood.localBounds.halfExtent);
    const auto leafMatrix = XMLoadFloat4x4(&leaves.localToWorld);
    const auto woodMatrix = XMLoadFloat4x4(&wood.localToWorld);
    std::vector<BoundingBox> placed;
    placed.reserve(leaves.instances->size());
    for (const auto& instance : *leaves.instances)
    {
        BoundingBox a, b, combined;
        leafBounds.Transform(a, instance.matrix() * leafMatrix);
        woodBounds.Transform(b, instance.matrix() * woodMatrix);
        BoundingBox::CreateMerged(combined, a, b);
        placed.push_back(combined);
    }
    CanopyTopology result{std::vector<std::uint32_t>(placed.size()), std::vector<TreeLodNode>(1)};
    std::iota(result.order.begin(), result.order.end(), 0u);
    const auto split = [&](auto&& self, std::uint32_t parent, std::uint32_t first, std::uint32_t count) -> void {
        auto ids = std::span(result.order).subspan(first, count);
        BoundingBox box = placed[ids.front()];
        for (auto id : ids) BoundingBox::CreateMerged(box, box, placed[id]);
        result.nodes[parent].bounds = {box.Center, box.Extents};
        result.nodes[parent].firstPlacement = first;
        result.nodes[parent].placementCount = count;
        if (count <= std::max(1u, fine / 2)) return;
        const auto axis = box.Extents.x >= box.Extents.y && box.Extents.x >= box.Extents.z ? &XMFLOAT3::x
            : box.Extents.y >= box.Extents.z ? &XMFLOAT3::y : &XMFLOAT3::z;
        const auto half = count / 2;
        std::nth_element(ids.begin(), ids.begin() + half, ids.end(), [&](auto a, auto b) {
            const auto x = placed[a].Center.*axis, y = placed[b].Center.*axis;
            return x == y ? a < b : x < y;
        });
        const auto children = static_cast<std::uint32_t>(result.nodes.size());
        result.nodes[parent].firstChild = children;
        result.nodes[parent].childCount = 2;
        result.nodes.resize(result.nodes.size() + 2);
        self(self, children, first, half);
        self(self, children + 1, first + half, count - half);
    };
    const auto count = static_cast<std::uint32_t>(placed.size());
    if (regions.empty()) split(split, 0, 0, count);
    else
    {
        std::ranges::stable_sort(result.order, {}, [&](auto id) { return regions[id]; });
        const unsigned regionCount = std::ranges::max(regions) + 1;
        result.nodes.resize(1 + regionCount);
        result.nodes[0].firstChild = 1;
        result.nodes[0].childCount = regionCount;
        result.nodes[0].placementCount = count;
        unsigned first = 0;
        for (unsigned region = 0; region < regionCount; ++region)
        {
            unsigned end = first;
            while (end < count && regions[result.order[end]] == region) ++end;
            if (end == first) throw std::runtime_error("Empty bough grid region.");
            split(split, 1 + region, first, end - first);
            const auto& b = result.nodes[1 + region].bounds;
            result.nodes[0].bounds = region ? mergedBounds(result.nodes[0].bounds, BoundingBox(b.center,b.halfExtent)) : b;
            first = end;
        }
    }
    return result;
}

bool bakeGridWood(TreeLodPart& part, TreeLodNode& node, const MeshData& mesh, const FixedLod& lod,
                  std::span<const std::uint32_t> ids, bool coarseTier)
{
    float error = 0;
    // Evaluate the assembled cut directly against the assembled LOD 4 mesh.
    const auto reference = expandPart(mesh, lod, fixedLodFarLevel, ids, error);
    float inherited = 0;
    auto proxy = expandPart(mesh, lod, lod.levels.size() - 1, ids, inherited);
    const float cell = coarseTier ? 0.047f : 0.027f;
    auto reduced = spatialReduction(proxy, cell);
    if (reduced.empty()) return false;
    const auto coarseReference = surface_distance::surface(proxy.vertices, proxy.indices);
    limitWoodTransition(proxy, coarseReference, reduced, coarseTier ? 0.020f : 0.0045f);
    if (reduced.size() >= proxy.indices.size()) return false;
    proxy.indices = std::move(reduced);
    compact(proxy);
    using namespace surface_distance;
    const auto refSurface = surface(reference.vertices, reference.indices);
    const auto reducedSurface = surface(proxy.vertices, proxy.indices);
    const auto transition = std::max(sampledDistance(refSurface, reducedSurface, 2),
                                     sampledDistance(reducedSurface, refSurface, 2));
    addGeometry(part, node, proxy, error + transition, transition, XMMatrixIdentity());
    return true;
}

void bakeCanopyPart(TreeLodPart& part, const MeshData& mesh, const FixedLod& lod,
                    std::span<const std::uint32_t> order, unsigned fine, unsigned coarse, bool leaves)
{
    const auto identity = XMMatrixIdentity();
    const auto coarseCount = lod.levels.back().indices.size();
    float fittedReferenceError = 0;
    if (leaves)
    {
        for (const auto& group : lod.sprayGroups)
            for (const auto& cut : group)
                if (cut.tintLevel == fixedLodTwoTriangleLevel)
                    fittedReferenceError = std::max(fittedReferenceError, cut.referenceError);
        if (!(fittedReferenceError > 0))
            throw std::runtime_error("Tree hierarchy requires measured fitted leaf cuts.");
    }
    unsigned proxies = 0;
    for (auto& node : part.nodes)
    {
        const auto size = node.placementCount;
        const bool coarseTier = size <= coarse && size > coarse / 2;
        const bool fineTier = size <= fine && size > fine / 2;
        if (!coarseTier && !fineTier) continue;
        const auto ids = order.subspan(node.firstPlacement, size);
        float error = 0;
        if (leaves)
        {
            float scale;
            auto proxy = expandPart(mesh, lod, lod.levels.size() - 1, ids, error, &scale);
            const auto transition = reduceLeaves(proxy, coarseTier ? 0.016f : 0.0137f, true);
            if (proxy.indices.size() >= coarseCount * size) continue;
            addGeometry(part, node, proxy, error + transition, fittedReferenceError * scale + transition, identity);
        }
        else
        {
            if (!bakeGridWood(part, node, mesh, lod, ids, coarseTier)) continue;
        }
        ++proxies;
        std::cout << "Tree part " << part.meshIndex << ", " << size << " sprays: "
                  << node.geometry.indexCount / 3 << " triangles, error " << node.error << '\n';
    }
    // Children may widen after reduction. Include all descendants in each parent's culling bound.
    for (auto i = part.nodes.size(); i-- > 0;)
    {
        auto& parent = part.nodes[i];
        for (std::size_t j = parent.firstChild; j < parent.firstChild + parent.childCount; ++j)
        {
            const auto& child = part.nodes[j];
            parent.bounds = mergedBounds(parent.bounds, BoundingBox(child.bounds.center, child.bounds.halfExtent));
        }
    }
    std::cout << "Baked " << proxies << " parent cuts for mesh " << part.meshIndex << '\n';
}

void shareVertices(MeshData& mesh)
{
    std::vector<std::uint32_t> order(mesh.vertices.size()), remap(mesh.vertices.size());
    std::iota(order.begin(), order.end(), 0u);
    const auto compare = [](const Vertex& a, const Vertex& b) {
        return std::memcmp(&a, &b, sizeof(Vertex));
    };
    std::ranges::sort(order, [&](auto a, auto b) { return compare(mesh.vertices[a], mesh.vertices[b]) < 0; });
    std::vector<Vertex> unique;
    for (const auto id : order)
    {
        const auto& v = mesh.vertices[id];
        if (unique.empty() || compare(v, unique.back()) != 0) unique.push_back(v);
        remap[id] = static_cast<std::uint32_t>(unique.size() - 1);
    }
    for (auto& index : mesh.indices) index = remap[index];
    mesh.vertices = std::move(unique);
}

unsigned unsignedOption(const wchar_t* text)
{
    wchar_t* end = nullptr;
    const auto value = std::wcstoul(text, &end, 10);
    if (end == text || *end || *text == L'-') throw std::runtime_error("Invalid unsigned hierarchy argument.");
    return value;
}

void bakeHierarchy(int argc, wchar_t* argv[])
{
    if (argc != 9)
        throw std::runtime_error("Usage: BakeSprayBough --hierarchy tree.glb leaves.fixedlod wood.fixedlod trunk-index output.treelod fine-sprays coarse-sprays");
    const SprayBoughSources sources{argv[2], argv[3], argv[4]};
    const auto trunk = unsignedOption(argv[5]);
    const auto fine = unsignedOption(argv[7]), coarse = unsignedOption(argv[8]);
    if (fine < 2 || coarse / 2 < fine) throw std::runtime_error("Hierarchy tiers need distinct placement ranges.");
    const auto asset = loadGlb(sources[0]);
    const std::array lods{readFixedLod(sources[0], sources[1], asset), readFixedLod(sources[0], sources[2], asset)};
    const auto& leaves = asset.meshes[lods[0].meshIndex];
    const auto& wood = asset.meshes[lods[1].meshIndex];
    if (!leaves.instances || leaves.instances->empty() || wood.instances != leaves.instances ||
        trunk >= asset.meshes.size() || asset.meshes[trunk].instances ||
        lods[0].levels.size() != fixedLodLevelCount || lods[1].levels.size() <= fixedLodFarLevel)
        throw std::runtime_error("Hierarchy requires matching spray placements, fitted leaf and twig LODs, and a static trunk.");
    auto topology = partitionCanopy(leaves, lods[0], wood, fine);
    TreeHierarchy hierarchy{std::move(topology.order), {}};
    for (std::size_t i = 0; i < lods.size(); ++i)
    {
        const auto index = lods[i].meshIndex;
        auto& part = hierarchy.parts.emplace_back(TreeLodPart{index, topology.nodes, {}});
        bakeCanopyPart(part, asset.meshes[index], lods[i], hierarchy.placementOrder, fine, coarse, i == 0);
    }
    hierarchy.parts.push_back(bakeTrunk(asset.meshes[trunk], trunk));
    for (auto& part : hierarchy.parts) shareVertices(part.geometry);
    // Keep the deduplicated spray wood in draw order for vertex fetch locality.
    compact(hierarchy.parts[1].geometry);
    writeTreeHierarchy(argv[6], sources, hierarchy);
}

// A diagnostic stream of the assembled parent, including normals, for the independent
// image-space depth and silhouette comparison. The viewer only needs the .treelod.
void writeTwigSample(const std::filesystem::path& path, MeshData mesh)
{
    compact(mesh);
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    const std::array counts{static_cast<std::uint32_t>(mesh.vertices.size()), static_cast<std::uint32_t>(mesh.indices.size())};
    out.write(reinterpret_cast<const char*>(counts.data()), sizeof(counts));
    out.write(reinterpret_cast<const char*>(mesh.vertices.data()), mesh.vertices.size() * sizeof(Vertex));
    out.write(reinterpret_cast<const char*>(mesh.indices.data()), mesh.indices.size() * sizeof(std::uint32_t));
}

void refitTwigParent(TreeLodPart& part, unsigned id, const MeshData& wood, const FixedLod& lod,
                     std::span<const std::uint32_t> order, bool large, const std::filesystem::path& samples, twig_repair::TwigRepair recipe)
{
    using namespace surface_distance;
    auto& n = part.nodes.at(id);
    const unsigned capacity = large ? 384 : 96;
    if (!n.geometry.indexCount || n.placementCount <= capacity/2 || n.placementCount > capacity)
        throw std::runtime_error("Selected twig node has no parent geometry at the requested scale.");
    const auto placements = order.subspan(n.firstPlacement, n.placementCount);
    float sourceError = 0, coarseError = 0;
    const auto reference = expandPart(wood, lod, fixedLodFarLevel, placements, sourceError);
    auto coarse = expandPart(wood, lod, lod.levels.size()-1, placements, coarseError);
    const auto before = spatialReduction(coarse, large ? 0.047f : 0.027f);
    auto indices = before;
    const auto coarseSurface = surface(coarse.vertices, coarse.indices);
    const float allowance = large ? 0.020f : 0.0045f;
    std::size_t repaired = 0;
    limitWoodTransition(coarse, coarseSurface, indices, allowance, &repaired);
    if (indices.size() != n.geometry.indexCount)
        throw std::runtime_error("Twig pilot does not reproduce the existing grid repair.");
    const auto sample = [&](const char* label, const MeshData& mesh) {
        if (!samples.empty()) writeTwigSample(samples / (std::to_string(id) + "-" + label + ".mesh"), mesh);
    };
    sample("reference", reference);
    auto oldMesh = coarse;
    oldMesh.indices = indices;
    sample("previous", oldMesh);

    // Keep the grid cut intact. Only edges within the restored source patches can
    // collapse. Lock their open rims and junctions with faces outside the patch.
    const auto failed = std::span(indices).subspan(repaired);
    if (failed.empty()) throw std::runtime_error("Selected twig parent has no restored faces.");
    using namespace twig_repair;
    const auto fit = recipe == TwigRepair::Loose ? fitTwigPatch(coarse,failed,allowance)
                                                  : fitAnchoredTwigPatches(coarse,failed,allowance,recipe);
    const auto fittedFaces = surface(coarse.vertices, fit);
    const SurfaceSearch coarseSearch(coarseSurface);
    indices.resize(repaired);
    for (std::size_t i = 0; i < fittedFaces.size(); ++i)
        if (sampledDistanceSquared(fittedFaces[i], coarseSearch, 4) <= allowance*allowance)
            indices.insert(indices.end(), fit.begin()+3*i, fit.begin()+3*i+3);
    const auto cutSurface = surface(coarse.vertices, indices);
    const SurfaceSearch cutSearch(cutSurface);
    const auto fittedCount = indices.size();
    for (std::size_t i = 0; i < coarseSurface.size(); ++i)
        if (sampledDistanceSquared(coarseSurface[i], cutSearch, 4) > allowance*allowance)
            indices.insert(indices.end(), coarse.indices.begin()+3*i, coarse.indices.begin()+3*i+3);
    std::cout << "Parent " << id << ", " << n.placementCount << " sprays, kept grid " << repaired/3
              << ", old repair " << (oldMesh.indices.size()-repaired)/3 << ", patch fit " << (fittedCount-repaired)/3
              << ", restored after refit " << (indices.size()-fittedCount)/3 << '\n';
    if (indices.size() >= n.geometry.indexCount)
    {
        sample("refit",oldMesh);
        std::cout << "Retaining existing parent\n";
        return;
    }
    coarse.indices = std::move(indices);
    compact(coarse);
    sample("refit",coarse);
    const auto lod4Surface = surface(reference.vertices,reference.indices);
    const auto result = surface(coarse.vertices,coarse.indices);
    const auto forward = sampledDistance(lod4Surface,result,2), reverse = sampledDistance(result,lod4Surface,2);
    const auto transition = std::max(forward,reverse);
    std::cout << "Parent " << id << ", triangles " << n.geometry.indexCount/3 << " -> " << coarse.indices.size()/3
              << ", previous LOD4 error " << n.referenceError << ", LOD4 to new " << forward
              << ", new to LOD4 " << reverse << '\n';
    // Keep the parent at the same position in the index buffer and update following ranges.
    const auto range = n.geometry;
    auto& geometry = part.geometry;
    auto pos = geometry.indices.erase(geometry.indices.begin()+range.firstIndex,
                                      geometry.indices.begin()+range.firstIndex+range.indexCount);
    const auto base = static_cast<std::uint32_t>(geometry.vertices.size());
    for (auto& v : coarse.indices) v += base;
    geometry.indices.insert(pos,coarse.indices.begin(),coarse.indices.end());
    geometry.vertices.insert(geometry.vertices.end(),coarse.vertices.begin(),coarse.vertices.end());
    n.geometry.indexCount = static_cast<std::uint32_t>(coarse.indices.size());
    for (auto& other : part.nodes)
        if (other.geometry.firstIndex > range.firstIndex)
            other.geometry.firstIndex -= range.indexCount-n.geometry.indexCount;
    n.referenceError = transition;
    n.error = sourceError + transition;
    // Every retained vertex belongs to the assembled input and is within the node's bounds.
}

void bakeTwigPilot(int argc, wchar_t* argv[])
{
    if (argc != 8 && argc != 9)
        throw std::runtime_error("Usage: BakeSprayBough --twig-repair tree.glb leaves.fixedlod wood.fixedlod output.treelod small-node large-node [sample-directory]");
    const SprayBoughSources sources{argv[2],argv[3],argv[4]};
    const auto asset = loadGlb(sources[0]);
    auto hierarchy = readTreeHierarchy(sources,asset);
    const auto lod = readFixedLod(sources[0],sources[2],asset);
    const auto wood = std::ranges::find(hierarchy.parts,lod.meshIndex,&TreeLodPart::meshIndex);
    if (wood == hierarchy.parts.end()) throw std::runtime_error("Missing wood hierarchy.");
    std::filesystem::path samples;
    if (argc == 9) { samples = argv[8]; std::filesystem::create_directories(samples); }
    const std::wstring_view mode = argv[1];
    using twig_repair::TwigRepair;
    const auto recipe = mode == L"--twig-repair-loose" ? TwigRepair::Loose :
        mode == L"--twig-repair-intermediate" ? TwigRepair::Intermediate : TwigRepair::Anchored;
    for (unsigned i = 0; i < 2; ++i)
        refitTwigParent(*wood,unsignedOption(argv[6+i]),asset.meshes.at(lod.meshIndex),lod,
                        hierarchy.placementOrder,i == 1,samples,recipe);
    compact(wood->geometry);
    shareVertices(wood->geometry);
    writeTreeHierarchy(argv[5],sources,hierarchy);
}

struct BoughCut
{
    FixedLodIndexRange range;
    unsigned indicesPerSpray;
    float referenceError;
};

void bakeBranchPilot(int argc, wchar_t* argv[])
{
    if (argc != 8)
        throw std::runtime_error("Usage: BakeSprayBough --branch-cut tree.glb leaves.fixedlod wood.fixedlod members.txt control.treelod branch.treelod");
    const SprayBoughSources sources{argv[2], argv[3], argv[4]};
    const auto asset = loadGlb(sources[0]);
    auto hierarchy = readTreeHierarchy(sources, asset);
    const auto lod = readFixedLod(sources[0], sources[2], asset);
    const auto& wood = asset.meshes.at(lod.meshIndex);
    auto it = std::ranges::find(hierarchy.parts, lod.meshIndex, &TreeLodPart::meshIndex);
    if (it == hierarchy.parts.end() || !wood.instances) throw std::runtime_error("Missing woody hierarchy.");
    auto& part = *it;
    if (!part.placementOrder.empty() || lod.usesProxyVertices(fixedLodFarLevel))
        throw std::runtime_error("Branch pilot requires the base grid hierarchy and a shared wood cut indexed into the authored mesh.");
    const auto members = boughPlacements(argv[5], sourceFingerprint(sources[0]), wood.instances->size());
    const auto original = part.nodes;
    const auto& order = hierarchy.placementOrder;
    std::vector<unsigned> outsidePrefix{0};
    for (auto id : order)
    {
        if (!std::ranges::binary_search(members, id)) part.placementOrder.push_back(id);
        outsidePrefix.push_back(static_cast<unsigned>(part.placementOrder.size()));
    }
    const auto offset = outsidePrefix.back();
    part.placementOrder.insert(part.placementOrder.end(), members.begin(), members.end());
    part.nodes.assign(3, {});
    part.nodes[0] = original.front();
    part.nodes[0].firstChild = 1;
    part.nodes[0].childCount = 2;
    part.nodes[0].geometry = {};
    const auto outsideCount = [&](const TreeLodNode& n) {
        return outsidePrefix[n.firstPlacement+n.placementCount]-outsidePrefix[n.firstPlacement];
    };
    // Preserve every untouched spatial proxy. Rebuild only the old grid parents
    // that cross the ownership boundary, identically in the control and trial.
    const auto outside = [&](auto&& self, unsigned dest, unsigned src) -> void {
        auto n = original[src];
        n.firstPlacement = outsidePrefix[n.firstPlacement];
        n.placementCount = outsideCount(original[src]);
        if (n.geometry.indexCount && n.placementCount != original[src].placementCount)
        {
            n.geometry = {};
            const auto ids = std::span(part.placementOrder).subspan(n.firstPlacement,n.placementCount);
            bakeGridWood(part,n,wood,lod,ids,original[src].placementCount > 96);
        }
        const auto oldFirst = n.firstChild, oldCount = n.childCount;
        n.firstChild = static_cast<unsigned>(part.nodes.size());
        n.childCount = 0;
        for (unsigned j = 0; j < oldCount; ++j) n.childCount += outsideCount(original[oldFirst+j]) != 0;
        part.nodes.resize(part.nodes.size()+n.childCount);
        part.nodes[dest] = n;
        auto child = n.firstChild;
        for (unsigned j = 0; j < oldCount; ++j)
            if (outsideCount(original[oldFirst+j])) self(self,child++,oldFirst+j);
    };
    outside(outside,1,0);
    const auto model = XMLoadFloat4x4(&wood.localToWorld);
    std::vector<float> scales;
    std::vector<BoundingBox> bounds;
    for (auto id : members)
    {
        const auto m = (*wood.instances)[id].matrix()*model;
        scales.push_back(transformStretch(m));
        BoundingBox box(wood.localBounds.center,wood.localBounds.halfExtent);
        BoundingBox placed;
        box.Transform(placed,m);
        bounds.push_back(placed);
    }
    // Build the grid control on the *same* authored bough and contiguous ranges.
    const auto inside = [&](auto&& self, unsigned dest, unsigned first, unsigned count) -> void {
        TreeLodNode n{};
        n.firstPlacement = offset + first;
        n.placementCount = count;
        auto box = bounds[first];
        for (unsigned j = 1; j < count; ++j) BoundingBox::CreateMerged(box,box,bounds[first+j]);
        n.bounds = {box.Center,box.Extents};
        if ((count > 48 && count <= 96) || (count > 192 && count <= 384))
            bakeGridWood(part,n,wood,lod,std::span(members).subspan(first,count),count > 192);
        if (count > 24)
        {
            n.firstChild = static_cast<unsigned>(part.nodes.size());
            n.childCount = 2;
            part.nodes.resize(part.nodes.size()+2);
            self(self,n.firstChild,first,count/2);
            self(self,n.firstChild+1,first+count/2,count-count/2);
            for (unsigned j = 0; j < 2; ++j)
            {
                const auto b = part.nodes[n.firstChild+j].bounds;
                n.bounds = mergedBounds(n.bounds,BoundingBox(b.center,b.halfExtent));
            }
        }
        part.nodes[dest] = n;
    };
    inside(inside,2,0,static_cast<unsigned>(members.size()));
    writeTreeHierarchy(argv[6],sources,hierarchy);

    MeshData base;
    base.vertices = wood.vertices;
    base.indices = lod.levels.at(fixedLodFarLevel).indices;
    compact(base);
    const auto branches = branch_cut::components(wood,lod.levels[fixedLodFarLevel].indices);
    const auto reference = surface_distance::surface(base.vertices,base.indices);
    float inherited = 0;
    for (const auto& g : lod.groups)
    {
        const auto& s = g.levels[fixedLodFarLevel];
        inherited = std::max({inherited,s.surfaceError(),s.rimDeviation,s.simplifierError});
    }
    std::cout << "Bough " << members.size() << " placements, edge-connected pieces per spray " << branches.size()
              << ", input triangles " << base.indices.size()/3 << '\n';
    std::vector<BoughCut> cuts;
    std::vector<MeshData> templates = branch_cut::candidates(branches);
    // Ranges are shared across all bough nodes. Avoid storing identical cuts.
    std::ranges::sort(templates,{},[](const auto& m){return m.indices.size();});
    float bestError = std::numeric_limits<float>::infinity();
    for (auto& fit : templates)
    {
        compact(fit);
        const auto surface = surface_distance::surface(fit.vertices,fit.indices);
        const auto forward = surface_distance::sampledDistance(reference,surface,4);
        const auto reverse = surface_distance::sampledDistance(surface,reference,4);
        const auto error = std::max(forward,reverse);
        if (error >= bestError) continue;
        bestError = error;
        TreeLodNode storage{};
        float unused = 0;
        auto placed = expandCut(wood,fit,members,unused);
        addGeometry(part,storage,placed,0,0,XMMatrixIdentity());
        // Enclose all fitted vertices even where the optimizer moves a vertex
        // outside the authored local box.
        BoundingBox fitBox;
        BoundingBox::CreateFromPoints(fitBox,fit.vertices.size(),&fit.vertices.front().position,sizeof(Vertex));
        for (unsigned j = 0; j < members.size(); ++j)
        {
            BoundingBox b;
            fitBox.Transform(b,(*wood.instances)[members[j]].matrix()*model);
            BoundingBox::CreateMerged(bounds[j],bounds[j],b);
        }
        cuts.push_back({storage.geometry,static_cast<unsigned>(fit.indices.size()),error});
        std::cout << "Connected range " << storage.geometry.firstIndex << ", triangles/spray " << fit.indices.size()/3
                  << ", local forward/reverse " << forward << " / " << reverse << '\n';
    }
    const auto grid = part.nodes;
    // Re-emit the bough subtree so every child follows its parent. Each unary
    // alternative covers the exact same placements and falls through on error.
    const auto copy = [&](auto&& self,unsigned dest,unsigned src,bool branch) -> void {
        auto n = grid[src];
        if (branch)
        {
            const auto first = n.firstPlacement-offset;
            auto box = bounds[first];
            for (unsigned j = 1; j < n.placementCount; ++j) BoundingBox::CreateMerged(box,box,bounds[first+j]);
            n.bounds = mergedBounds(n.bounds,box);
            const float scale = std::ranges::max(std::span(scales).subspan(first,n.placementCount));
            std::vector<TreeLodNode> choices;
            if (n.geometry.indexCount) choices.push_back(n);
            for (const auto& cut : cuts)
            {
                auto choice = n;
                choice.geometry = {cut.range.firstIndex+first*cut.indicesPerSpray,n.placementCount*cut.indicesPerSpray};
                if (n.childCount && (!n.geometry.indexCount || choice.geometry.indexCount >= n.geometry.indexCount)) continue;
                choice.referenceError = cut.referenceError*scale;
                choice.error = (inherited+cut.referenceError)*scale;
                choices.push_back(choice);
            }
            std::ranges::stable_sort(choices,{},[](const auto& c){return c.geometry.indexCount;});
            float bestSource = INFINITY, bestRef = INFINITY;
            for (auto choice : choices)
            {
                if (choice.error >= bestSource && choice.referenceError >= bestRef) continue;
                bestSource = std::min(bestSource,choice.error);
                bestRef = std::min(bestRef,choice.referenceError);
                choice.firstChild = static_cast<unsigned>(part.nodes.size());
                choice.childCount = 1;
                part.nodes[dest] = choice;
                dest = choice.firstChild;
                part.nodes.emplace_back();
            }
            n.geometry = {};
        }
        n.firstChild = static_cast<unsigned>(part.nodes.size());
        part.nodes[dest] = n;
        part.nodes.resize(part.nodes.size()+n.childCount);
        for (unsigned j = 0; j < n.childCount; ++j) self(self,n.firstChild+j,grid[src].firstChild+j,branch);
    };
    part.nodes.resize(3);
    copy(copy,1,1,false);
    copy(copy,2,2,true);
    for (auto i = part.nodes.size(); i-- > 0;)
        for (unsigned j = 0; j < part.nodes[i].childCount; ++j)
        {
            const auto b = part.nodes[part.nodes[i].firstChild+j].bounds;
            part.nodes[i].bounds = mergedBounds(part.nodes[i].bounds,BoundingBox(b.center,b.halfExtent));
        }
    writeTreeHierarchy(argv[7],sources,hierarchy);
}

void bakeBoughGrid(int argc, wchar_t* argv[])
{
    if (argc != 8) throw std::runtime_error("Usage: --bough-grid tree.glb leaves.fixedlod wood.fixedlod exposed.txt interior.txt output.treelod");
    const SprayBoughSources sources{argv[2],argv[3],argv[4]};
    const auto asset=loadGlb(sources[0]);
    const std::array lods{readFixedLod(sources[0],sources[1],asset),readFixedLod(sources[0],sources[2],asset)};
    const auto& leaves=asset.meshes.at(lods[0].meshIndex);
    const auto& wood=asset.meshes.at(lods[1].meshIndex);
    auto h=readTreeHierarchy(sources,asset);
    std::vector<unsigned> regions(leaves.instances->size());
    for (unsigned r=1;r<=2;++r)
        for (auto id:boughPlacements(argv[4+r],sourceFingerprint(sources[0]),regions.size()))
        {
            if (regions[id]) throw std::runtime_error("Overlapping boughs.");
            regions[id]=r;
        }
    auto topology=partitionCanopy(leaves,lods[0],wood,96,regions);
    h.placementOrder=std::move(topology.order);
    for (unsigned slot=0;slot<2;++slot)
    {
        auto& part=*std::ranges::find(h.parts,lods[slot].meshIndex,&TreeLodPart::meshIndex);
        part={lods[slot].meshIndex,topology.nodes,{}};
        bakeCanopyPart(part,asset.meshes[part.meshIndex],lods[slot],h.placementOrder,96,384,slot==0);
        shareVertices(part.geometry);
        if (slot==1) compact(part.geometry);
    }
    writeTreeHierarchy(argv[7],sources,h);
}
}

// Reorder triangles inside each static draw. Ranges, vertices, material and LOD
// errors remain valid. The renderer still issues the original indexed draw.
static void packStaticDrawOrder(int argc, wchar_t* argv[])
{
    if (argc != 6) throw std::runtime_error("Usage: BakeSprayBough --pack-static-order tree.glb leaves.fixedlod wood.fixedlod output.treelod");
    const SprayBoughSources sources{argv[2], argv[3], argv[4]};
    const auto asset = loadGlb(sources[0]);
    auto hierarchy = readTreeHierarchy(sources, asset);
    for (auto& part : hierarchy.parts)
    {
        if (asset.meshes[part.meshIndex].instances) continue;
        auto& mesh = part.geometry;
        std::vector<std::pair<unsigned, unsigned>> ranges;
        for (const auto& node : part.nodes)
            if (node.geometry.indexCount) ranges.emplace_back(node.geometry.firstIndex, node.geometry.indexCount);
        std::ranges::sort(ranges);
        ranges.erase(std::unique(ranges.begin(),ranges.end()),ranges.end());
        size_t end = 0;
        for (const auto& [first, count] : ranges)
        {
            if (first < end) throw std::runtime_error("Static draw ranges overlap.");
            end = size_t(first) + count;
            constexpr unsigned maxVertices = 64, maxTriangles = 96;
            std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(count, maxVertices, maxTriangles));
            std::vector<unsigned> vertices(count);
            std::vector<unsigned char> triangles(count + 3 * meshlets.size());
            const size_t batches = meshopt_buildMeshlets(meshlets.data(), vertices.data(), triangles.data(),
                mesh.indices.data()+first, count, &mesh.vertices.front().position.x, mesh.vertices.size(),
                sizeof(Vertex), maxVertices, maxTriangles, 0.f);
            auto output = mesh.indices.begin() + first;
            for (const auto& batch : std::span(meshlets).first(batches))
                for (unsigned corner = 0; corner < 3 * batch.triangle_count; ++corner)
                    *output++ = vertices[batch.vertex_offset + triangles[batch.triangle_offset + corner]];
            if (output != mesh.indices.begin() + end) throw std::runtime_error("Static draw packing lost triangles.");
        }
    }
    writeTreeHierarchy(argv[5], sources, hierarchy);
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        if (argc > 1 && std::wstring_view(argv[1]) == L"--pack-static-order")
        {
            packStaticDrawOrder(argc, argv);
            return 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--bough-grid")
        {
            bakeBoughGrid(argc,argv);
            return 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--branch-cut")
        {
            bakeBranchPilot(argc,argv);
            return 0;
        }
        if (argc > 1 && (std::wstring_view(argv[1]) == L"--twig-repair" || std::wstring_view(argv[1]) == L"--twig-repair-loose"
                         || std::wstring_view(argv[1]) == L"--twig-repair-intermediate"))
        {
            bakeTwigPilot(argc, argv);
            return 0;
        }
        if (argc > 1 && std::wstring_view(argv[1]) == L"--hierarchy")
        {
            bakeHierarchy(argc, argv);
            return 0;
        }
        if (argc < 6)
            throw std::runtime_error("Usage: BakeSprayBough tree.glb leaves.fixedlod wood.fixedlod members.txt output.bough "
                                     "[--leaf-error distance] [--wood-cell distance] [--wood-error distance] "
                                     "[--no-leaf-area-compensation]");
        float leafLimit = 0.0f;
        std::optional<float> woodCell;
        std::optional<float> woodLimit;
        bool preserveArea = true;
        for (int i = 6; i < argc; ++i)
        {
            const std::wstring_view flag = argv[i];
            if (flag == L"--no-leaf-area-compensation") preserveArea = false;
            else if (flag == L"--leaf-error" && i + 1 < argc) leafLimit = parseDistance(argv[++i]);
            else if (flag == L"--wood-cell" && i + 1 < argc) woodCell = parseDistance(argv[++i]);
            else if (flag == L"--wood-error" && i + 1 < argc) woodLimit = parseDistance(argv[++i]);
            else throw std::runtime_error("Unknown or incomplete bough bake option.");
        }
        if (!preserveArea && leafLimit == 0)
            throw std::runtime_error("Disabling leaf area compensation requires --leaf-error.");
        if (woodLimit && !woodCell) throw std::runtime_error("--wood-error requires --wood-cell.");
        const SprayBoughSources sources{argv[1], argv[2], argv[3]};
        const auto asset = loadGlb(sources[0]);
        const std::array lods{readFixedLod(sources[0], sources[1], asset), readFixedLod(sources[0], sources[2], asset)};
        const auto& leaves = asset.meshes[lods[0].meshIndex];
        const auto& wood = asset.meshes[lods[1].meshIndex];
        if (!leaves.instances || leaves.instances != wood.instances || lods[0].meshIndex == lods[1].meshIndex)
            throw std::runtime_error("Bough parts must use the same set of spray placements.");

        SprayBough bough;
        bough.placements = boughPlacements(argv[4], sourceFingerprint(sources[0]), leaves.instances->size());
        float leafError = 0.0f, woodError = 0.0f;
        bough.parts[0] = expandPart(leaves, lods[0], sprayBoughInputLevel(lods[0]), bough.placements, leafError);
        if (leafLimit > 0) leafError += reduceLeaves(bough.parts[0], leafLimit, preserveArea);
        auto& proxyWood = bough.parts[1];
        const auto woodLevel = woodCell ? lods[1].levels.size() - 1 : sprayBoughInputLevel(lods[1]);
        proxyWood = expandPart(wood, lods[1], woodLevel, bough.placements, woodError);
        const auto woodReference = woodCell ? surface_distance::surface(proxyWood.vertices, proxyWood.indices)
                                            : std::vector<surface_distance::SurfaceTriangle>{};
        const auto original = proxyWood.indices.size();
        std::vector<std::uint32_t> reduced(original);
        const auto* positions = &proxyWood.vertices.front().position.x;
        const float extent = meshopt_simplifyScale(positions, proxyWood.vertices.size(), sizeof(Vertex));
        if (!(extent > 0)) throw std::runtime_error("Bough wood must have nonzero extent.");
        const float cellSize = woodCell.value_or(0.025f);
        float simplifiedError = 0;
        reduced.resize(meshopt_simplifySloppy(reduced.data(), proxyWood.indices.data(), original, positions,
            proxyWood.vertices.size(), sizeof(Vertex), nullptr, std::max<std::size_t>(3, original / 60 * 3),
            std::min(1.0f, cellSize / extent), &simplifiedError));
        if (reduced.empty()) throw std::runtime_error("Bough wood simplification removed every triangle.");
        if (woodLimit) limitWoodTransition(proxyWood, woodReference, reduced, *woodLimit);
        if (reduced.size() >= original)
            throw std::runtime_error("Could not produce reduced bough wood.");
        proxyWood.indices = std::move(reduced);
        compact(proxyWood);
        float woodReductionError = std::max(1.75f * cellSize, simplifiedError * extent);
        if (woodCell)
        {
            const auto proxy = surface_distance::surface(proxyWood.vertices, proxyWood.indices);
            const float sourceToProxy = surface_distance::sampledDistance(woodReference, proxy, 4);
            const float proxyToSource = surface_distance::sampledDistance(proxy, woodReference, 4);
            woodReductionError = std::max(sourceToProxy, proxyToSource);
            if (woodLimit && woodReductionError > *woodLimit + 1e-6f)
                throw std::runtime_error("The final wood proxy exceeds the sampled transition allowance.");
            std::cout << "Bough wood sampled transition " << sourceToProxy << " / " << proxyToSource << " scene units\n";
        }
        const float combinedWoodError = woodError + woodReductionError;
        bough.error = std::max(leafError, combinedWoodError);
        writeSprayBough(argv[5], sources, bough);
        std::cout << bough.placements.size() << " placements\n"
                  << bough.parts[0].indices.size() / 3 << " leaf triangles, error input " << leafError << "\n"
                  << "Bough wood LOD " << woodLevel << ", " << original / 3 << " -> " << proxyWood.indices.size() / 3
                  << " triangles, error input " << combinedWoodError << "\n"
                  << "Projected error input " << bough.error << " scene units\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "BakeSprayBough: " << error.what() << '\n';
        return 1;
    }
}
