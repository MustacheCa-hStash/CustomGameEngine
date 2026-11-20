#pragma once

#include "MeshData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <utility>

namespace surface_distance
{
using namespace DirectX;
struct SurfaceTriangle
{
    DirectX::XMVECTOR a, ab, ac, normal;
    float normalSquared;
};

inline SurfaceTriangle triangle(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b, DirectX::FXMVECTOR c)
{
    using namespace DirectX;
    const auto ab = b - a;
    const auto ac = c - a;
    const auto normal = XMVector3Cross(ab, ac);
    return {a, ab, ac, normal, XMVectorGetX(XMVector3LengthSq(normal))};
}

inline float segmentDistanceSquared(DirectX::FXMVECTOR point, DirectX::FXMVECTOR edge)
{
    using namespace DirectX;
    const float t = std::clamp(XMVectorGetX(XMVector3Dot(point, edge)) /
                              XMVectorGetX(XMVector3LengthSq(edge)), 0.0f, 1.0f);
    return XMVectorGetX(XMVector3LengthSq(point - t * edge));
}

inline float triangleDistanceSquared(DirectX::FXMVECTOR sample, const SurfaceTriangle& t)
{
    using namespace DirectX;
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

inline std::vector<SurfaceTriangle> surface(std::span<const Vertex> vertices, std::span<const std::uint32_t> indices)
{
    std::vector<SurfaceTriangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i < indices.size(); i += 3)
    {
        const auto a = XMLoadFloat3(&vertices[indices[i]].position);
        const auto b = XMLoadFloat3(&vertices[indices[i + 1]].position);
        const auto c = XMLoadFloat3(&vertices[indices[i + 2]].position);
        triangles.push_back(triangle(a, b, c));
    }
    return triangles;
}

class SurfaceSearch
{
public:
    explicit SurfaceSearch(const std::vector<SurfaceTriangle>& triangles)
        : triangles_(triangles), order_(triangles.size())
    {
        std::iota(order_.begin(), order_.end(), 0u);
        build(0, order_.size());
    }

    float distanceSquared(FXMVECTOR point) const
    {
        float nearest = std::numeric_limits<float>::infinity();
        visit(0, point, nearest);
        return nearest;
    }

private:
    struct Node
    {
        XMVECTOR lower;
        XMVECTOR upper;
        std::size_t first;
        std::size_t count;
    };

    void build(std::size_t first, std::size_t end)
    {
        auto lower = XMVectorReplicate(std::numeric_limits<float>::infinity());
        auto upper = -lower;
        for (auto i = first; i < end; ++i)
        {
            const auto& t = triangles_[order_[i]];
            lower = XMVectorMin(lower, XMVectorMin(t.a, XMVectorMin(t.a + t.ab, t.a + t.ac)));
            upper = XMVectorMax(upper, XMVectorMax(t.a, XMVectorMax(t.a + t.ab, t.a + t.ac)));
        }
        const auto node = nodes_.size();
        const auto count = end - first;
        nodes_.push_back({lower, upper, first, count});
        if (count <= 8) return;
        XMFLOAT3 span;
        XMStoreFloat3(&span, upper - lower);
        const unsigned axis = span.x >= span.y && span.x >= span.z ? 0 : span.y >= span.z ? 1 : 2;
        const auto centroid = [&](std::size_t i) {
            const auto& t = triangles_[i];
            return XMVectorGetByIndex(t.a + (t.ab + t.ac) / 3.0f, axis);
        };
        const auto middle = first + count / 2;
        std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + end,
                         [&](auto a, auto b) { return centroid(a) < centroid(b); });
        nodes_[node].count = 0;
        build(first, middle);
        nodes_[node].first = nodes_.size();
        build(middle, end);
    }

    float boundDistance(std::size_t index, FXMVECTOR point) const
    {
        const auto& node = nodes_[index];
        const auto outside = XMVectorMax(XMVectorZero(), XMVectorMax(node.lower - point, point - node.upper));
        return XMVectorGetX(XMVector3LengthSq(outside));
    }

    void visit(std::size_t index, FXMVECTOR point, float& nearest) const
    {
        if (boundDistance(index, point) >= nearest) return;
        const auto& node = nodes_[index];
        if (node.count)
        {
            for (auto i = node.first; i < node.first + node.count; ++i)
                nearest = std::min(nearest, triangleDistanceSquared(point, triangles_[order_[i]]));
            return;
        }
        auto left = index + 1;
        auto right = node.first;
        if (boundDistance(right, point) < boundDistance(left, point)) std::swap(left, right);
        visit(left, point, nearest);
        visit(right, point, nearest);
    }

    const std::vector<SurfaceTriangle>& triangles_;
    std::vector<std::size_t> order_;
    std::vector<Node> nodes_;
};

inline float sampledDistanceSquared(const SurfaceTriangle& triangle, const SurfaceSearch& search, unsigned subdivisions)
{
    float worst = 0;
    const auto step = 1.0f / static_cast<float>(subdivisions);
    for (unsigned u = 0; u <= subdivisions; ++u)
    {
        for (unsigned v = 0; v <= subdivisions - u; ++v)
        {
            const auto p = triangle.a + (static_cast<float>(u) * step) * triangle.ab +
                           (static_cast<float>(v) * step) * triangle.ac;
            worst = std::max(worst, search.distanceSquared(p));
        }
    }
    return worst;
}

inline float sampledDistance(const std::vector<SurfaceTriangle>& from, const std::vector<SurfaceTriangle>& to,
                             unsigned subdivisions)
{
    const SurfaceSearch search(to);
    float worst = 0;
    for (const auto& triangle : from)
        worst = std::max(worst, sampledDistanceSquared(triangle, search, subdivisions));
    return std::sqrt(worst);
}
}
