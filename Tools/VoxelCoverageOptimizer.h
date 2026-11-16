#pragma once

#include "VoxelFoliage.h"

#include <span>

struct VoxelCoverageView
{
    unsigned samplesPerPixel;
    std::vector<std::uint16_t> sourceCoverage;
    // Indexed by brick * 64 + bit. Ray indices within a view are pixel * samplesPerPixel + sample.
    std::vector<std::vector<std::uint32_t>> cellRays;
};

struct VoxelCoverageFit
{
    VoxelFoliage foliage;
    std::vector<double> seedErrors;
    std::vector<double> optimizedErrors;
    std::size_t brickSwaps;
};

VoxelCoverageFit fitVoxelCoverage(const VoxelFoliage& eligible, const VoxelFoliage& seed,
                                  std::span<const VoxelCoverageView> views, std::size_t brickBudget);
