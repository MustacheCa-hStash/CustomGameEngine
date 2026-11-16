#pragma once

#include "VoxelFoliage.h"

#include <span>

using TracePoint = std::array<double, 3>;
using TraceTriangle = std::array<TracePoint, 3>;

struct TraceRay
{
    TracePoint origin;
    TracePoint direction;
};

struct TraceInterval
{
    double enter;
    double exit;
};

struct TraceBounds
{
    TracePoint lower;
    TracePoint upper;
};

class TraceBvh
{
    friend class TriangleTrace;
    friend class VoxelTrace;

    explicit TraceBvh(std::span<const TraceBounds> bounds);

    template<class Hit>
    double closest(const TraceRay& ray, TraceInterval interval, const Hit& hit) const;

    struct Node
    {
        TraceBounds bounds;
        std::size_t first;
        std::size_t count;
    };

    void build(std::span<const TraceBounds> bounds, std::size_t first, std::size_t end);
    template<class Hit>
    void visit(std::size_t index, const TraceRay& ray, TraceInterval& limits, const Hit& hit) const;

    std::vector<Node> nodes_;
    std::vector<std::size_t> order_;
};

class TriangleTrace
{
public:
    explicit TriangleTrace(std::vector<TraceTriangle> triangles);
    double nearest(const TraceRay& ray, TraceInterval interval) const;

private:
    std::vector<TraceTriangle> triangles_;
    TraceBvh bvh_;
};

class VoxelTrace
{
public:
    struct OccupiedCell { std::size_t brick; unsigned bit; };

    explicit VoxelTrace(VoxelFoliage foliage);
    double nearest(const TraceRay& ray, TraceInterval interval) const;
    void appendOccupiedCells(const TraceRay& ray, TraceInterval interval, std::vector<OccupiedCell>& cells) const;

private:
    VoxelFoliage foliage_;
    std::vector<TraceBounds> bricks_;
    TraceBvh bvh_;
};
