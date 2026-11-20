#pragma once

#include "SurfaceDistance.h"

#include <array>
#include <bit>
#include <cstdint>

// Orthographic occupancy of one leaf, in the same seven projections for every
// candidate. Both sides of the leaf have the same silhouette.
namespace leaf_outline
{
constexpr unsigned resolution = 96;
constexpr unsigned directions = 7;
using Mask = std::array<std::uint64_t, directions * resolution * resolution / 64>;

struct Projection
{
    DirectX::XMVECTOR right, up, center;
    float scale;
};

inline auto projections(const std::vector<surface_distance::SurfaceTriangle>& source)
{
    using namespace DirectX;
    auto lo = source.front().a, hi = lo;
    for (const auto& t : source)
        for (auto p : {t.a, t.a + t.ab, t.a + t.ac})
        {
            lo = XMVectorMin(lo, p);
            hi = XMVectorMax(hi, p);
        }
    const auto center = (hi + lo) * 0.5f;
    const float scale = (resolution - 2) / XMVectorGetX(XMVector3Length(hi - lo));
    const std::array<XMFLOAT3, directions> axes{{{1,0,0},{0,1,0},{0,0,1},{1,1,1},{1,-1,1},{-1,1,1},{1,1,-1}}};
    std::array<Projection, directions> views;
    for (unsigned i = 0; i < views.size(); ++i)
    {
        const auto axis = XMVector3Normalize(XMLoadFloat3(&axes[i]));
        const auto seed = axes[i].x == 1 && axes[i].y == 0 ? XMVectorSet(0,1,0,0) : XMVectorSet(1,0,0,0);
        const auto right = XMVector3Normalize(XMVector3Cross(axis, seed));
        views[i] = {right, XMVector3Cross(axis, right), center, scale};
    }
    return views;
}

inline Mask raster(const std::vector<surface_distance::SurfaceTriangle>& triangles,
                   const std::array<Projection, directions>& views, int stride = 1)
{
    using namespace DirectX;
    Mask mask{};
    for (unsigned view = 0; view < directions; ++view)
    {
        const auto& v = views[view];
        const auto project = [&](FXMVECTOR p) -> XMFLOAT2 {
            return {resolution * 0.5f + v.scale * XMVectorGetX(XMVector3Dot(p - v.center, v.right)),
                    resolution * 0.5f + v.scale * XMVectorGetX(XMVector3Dot(p - v.center, v.up))};
        };
        const auto edge = [](XMFLOAT2 a, XMFLOAT2 b, XMFLOAT2 p) {
            return (b.x-a.x)*(p.y-a.y) - (b.y-a.y)*(p.x-a.x);
        };
        for (const auto& t : triangles)
        {
            auto a = project(t.a), b = project(t.a + t.ab), c = project(t.a + t.ac);
            const float area = edge(a,b,c);
            if (area == 0) continue;
            if (area < 0) std::swap(b,c);
            const int x0 = std::max(0, int(std::floor(std::min({a.x,b.x,c.x}))));
            const int x1 = std::min(int(resolution)-1, int(std::ceil(std::max({a.x,b.x,c.x}))));
            const int y0 = std::max(0, int(std::floor(std::min({a.y,b.y,c.y}))));
            const int y1 = std::min(int(resolution)-1, int(std::ceil(std::max({a.y,b.y,c.y}))));
            for (int y = ((y0 + stride - 1) / stride) * stride; y <= y1; y += stride)
                for (int x = ((x0 + stride - 1) / stride) * stride; x <= x1; x += stride)
                {
                    XMFLOAT2 p{x+0.5f,y+0.5f};
                    if (edge(a,b,p) < 0 || edge(b,c,p) < 0 || edge(c,a,p) < 0) continue;
                    const unsigned bit = (view * resolution + y) * resolution + x;
                    mask[bit / 64] |= std::uint64_t{1} << (bit % 64);
                }
        }
    }
    return mask;
}

inline unsigned difference(const Mask& a, const Mask& b)
{
    unsigned pixels = 0;
    for (std::size_t i = 0; i < a.size(); ++i) pixels += std::popcount(a[i] ^ b[i]);
    return pixels;
}
}
