#include "FoliageComparison.h"
#include "FoliageTrace.h"
#include "VoxelCoverageOptimizer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace
{
constexpr double infinity = std::numeric_limits<double>::infinity();

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

double bruteVoxels(const VoxelFoliage& data, const TraceRay& ray, TraceInterval limits)
{
    double closest = infinity;
    for (const auto& brick : data.bricks)
    {
        auto active = brick.occupancy;
        while (active)
        {
            const int bit = std::countr_zero(active);
            const int local[]{bit % 4, bit / 4 % 4, bit / 16};
            auto entry = limits.enter;
            auto exit = std::min(limits.exit, closest);
            for (int axis = 0; axis < 3; ++axis)
            {
                const double low = data.origin[axis] + (brick.coordinate[axis] * 4 + local[axis]) * data.cellSize;
                const double high = low + data.cellSize;
                const double d = ray.direction[axis];
                if (d == 0)
                {
                    if (ray.origin[axis] < low || ray.origin[axis] >= high) exit = -infinity;
                    continue;
                }
                const double a = ((d > 0 ? low : high) - ray.origin[axis]) / d;
                const double b = ((d > 0 ? high : low) - ray.origin[axis]) / d;
                entry = std::max(entry, a);
                exit = std::min(exit, b);
            }
            if (exit > entry + 1e-12) closest = entry;
            active &= active - 1;
        }
    }
    return closest;
}

void checkVoxels()
{
    constexpr TraceInterval limits{0, 64};
    const VoxelTrace simple({0, 0, {}, 1, {{{0, 0, 0}, (std::uint64_t{1} << 1) | (std::uint64_t{1} << 21)}}});
    require(simple.nearest({{-1, 0.5, 0.5}, {1, 0, 0}}, limits) == 2, "Axis ray did not enter the first occupied cell.");
    require(simple.nearest({{1.5, 0.5, 0.5}, {-1, 0, 0}}, limits) == 0, "An inside ray did not hit at its start.");
    require(simple.nearest({{-1, -1, -1}, {1, 1, 1}}, limits) == 2, "Corner ray counted a tangential cell or skipped its next cell.");
    require(simple.nearest({{-1, 0.5, 0.5}, {1, 0, 0}}, {0, 2}) == infinity, "Ray endpoint did not clip a voxel hit.");
    require(simple.nearest({{-1, 1, 0.5}, {1, 0, 0}}, limits) == infinity, "Parallel face ray used the wrong cell ownership.");
    std::vector<VoxelTrace::OccupiedCell> cells;
    const VoxelTrace layered({0, 0, {}, 1, {{{0, 0, 0}, 3}, {{1, 0, 0}, 1}}});
    layered.appendOccupiedCells({{-1, 0.5, 0.5}, {1, 0, 0}}, {0, 5}, cells);
    require(cells.size() == 2 && cells[0].brick == 0 && cells[1].brick == 0 && cells[0].bit == 0 && cells[1].bit == 1,
            "Coverage tracing should visit both occupied cells before the occluder.");

    const VoxelFoliage data{0, 0, {0.125, 0.25, -0.125}, 0.5, {
        {{-2, 0, 0}, 0x021345670809ABCD}, {{-1, 0, 0}, 0x22345678F0F0F0F0}, {{0, -1, 0}, 0x8000000000000001},
        {{0, 0, 0}, 0x0000FFFF0000FFFF}, {{0, 0, 1}, 0xF0F0F0F000000080}, {{1, 0, 0}, 0x13579BDF02468ACE},
        {{1, 0, 1}, 0x0000000000000101}, {{2, 0, 1}, 0xFFFFFFFFFFFFFFFF}}};
    const VoxelTrace trace(data);
    std::minstd_rand random(2027);
    const auto unit = [&] { return double(random() - random.min()) / (random.max() - random.min()); };
    unsigned hits = 0;
    for (int i = 0; i < 6000; ++i)
    {
        TraceRay ray;
        for (int axis = 0; axis < 3; ++axis)
        {
            ray.origin[axis] = (axis == 0 ? -5 : -3) + unit() * (axis == 0 ? 12 : 8);
            ray.direction[axis] = -1 + 2 * unit();
        }
        const TraceInterval range{unit() * 0.2, 0.2 + unit() * 12};
        const auto got = trace.nearest(ray, range);
        const auto expected = bruteVoxels(data, ray, range);
        require(got == expected || std::abs(got - expected) < 1e-11, "Brick traversal disagrees with occupied-box reference.");
        hits += std::isfinite(expected);
    }
    require(hits > 200, "Brute-force fixture generated too few hits to exercise traversal.");
}

void checkTriangles()
{
    std::vector<TraceTriangle> triangles;
    for (double z : {8., 6., 4., 2., -2.})
    {
        triangles.push_back({{{0, 0, z}, {2, 0, z}, {2, 2, z}}});
        triangles.push_back({{{0, 0, z}, {0, 2, z}, {2, 2, z}}});
    }
    const TriangleTrace trace(std::move(triangles));
    constexpr TraceRay onSharedEdge{{1, 1, 0}, {0, 0, 1}};
    require(trace.nearest(onSharedEdge, {0, 9}) == 2, "Nearest triangle or shared-edge coverage is wrong.");
    require(trace.nearest(onSharedEdge, {3, 9}) == 4, "Triangle near clip was ignored.");
    require(trace.nearest(onSharedEdge, {0, 2}) == infinity, "Triangle far clip was ignored.");
    require(trace.nearest({{1, 1, 9}, {0, 0, -1}}, {0, 9}) == 1, "Triangles were not hit from both sides.");
}

void checkCoverageFit()
{
    const VoxelFoliage eligible{7, 0, {}, 1, {{{0, 0, 0}, 3}, {{1, 0, 0}, 3}}};
    const VoxelFoliage seed{7, 0, {}, 1, {{{0, 0, 0}, 3}}};
    VoxelCoverageView view{4, {2, 0, 1}, std::vector<std::vector<std::uint32_t>>(128)};
    view.cellRays[0] = {0, 1, 4};
    view.cellRays[1] = {0, 1};
    view.cellRays[64] = {0, 1, 8};
    view.cellRays[65] = {0};
    const auto result = fitVoxelCoverage(eligible, seed, std::span(&view, 1), 1);
    require(result.seedErrors.front() == 2.0 / 3.0, "Overlapping voxel cells counted the same rays twice.");
    require(result.optimizedErrors.front() == 0 && result.foliage.bricks == std::vector<VoxelBrick>{{{1, 0, 0}, 1}} &&
                result.brickSwaps == 1, "Coverage fit did not find the improving brick replacement at the cap.");
}

void checkGaps()
{
    constexpr unsigned width = 7, height = 6, samples = 4;
    std::vector<std::uint16_t> coverage(width * height, samples);
    for (auto i : {0, 1, 7, 8, 12, 16, 17, 23, 24}) coverage[i] = 0;
    const auto gaps = enclosedFoliageGaps(coverage, width, height, samples);
    require(gaps.size() == 1 && gaps.front().pixels.size() == 4, "Only the enclosed four-pixel hole should count.");
    coverage[16] = coverage[17] = 4;
    require(lostFoliageGaps(gaps, coverage, samples) == 0, "Half a hole is still present.");
    coverage[23] = 4;
    require(lostFoliageGaps(gaps, coverage, samples) == 1, "The mostly covered hole should count as closed.");
}
}

int main()
{
    try
    {
        checkVoxels();
        checkTriangles();
        checkGaps();
        checkCoverageFit();
        std::cout << "Foliage trace and coverage fixtures passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FoliageTraceTests: " << error.what() << '\n';
        return 1;
    }
}
