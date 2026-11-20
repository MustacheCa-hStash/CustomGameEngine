#pragma once

#include "MeshData.h"
#include "SurfaceDistance.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <stdexcept>

namespace branch_cut
{
// Partition on shared geometric edges. UV and normal wedges keep their original
// vertex records. Unrelated twigs can never collapse into one another.
inline std::vector<MeshData> components(const MeshData& source, std::span<const unsigned> cut)
{
    std::vector<unsigned> positions(source.vertices.size()), parent(source.indices.size() / 3);
    std::map<std::array<float,3>,unsigned> welded;
    for (unsigned i = 0; i < positions.size(); ++i)
    {
        const auto p = source.vertices[i].position;
        positions[i] = welded.try_emplace(std::array{p.x,p.y,p.z},i).first->second;
    }
    std::iota(parent.begin(), parent.end(), 0u);
    const auto root = [&](unsigned face) {
        while (face != parent[face]) { parent[face] = parent[parent[face]]; face = parent[face]; }
        return face;
    };
    std::map<std::pair<unsigned, unsigned>, unsigned> owner;
    for (unsigned f = 0; f < parent.size(); ++f)
        for (unsigned edge = 0; edge < 3; ++edge)
        {
            const auto a = positions[source.indices[3*f + edge]];
            const auto b = positions[source.indices[3*f + (edge+1)%3]];
            if (a == b) continue;
            const auto [it, first] = owner.try_emplace(std::minmax(a,b), f);
            if (!first) parent[root(f)] = root(it->second);
        }
    // Carry authored connectivity through an index-only reduction. The shared
    // LOD 4 can split a continuous twig into several disconnected slivers.
    std::vector<unsigned> vertexFace(positions.size(), UINT32_MAX);
    for (unsigned i = 0; i < source.indices.size(); ++i)
        vertexFace[source.indices[i]] = i/3;
    if (cut.size() % 3 || std::ranges::any_of(cut, [&](unsigned v) {
        return v >= vertexFace.size() || vertexFace[v] == UINT32_MAX;
    })) throw std::runtime_error("Branch cut contains a face without authored wood ownership.");
    for (unsigned i = 0; i < cut.size(); i += 3)
    {
        const auto component = root(vertexFace.at(cut[i]));
        for (unsigned j = 1; j < 3; ++j) parent[root(vertexFace.at(cut[i+j]))] = component;
    }
    std::map<unsigned, std::vector<unsigned>> islands;
    for (unsigned i = 0; i < cut.size(); i += 3)
    {
        auto& faces = islands[root(vertexFace[cut[i]])];
        faces.insert(faces.end(), cut.begin()+i, cut.begin()+i+3);
    }
    std::vector<MeshData> result;
    for (const auto& [id, indices] : islands)
    {
        auto& part = result.emplace_back();
        std::map<unsigned,unsigned> remap;
        for (auto v : indices)
        {
            const auto [it, first] = remap.try_emplace(v, static_cast<unsigned>(remap.size()));
            if (first) part.vertices.push_back(source.vertices[v]);
            part.indices.push_back(it->second);
        }
    }
    return result;
}

inline MeshData reducePiece(const MeshData& original, float fraction)
{
    constexpr std::array weights{0.003f,0.003f,0.003f,0.01f,0.01f,0.01f,0.01f};
    auto part = original;
    // Keep both extremities of each connected piece, including coincident seams.
    std::vector<unsigned char> locks(part.vertices.size());
    float longest = -1;
    auto axis = &DirectX::XMFLOAT3::x;
    float low = 0, high = 0;
    for (auto k : {&DirectX::XMFLOAT3::x, &DirectX::XMFLOAT3::y, &DirectX::XMFLOAT3::z})
    {
        const auto [lo, hi] = std::ranges::minmax_element(part.vertices, {}, [k](const auto& v){return v.position.*k;});
        const float span = hi->position.*k - lo->position.*k;
        if (span > longest) { longest = span; axis = k; low = lo->position.*k; high = hi->position.*k; }
    }
    for (unsigned i = 0; i < locks.size(); ++i)
        if (part.vertices[i].position.*axis == low || part.vertices[i].position.*axis == high)
            locks[i] = meshopt_SimplifyVertex_Lock;
    const auto target = std::max(1u, static_cast<unsigned>(part.indices.size()/3 * fraction));
    part.indices.resize(meshopt_simplifyWithUpdate(part.indices.data(), part.indices.size(),
        &part.vertices.front().position.x, part.vertices.size(), sizeof(Vertex),
        &part.vertices.front().normal.x, sizeof(Vertex), weights.data(), weights.size(), locks.data(),
        3*target, 1.0f, meshopt_SimplifyPermissive, nullptr));
    if (part.indices.empty()) part = original;
    for (auto& v : part.vertices)
        DirectX::XMStoreFloat3(&v.normal, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&v.normal)));
    return part;
}

inline void append(MeshData& result, const MeshData& piece)
{
    const auto first = static_cast<unsigned>(result.vertices.size());
    result.vertices.insert(result.vertices.end(), piece.vertices.begin(), piece.vertices.end());
    for (auto v : piece.indices) result.indices.push_back(first+v);
}

// Fit a tapered strip or tube to the surviving vertices of one authored twig.
// All corners come from the shared input, including their UVs and normals.
inline MeshData fitStem(const MeshData& branch, unsigned rings, unsigned sides)
{
    using namespace DirectX;
    MeshData fit;
    fit.vertices = branch.vertices;
    const auto point = [&](unsigned i) {return XMLoadFloat3(&branch.vertices[i].position);};
    const auto farthest = [&](unsigned from) {
        unsigned best = from;
        float distance = 0;
        for (unsigned i = 0; i < branch.vertices.size(); ++i)
        {
            const float d = XMVectorGetX(XMVector3LengthSq(point(i)-point(from)));
            if (d > distance) {distance = d; best = i;}
        }
        return best;
    };
    const unsigned first = farthest(0), last = farthest(first);
    if (first == last) return fit;
    const auto axis = XMVector3Normalize(point(last)-point(first));
    const auto length = XMVectorGetX(XMVector3Length(point(last)-point(first)));
    auto u = XMVectorZero();
    float radius = 0;
    for (unsigned i = 0; i < branch.vertices.size(); ++i)
    {
        auto v = point(i)-point(first);
        v -= axis*XMVector3Dot(v,axis);
        const auto r = XMVectorGetX(XMVector3LengthSq(v));
        if (r > radius) {radius = r; u = v;}
    }
    if (radius == 0) return fit;
    u = XMVector3Normalize(u);
    const auto v = XMVector3Cross(axis,u);
    const auto face = [&](unsigned a,unsigned b,unsigned c) {
        auto n = XMVector3Cross(point(b)-point(a),point(c)-point(a));
        if (XMVectorGetX(XMVector3LengthSq(n)) < 1.e-22f) return;
        const auto average = XMLoadFloat3(&fit.vertices[a].normal)+XMLoadFloat3(&fit.vertices[b].normal)+XMLoadFloat3(&fit.vertices[c].normal);
        if (XMVectorGetX(XMVector3Dot(n,average)) < 0) std::swap(b,c);
        fit.indices.insert(fit.indices.end(),{a,b,c});
    };
    std::vector<unsigned> previous(sides,first), current(sides);
    for (unsigned ring = 0; ring < rings; ++ring)
    {
        const float z = (ring+0.5f)*length/rings;
        for (unsigned side = 0; side < sides; ++side)
        {
            const float angle = XM_2PI*side/sides;
            const auto direction = u*std::cos(angle)+v*std::sin(angle);
            float best = -INFINITY;
            for (unsigned i = 0; i < branch.vertices.size(); ++i)
            {
                const auto p = point(i)-point(first);
                const float dz = XMVectorGetX(XMVector3Dot(p,axis))-z;
                const float score = XMVectorGetX(XMVector3Dot(p,direction))-dz*dz*rings/length;
                if (score > best) {current[side] = i; best = score;}
            }
        }
        const auto edges = sides == 2 ? 1u : sides;
        for (unsigned j = 0; j < edges; ++j)
        {
            const auto k = (j+1)%sides;
            face(previous[j],current[j],current[k]);
            face(previous[j],current[k],previous[k]);
            if (ring+1 == rings) face(current[j],last,current[k]);
        }
        previous.swap(current);
    }
    return fit;
}

inline std::vector<MeshData> candidates(const std::vector<MeshData>& branches)
{
    // An error limit per connected piece lets one curved twig keep an extra
    // ring while its nearly straight neighbors use their cheapest viable cut.
    constexpr std::array limits{0.004f,0.007f,0.01f,0.0125f,0.015f,0.018f,0.022f,0.028f};
    std::vector<MeshData> results(limits.size());
    for (const auto& branch : branches)
    {
        struct Option { MeshData mesh; float error; };
        std::vector<Option> options;
        const auto reference = surface_distance::surface(branch.vertices,branch.indices);
        const auto consider = [&](MeshData fit) {
            if (fit.indices.empty() || fit.indices.size() >= branch.indices.size()) return;
            const auto reduced = surface_distance::surface(fit.vertices,fit.indices);
            const auto error = std::max(surface_distance::sampledDistance(reference,reduced,4),
                                        surface_distance::sampledDistance(reduced,reference,4));
            options.push_back({std::move(fit),error});
        };
        for (float fraction : {0.04f,0.08f,0.12f,0.18f,0.24f,0.32f,0.4f,0.52f,0.65f,0.80f})
            consider(reducePiece(branch,fraction));
        for (unsigned rings : {1u,2u,3u,5u,8u})
            for (unsigned sides : {2u,3u,4u}) consider(fitStem(branch,rings,sides));
        for (unsigned i = 0; i < limits.size(); ++i)
        {
            const auto* best = &branch;
            for (const auto& option : options)
                if (option.error <= limits[i] && option.mesh.indices.size() < best->indices.size()) best = &option.mesh;
            append(results[i],*best);
        }
    }
    return results;
}
}
