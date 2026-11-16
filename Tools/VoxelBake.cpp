#include "VoxelBake.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace
{
using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;

Point subtract(const Point& a, const Point& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

double dot(const Point& a, const Point& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Point cross(const Point& a, const Point& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Point normalized(Point v)
{
    const double magnitude = std::sqrt(dot(v, v));
    for (auto& component : v) component /= magnitude;
    return v;
}

double boxRadius(const Point& axis, double halfSize)
{
    return halfSize * (std::abs(axis[0]) + std::abs(axis[1]) + std::abs(axis[2]));
}

constexpr double roundoff = 16 * std::numeric_limits<double>::epsilon();

bool triangleOverlapsCell(const Triangle& triangle, const Point& cellCenter, double halfSize)
{
    // The candidate range already tests the three box axes. Test the triangle normal
    // and the cross product of each triangle edge with each box axis.
    const Triangle local{subtract(triangle[0], cellCenter), subtract(triangle[1], cellCenter),
                         subtract(triangle[2], cellCenter)};
    const auto separated = [&](const Point& axis) {
        const auto [lo, hi] = std::minmax({dot(axis, local[0]), dot(axis, local[1]), dot(axis, local[2])});
        const double radius = boxRadius(axis, halfSize);
        const double tolerance = roundoff * std::max({radius, std::abs(lo), std::abs(hi)});
        return lo > radius + tolerance || hi < -radius - tolerance;
    };
    const auto e0 = subtract(triangle[1], triangle[0]);
    const auto e1 = subtract(triangle[2], triangle[1]);
    const auto e2 = subtract(triangle[0], triangle[2]);
    if (separated(cross(e0, e1))) return false;
    constexpr Point axes[]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const auto& edge : {e0, e1, e2})
    {
        for (const auto& axis : axes)
        {
            if (separated(cross(edge, axis))) return false;
        }
    }
    return true;
}

std::int32_t cellIndex(double index)
{
    if (!std::isfinite(index) || index < std::numeric_limits<std::int32_t>::min() ||
        index > std::numeric_limits<std::int32_t>::max())
    {
        throw std::runtime_error("Mesh or cell size exceeds the signed voxel coordinate range.");
    }
    return static_cast<std::int32_t>(index);
}

Point centerOf(const VoxelCoordinate& coordinate, double size)
{
    return {(coordinate[0] + 0.5) * size, (coordinate[1] + 0.5) * size, (coordinate[2] + 0.5) * size};
}

struct ProbeRay
{
    double u;
    double v;
    double enter;
    double exit;
};

struct Probe
{
    Point direction;
    Point right;
    Point up;
    std::vector<ProbeRay> rays;
};

Probe makeProbe(const std::array<int, 3>& components, unsigned samplesPerSide)
{
    const auto direction = normalized({double(components[0]), double(components[1]), double(components[2])});
    const Point reference = std::abs(direction[0]) > 0.9 ? Point{0, 1, 0} : Point{1, 0, 0};
    const auto right = normalized(cross(reference, direction));
    const auto up = cross(direction, right);
    Probe probe{direction, right, up, {}};
    const double uRadius = boxRadius(right, 0.5);
    const double vRadius = boxRadius(up, 0.5);
    for (unsigned y = 0; y < samplesPerSide; ++y)
    {
        for (unsigned x = 0; x < samplesPerSide; ++x)
        {
            const double u = uRadius * (2 * (x + 0.5) / samplesPerSide - 1);
            const double v = vRadius * (2 * (y + 0.5) / samplesPerSide - 1);
            ProbeRay ray{u, v, -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const double origin = u * right[axis] + v * up[axis];
                if (direction[axis] == 0)
                {
                    if (std::abs(origin) >= 0.5) ray.exit = ray.enter;
                    continue;
                }
                const double a = (-0.5 - origin) / direction[axis];
                const double b = (0.5 - origin) / direction[axis];
                ray.enter = std::max(ray.enter, std::min(a, b));
                ray.exit = std::min(ray.exit, std::max(a, b));
            }
            // The bounding rectangle encloses the projected cell. Skip rays outside
            // its footprint and tangencies, since they traverse no interior.
            if (ray.exit - ray.enter > roundoff) probe.rays.push_back(ray);
        }
    }
    return probe;
}

struct ProjectedTriangle
{
    Point origin;
    Point ab;
    Point ac;
    double area;
};

ProjectedTriangle project(const Triangle& triangle, const Point& center, double cellSize, const Probe& probe)
{
    const auto relative = subtract(triangle[0], center);
    const auto coordinates = [&](const Point& point) -> Point {
        return {dot(point, probe.right) / cellSize, dot(point, probe.up) / cellSize,
                dot(point, probe.direction) / cellSize};
    };
    const auto ab = coordinates(subtract(triangle[1], triangle[0]));
    const auto ac = coordinates(subtract(triangle[2], triangle[0]));
    return {coordinates(relative), ab, ac, ab[0] * ac[1] - ab[1] * ac[0]};
}

struct ProbeHit { double depth, b, c; };

std::optional<ProbeHit> hitTriangle(const ProbeRay& ray, const ProjectedTriangle& triangle)
{
    const auto& [origin, ab, ac, area] = triangle;
    const double x = ray.u - origin[0];
    const double y = ray.v - origin[1];
    const double b = x * ac[1] - y * ac[0];
    const double c = ab[0] * y - ab[1] * x;
    const double a = area - b - c;
    const double tolerance = roundoff * std::abs(area);
    if (area > 0 ? std::min({a, b, c}) < -tolerance : std::max({a, b, c}) > tolerance) return {};
    const double depth = origin[2] + (b * ab[2] + c * ac[2]) / area;
    // A face aligned with the grid is already owned by one cell in the candidate range.
    if (depth < ray.enter - roundoff || depth > ray.exit + roundoff) return {};
    return ProbeHit{depth, b / area, c / area};
}

bool usableProjection(const ProjectedTriangle& triangle)
{
    const double scale = std::abs(triangle.ab[0] * triangle.ac[1]) + std::abs(triangle.ab[1] * triangle.ac[0]);
    return std::abs(triangle.area) > roundoff * scale;
}

std::uint64_t brickBit(const VoxelCoordinate& cell, VoxelCoordinate& brick)
{
    std::array<int, 3> local{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        local[axis] = (cell[axis] % voxelBrickSide + voxelBrickSide) % voxelBrickSide;
        brick[axis] = (cell[axis] - local[axis]) / voxelBrickSide;
    }
    return std::uint64_t{1} << (local[0] + voxelBrickSide * (local[1] + voxelBrickSide * local[2]));
}

struct CellCandidates
{
    std::vector<Triangle> triangles;
    std::vector<std::size_t> sourceStarts;
    std::map<VoxelCoordinate, std::vector<std::size_t>> cells;
    std::size_t degenerate = 0;
};

CellCandidates findCells(const MeshData& mesh, double cellSize, unsigned samplesPerSide)
{
    if (!std::isfinite(cellSize) || cellSize <= 0 || !samplesPerSide || samplesPerSide > 128 ||
        mesh.indices.size() % 3 != 0)
    {
        throw std::runtime_error("Invalid voxel bake size, sample count, or triangle index count.");
    }
    CellCandidates result;
    result.triangles.reserve(mesh.indices.size() / 3);
    result.sourceStarts.reserve(mesh.indices.size() / 3);
    for (std::size_t start = 0; start < mesh.indices.size(); start += 3)
    {
        Triangle triangle;
        for (std::size_t corner = 0; corner < triangle.size(); ++corner)
        {
            const auto index = mesh.indices[start + corner];
            if (index >= mesh.vertices.size()) throw std::runtime_error("Voxel source index is out of range.");
            const auto& p = mesh.vertices[index].position;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            {
                throw std::runtime_error("Voxel source vertex is non-finite.");
            }
            triangle[corner] = {p.x, p.y, p.z};
        }
        const auto normal = cross(subtract(triangle[1], triangle[0]), subtract(triangle[2], triangle[0]));
        if (normal == Point{})
        {
            ++result.degenerate;
            continue;
        }

        VoxelCoordinate first, last;
        for (std::size_t axis = 0; axis < first.size(); ++axis)
        {
            const auto [min, max] = std::minmax({triangle[0][axis], triangle[1][axis], triangle[2][axis]});
            const double low = std::floor(min / cellSize);
            // Exclude a contact at the upper boundary of a nonflat interval.
            // Entire surfaces on a grid plane belong to its positive-side cell.
            const double high = std::max(low, std::ceil(max / cellSize) - 1);
            first[axis] = cellIndex(low);
            last[axis] = cellIndex(high);
        }
        for (std::int64_t z = first[2]; z <= last[2]; ++z)
        {
            for (std::int64_t y = first[1]; y <= last[1]; ++y)
            {
                for (std::int64_t x = first[0]; x <= last[0]; ++x)
                {
                    const VoxelCoordinate cell{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                               static_cast<std::int32_t>(z)};
                    if (triangleOverlapsCell(triangle, centerOf(cell, cellSize), cellSize * 0.5))
                    {
                        result.cells[cell].push_back(result.triangles.size());
                    }
                }
            }
        }
        result.triangles.push_back(triangle);
        result.sourceStarts.push_back(start);
    }
    return result;
}

Point interpolatedNormal(const MeshData& mesh, const CellCandidates& geometry, std::size_t index, const ProbeHit& hit)
{
    const auto start = geometry.sourceStarts[index];
    const auto& a = mesh.vertices[mesh.indices[start]].normal;
    const auto& b = mesh.vertices[mesh.indices[start + 1]].normal;
    const auto& c = mesh.vertices[mesh.indices[start + 2]].normal;
    const double wa = 1 - hit.b - hit.c;
    const Point normal{wa * a.x + hit.b * b.x + hit.c * c.x,
                       wa * a.y + hit.b * b.y + hit.c * c.y,
                       wa * a.z + hit.b * b.z + hit.c * c.z};
    const auto lengthSquared = dot(normal, normal);
    if (std::isfinite(lengthSquared) && lengthSquared > roundoff) return normalized(normal);
    const auto& p = geometry.triangles[index];
    return normalized(cross(subtract(p[1], p[0]), subtract(p[2], p[0])));
}

std::uint32_t packNormal(Point n)
{
    const double l1 = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
    const double x = n[0] / l1, y = n[1] / l1;
    const double u = n[2] < 0 ? std::copysign(1 - std::abs(y), x) : x;
    const double v = n[2] < 0 ? std::copysign(1 - std::abs(x), y) : y;
    const auto quantize = [](double value) { return static_cast<std::uint32_t>(std::lround((value * 0.5 + 0.5) * 4095)); };
    return quantize(u) | (quantize(v) << 12);
}

struct NormalSample { Point axis; double weight; int direction = -1; };
struct ReceiverSample { Point position; double weight; };

std::uint32_t packShadowReceiver(std::span<const ReceiverSample> samples)
{
    if (samples.empty()) return 0;
    Point center{};
    double totalWeight = 0;
    for (const auto& [position, weight] : samples)
    {
        for (std::size_t axis = 0; axis < center.size(); ++axis) center[axis] += weight * position[axis];
        totalWeight += weight;
    }
    for (auto& component : center) component /= totalWeight;
    // The centroid can lie in empty space. Represent it by the closest actual leaf hit.
    const auto nearest = std::ranges::min_element(samples, {}, [&](const ReceiverSample& sample) {
        const auto delta = subtract(sample.position, center);
        return dot(delta, delta);
    });
    const auto encode = [](double component) {
        return static_cast<std::uint32_t>(std::lround(31 * std::clamp(component + 0.5, 0.0, 1.0)));
    };
    // The upper half of the last word is spare. Store a validity bit and three UNORM5 coordinates.
    const auto& p = nearest->position;
    return (1u << 31) | (encode(p[0]) << 16) | (encode(p[1]) << 21) | (encode(p[2]) << 26);
}

PackedVoxelNormals fitNormals(std::span<const NormalSample> samples)
{
    const auto primary = samples.front().axis;
    const auto distant = std::ranges::min_element(samples, {}, [&](const NormalSample& sample) {
        return std::abs(dot(sample.axis, primary));
    });
    std::array<Point, 2> axes{primary, distant->axis};
    std::array<double, 2> weights{};
    for (int iteration = 0; iteration < 8; ++iteration)
    {
        std::array<Point, 2> sums{};
        weights = {};
        for (const auto& sample : samples)
        {
            const auto first = dot(sample.axis, axes[0]), second = dot(sample.axis, axes[1]);
            // For two-sided foliage, opposite signs describe the same plane.
            const std::size_t bin = std::abs(second) > std::abs(first) ? 1 : 0;
            const double signedWeight = std::copysign(sample.weight, bin ? second : first);
            for (int axis = 0; axis < 3; ++axis) sums[bin][axis] += signedWeight * sample.axis[axis];
            weights[bin] += sample.weight;
        }
        for (int bin = 0; bin < 2; ++bin)
            if (dot(sums[bin], sums[bin]) > roundoff) axes[bin] = normalized(sums[bin]);
    }
    const double pooledWeight = weights[0] / (weights[0] + weights[1]);
    const auto quantize = [](double value) { return static_cast<std::uint32_t>(std::lround(255 * value)); };
    const auto packedPooledWeight = static_cast<std::uint32_t>(std::lround(255 * weights[0] / (weights[0] + weights[1])));
    PackedVoxelNormals result{packNormal(axes[0]) | (packedPooledWeight << 24), packNormal(axes[1]), 0, 0};
    std::array<unsigned, 6> hits{}, firstAxisHits{};
    for (const auto& sample : samples)
    {
        if (sample.direction < 0) continue;
        ++hits[sample.direction];
        if (std::abs(dot(sample.axis, axes[0])) >= std::abs(dot(sample.axis, axes[1]))) ++firstAxisHits[sample.direction];
    }
    for (unsigned direction = 0; direction < hits.size(); ++direction)
    {
        // Eight prior samples from the pooled distribution stabilize sparsely hit directions.
        const double weight = (firstAxisHits[direction] + 8 * pooledWeight) / (hits[direction] + 8);
        result[2 + direction / 4] |= quantize(weight) << (8 * (direction % 4));
    }
    return result;
}
}

double VoxelBake::meanCoverage(const VoxelBakeCell& cell) const
{
    double sum = 0;
    for (std::size_t i = 0; i < raysThroughCell.size(); ++i)
    {
        sum += static_cast<double>(cell.hits[i]) / raysThroughCell[i];
    }
    return sum / raysThroughCell.size();
}

VoxelBake sampleVoxelCoverage(const MeshData& mesh, double cellSize, unsigned samplesPerSide)
{
    const auto geometry = findCells(mesh, cellSize, samplesPerSide);
    VoxelBake bake{cellSize, {}, geometry.degenerate, {}};
    std::array<Probe, voxelProbeDirections.size()> probes;
    for (std::size_t i = 0; i < probes.size(); ++i)
    {
        probes[i] = makeProbe(voxelProbeDirections[i], samplesPerSide);
        bake.raysThroughCell[i] = static_cast<unsigned>(probes[i].rays.size());
    }
    bake.cells.reserve(geometry.cells.size());
    std::vector<ProjectedTriangle> projected;
    for (const auto& [coordinate, indices] : geometry.cells)
    {
        const auto center = centerOf(coordinate, cellSize);
        VoxelBakeCell cell{coordinate, {}};
        for (std::size_t i = 0; i < probes.size(); ++i)
        {
            const auto& probe = probes[i];
            projected.clear();
            for (const auto index : indices)
            {
                const auto triangle = project(geometry.triangles[index], center, cellSize, probe);
                if (usableProjection(triangle)) projected.push_back(triangle);
            }
            for (const auto& ray : probe.rays)
            {
                const bool hit = std::any_of(projected.begin(), projected.end(),
                                             [&](const auto& triangle) { return hitTriangle(ray, triangle).has_value(); });
                cell.hits[i] += hit;
            }
        }
        bake.cells.push_back(cell);
    }
    return bake;
}

VoxelNormalBake bakeVoxelNormals(const MeshData& mesh, const VoxelFoliage& mask, unsigned samplesPerSide)
{
    if (mask.origin != std::array<double, 3>{})
        throw std::runtime_error("Normal baking expects the source voxel grid origin.");
    const auto geometry = findCells(mesh, mask.cellSize, samplesPerSide);
    std::vector<Probe> probes;
    for (const auto& direction : voxelProbeDirections) probes.push_back(makeProbe(direction, samplesPerSide));
    struct ProjectedFace { ProjectedTriangle triangle; std::size_t index; };
    std::vector<ProjectedFace> faces;
    std::vector<NormalSample> samples;
    std::vector<ReceiverSample> receivers;
    VoxelNormalBake result;
    result.cells.reserve(occupiedVoxelCount(mask));
    for (const auto& brick : mask.bricks)
    {
        for (auto bits = brick.occupancy; bits; bits &= bits - 1)
        {
            const int bit = std::countr_zero(bits);
            constexpr int side = voxelBrickSide;
            const VoxelCoordinate cell{brick.coordinate[0] * side + bit % side, brick.coordinate[1] * side + bit / side % side,
                                       brick.coordinate[2] * side + bit / (side * side)};
            const auto candidate = geometry.cells.find(cell);
            if (candidate == geometry.cells.end()) throw std::runtime_error("Occupied cell has no source faces.");
            const auto center = centerOf(cell, mask.cellSize);
            samples.clear();
            receivers.clear();
            for (std::size_t probeIndex = 0; probeIndex < probes.size(); ++probeIndex)
            {
                const auto& probe = probes[probeIndex];
                faces.clear();
                for (const auto index : candidate->second)
                {
                    const auto triangle = project(geometry.triangles[index], center, mask.cellSize, probe);
                    if (usableProjection(triangle)) faces.push_back({triangle, index});
                }
                const double weight = 1.0 / probe.rays.size();
                for (const auto& ray : probe.rays)
                {
                    std::optional<ProbeHit> front, back;
                    std::size_t frontIndex = 0, backIndex = 0;
                    for (const auto& face : faces)
                    {
                        const auto hit = hitTriangle(ray, face.triangle);
                        if (!hit) continue;
                        if (!front || hit->depth < front->depth)
                        {
                            front = hit;
                            frontIndex = face.index;
                        }
                        if (!back || hit->depth > back->depth)
                        {
                            back = hit;
                            backIndex = face.index;
                        }
                    }
                    if (front)
                    {
                        const int minusSide = probeIndex < 3 ? static_cast<int>(2 * probeIndex) : -1;
                        const int plusSide = probeIndex < 3 ? minusSide + 1 : -1;
                        samples.push_back({interpolatedNormal(mesh, geometry, frontIndex, *front), weight, minusSide});
                        samples.push_back({interpolatedNormal(mesh, geometry, backIndex, *back), weight, plusSide});
                        for (const auto depth : {front->depth, back->depth})
                        {
                            Point hit;
                            for (std::size_t axis = 0; axis < hit.size(); ++axis)
                                hit[axis] = ray.u * probe.right[axis] + ray.v * probe.up[axis] + depth * probe.direction[axis];
                            receivers.push_back({hit, weight});
                        }
                    }
                }
            }
            if (samples.empty())
            {
                ++result.fallbackCells;
                const auto representative = candidate->second.front();
                samples.push_back({interpolatedNormal(mesh, geometry, representative, {0, 1.0/3, 1.0/3}), 1});
            }
            auto packed = fitNormals(samples);
            packed[3] |= packShadowReceiver(receivers);
            result.cells.push_back(packed);
        }
    }
    return result;
}

std::vector<std::uint8_t> assignVoxelRegions(const VoxelFoliage& mask, std::span<const VoxelBake> samples)
{
    struct Best { double coverage; std::uint8_t region; };
    std::map<VoxelCoordinate, Best> best;
    for (std::size_t region = 0; region < samples.size(); ++region)
    {
        for (const auto& cell : samples[region].cells)
        {
            const auto coverage = samples[region].meanCoverage(cell);
            auto [it, added] = best.try_emplace(cell.coordinate, Best{coverage, static_cast<std::uint8_t>(region)});
            if (!added && coverage > it->second.coverage) it->second = {coverage, static_cast<std::uint8_t>(region)};
        }
    }
    std::vector<std::uint8_t> owners;
    owners.reserve(occupiedVoxelCount(mask));
    for (const auto& brick : mask.bricks)
    {
        for (auto bits = brick.occupancy; bits; bits &= bits - 1)
        {
            const int bit = std::countr_zero(bits);
            constexpr int side = voxelBrickSide;
            const VoxelCoordinate cell{brick.coordinate[0] * side + bit % side, brick.coordinate[1] * side + bit / side % side,
                                       brick.coordinate[2] * side + bit / (side * side)};
            const auto it = best.find(cell);
            if (it == best.end()) throw std::runtime_error("An occupied cell has no source triangle region.");
            owners.push_back(it->second.region);
        }
    }
    return owners;
}

VoxelFoliage packVoxelFoliage(const VoxelBake& bake, std::uint64_t fingerprint, std::uint32_t meshIndex,
                            std::optional<double> minimumCoverage)
{
    if (minimumCoverage && (!std::isfinite(*minimumCoverage) || *minimumCoverage <= 0 || *minimumCoverage > 1))
    {
        throw std::runtime_error("Voxel coverage cutoff must be greater than zero and at most one.");
    }
    std::map<VoxelCoordinate, std::uint64_t> masks;
    for (const auto& cell : bake.cells)
    {
        if (minimumCoverage && bake.meanCoverage(cell) < *minimumCoverage) continue;
        VoxelCoordinate brick;
        const auto bit = brickBit(cell.coordinate, brick);
        masks[brick] |= bit;
    }
    VoxelFoliage foliage{fingerprint, meshIndex, {}, bake.cellSize, {}};
    foliage.bricks.reserve(masks.size());
    for (const auto& [coordinate, mask] : masks) foliage.bricks.push_back({coordinate, mask});
    return foliage;
}
