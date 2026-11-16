#include "FoliageTrace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace
{
constexpr double infinity = std::numeric_limits<double>::infinity();

TraceInterval clip(const TraceBounds& box, const TraceRay& ray, TraceInterval range)
{
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        if (ray.direction[axis] == 0)
        {
            if (ray.origin[axis] < box.lower[axis] || ray.origin[axis] > box.upper[axis]) return {infinity, -infinity};
            continue;
        }
        const double a = (box.lower[axis] - ray.origin[axis]) / ray.direction[axis];
        const double b = (box.upper[axis] - ray.origin[axis]) / ray.direction[axis];
        range.enter = std::max(range.enter, std::min(a, b));
        range.exit = std::min(range.exit, std::max(a, b));
        if (range.enter > range.exit) break;
    }
    return range;
}

void enclose(TraceBounds& box, const TraceBounds& other)
{
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        box.lower[axis] = std::min(box.lower[axis], other.lower[axis]);
        box.upper[axis] = std::max(box.upper[axis], other.upper[axis]);
    }
}

std::vector<TraceBounds> triangleBounds(std::span<const TraceTriangle> triangles)
{
    std::vector<TraceBounds> boxes;
    boxes.reserve(triangles.size());
    for (const auto& triangle : triangles)
    {
        TraceBounds bounds{triangle[0], triangle[0]};
        for (const auto& point : triangle) enclose(bounds, {point, point});
        boxes.push_back(bounds);
    }
    return boxes;
}

std::vector<TraceBounds> brickBounds(const VoxelFoliage& foliage)
{
    std::vector<TraceBounds> boxes;
    boxes.reserve(foliage.bricks.size());
    for (const auto& brick : foliage.bricks)
    {
        TraceBounds bounds;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double first = static_cast<double>(brick.coordinate[axis]) * voxelBrickSide;
            bounds.lower[axis] = foliage.origin[axis] + first * foliage.cellSize;
            bounds.upper[axis] = foliage.origin[axis] + (first + voxelBrickSide) * foliage.cellSize;
        }
        boxes.push_back(bounds);
    }
    return boxes;
}

struct TriangleRay
{
    std::size_t x, y, z;
    double sx, sy, sz;
};

TriangleRay shearedRay(const TraceRay& ray)
{
    std::size_t z = 0;
    for (std::size_t axis = 1; axis < 3; ++axis)
    {
        if (std::abs(ray.direction[axis]) > std::abs(ray.direction[z])) z = axis;
    }
    auto x = (z + 1) % 3;
    auto y = (z + 2) % 3;
    if (ray.direction[z] < 0) std::swap(x, y);
    const double inverse = 1.0 / ray.direction[z];
    return {x, y, z, ray.direction[x] * inverse, ray.direction[y] * inverse, inverse};
}

double hitTriangle(const TraceTriangle& triangle, const TraceRay& ray, const TriangleRay& shear, TraceInterval range)
{
    // Project to the ray's dominant axis, then evaluate shared edges in double precision.
    TraceTriangle projected;
    for (std::size_t i = 0; i < triangle.size(); ++i)
    {
        const auto& p = triangle[i];
        const double z = p[shear.z] - ray.origin[shear.z];
        projected[i] = {p[shear.x] - ray.origin[shear.x] - shear.sx * z,
                        p[shear.y] - ray.origin[shear.y] - shear.sy * z, z};
    }
    const auto& [a, b, c] = projected;
    const double u = c[0] * b[1] - c[1] * b[0];
    const double v = a[0] * c[1] - a[1] * c[0];
    const double w = b[0] * a[1] - b[1] * a[0];
    if (std::min({u, v, w}) < 0 && std::max({u, v, w}) > 0) return infinity;
    const double sum = u + v + w;
    if (sum == 0) return infinity;
    const double depth = shear.sz * (u * a[2] + v * b[2] + w * c[2]) / sum;
    return depth >= range.enter && depth < range.exit ? depth : infinity;
}

template<class Hit>
double walkBrick(const TraceBounds& box, std::uint64_t mask, double size, const TraceRay& ray, TraceInterval range,
                 const Hit& hit)
{
    auto interval = clip(box, ray, range);
    if (interval.enter >= interval.exit) return infinity;

    std::array<int, 3> cell{}, step{};
    TracePoint next{}, stride{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        const double direction = ray.direction[axis];
        const double grid = (ray.origin[axis] + interval.enter * direction - box.lower[axis]) / size;
        const double first = direction < 0 ? std::ceil(grid) - 1 : std::floor(grid);
        cell[axis] = static_cast<int>(std::clamp(first, 0.0, double(voxelBrickSide - 1)));
        if (direction == 0)
        {
            // A ray parallel to a shared face belongs to the positive-side cell.
            if (ray.origin[axis] >= box.upper[axis]) return infinity;
            next[axis] = stride[axis] = infinity;
            continue;
        }
        step[axis] = direction > 0 ? 1 : -1;
        const auto boundary = cell[axis] + (step[axis] > 0 ? 1 : 0);
        next[axis] = (box.lower[axis] + boundary * size - ray.origin[axis]) / direction;
        stride[axis] = size / std::abs(direction);
    }

    double depth = interval.enter;
    for (;;)
    {
        const double boundary = std::min({next[0], next[1], next[2]});
        const double tolerance = 16 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(depth));
        const auto bit = cell[0] + voxelBrickSide * (cell[1] + voxelBrickSide * cell[2]);
        if (std::min(boundary, interval.exit) > depth + tolerance && (mask & (std::uint64_t{1} << bit)) && hit(bit))
            return depth;
        if (boundary >= interval.exit) return infinity;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            if (next[axis] > boundary + tolerance) continue;
            cell[axis] += step[axis];
            if (cell[axis] < 0 || cell[axis] >= voxelBrickSide) return infinity;
            next[axis] += stride[axis];
        }
        depth = std::max(depth, boundary);
    }
}
}

TraceBvh::TraceBvh(std::span<const TraceBounds> bounds) : order_(bounds.size())
{
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    if (!bounds.empty()) build(bounds, 0, bounds.size());
}

void TraceBvh::build(std::span<const TraceBounds> boxes, std::size_t first, std::size_t end)
{
    TraceBounds bounds = boxes[order_[first]];
    for (std::size_t i = first + 1; i < end; ++i) enclose(bounds, boxes[order_[i]]);
    const auto index = nodes_.size();
    nodes_.push_back({bounds, first, end - first});
    if (end - first <= 4) return;

    std::size_t axis = 0;
    for (std::size_t i = 1; i < 3; ++i)
    {
        if (bounds.upper[i] - bounds.lower[i] > bounds.upper[axis] - bounds.lower[axis]) axis = i;
    }
    const auto center = [&](std::size_t i) { return boxes[i].lower[axis] + boxes[i].upper[axis]; };
    const auto middle = first + (end - first) / 2;
    std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + end,
                     [&](std::size_t a, std::size_t b) { return center(a) < center(b); });
    build(boxes, first, middle);
    nodes_[index].first = nodes_.size(); // Right child. Left follows this node.
    nodes_[index].count = 0;
    build(boxes, middle, end);
}

template<class Hit>
double TraceBvh::closest(const TraceRay& ray, TraceInterval limits, const Hit& hit) const
{
    if (nodes_.empty()) return infinity;
    const auto entry = clip(nodes_.front().bounds, ray, limits);
    if (entry.enter > entry.exit) return infinity;
    const double far = limits.exit;
    visit(0, ray, limits, hit);
    return limits.exit < far ? limits.exit : infinity;
}

template<class Hit>
void TraceBvh::visit(std::size_t index, const TraceRay& ray, TraceInterval& limits, const Hit& hit) const
{
    const auto& node = nodes_[index];
    if (node.count)
    {
        for (std::size_t i = 0; i < node.count; ++i)
        {
            limits.exit = std::min(limits.exit, hit(order_[node.first + i], limits));
        }
        return;
    }
    auto first = index + 1;
    auto second = node.first;
    auto firstClip = clip(nodes_[first].bounds, ray, limits);
    auto secondClip = clip(nodes_[second].bounds, ray, limits);
    if (firstClip.enter > secondClip.enter)
    {
        std::swap(first, second);
        std::swap(firstClip, secondClip);
    }
    if (firstClip.enter <= firstClip.exit) visit(first, ray, limits, hit);
    if (secondClip.enter <= secondClip.exit && secondClip.enter < limits.exit) visit(second, ray, limits, hit);
}

TriangleTrace::TriangleTrace(std::vector<TraceTriangle> triangles)
    : triangles_(std::move(triangles)), bvh_(triangleBounds(triangles_))
{
}

double TriangleTrace::nearest(const TraceRay& ray, TraceInterval interval) const
{
    const auto shear = shearedRay(ray);
    return bvh_.closest(ray, interval, [&](std::size_t index, TraceInterval limits) {
        return hitTriangle(triangles_[index], ray, shear, limits);
    });
}

VoxelTrace::VoxelTrace(VoxelFoliage foliage)
    : foliage_(std::move(foliage)), bricks_(brickBounds(foliage_)), bvh_(bricks_)
{
}

double VoxelTrace::nearest(const TraceRay& ray, TraceInterval interval) const
{
    return bvh_.closest(ray, interval, [&](std::size_t index, TraceInterval limits) {
        return walkBrick(bricks_[index], foliage_.bricks[index].occupancy, foliage_.cellSize, ray, limits,
                         [](unsigned) { return true; });
    });
}

void VoxelTrace::appendOccupiedCells(const TraceRay& ray, TraceInterval interval, std::vector<OccupiedCell>& cells) const
{
    bvh_.closest(ray, interval, [&](std::size_t index, TraceInterval limits) {
        return walkBrick(bricks_[index], foliage_.bricks[index].occupancy, foliage_.cellSize, ray, limits,
                         [&](unsigned bit) {
                             cells.push_back({index, bit});
                             return false;
                         });
    });
}
