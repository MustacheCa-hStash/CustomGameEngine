#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct FoliageGap
{
    std::vector<std::size_t> pixels;
};

std::vector<FoliageGap> enclosedFoliageGaps(std::span<const std::uint16_t> coverage, unsigned width, unsigned height,
                                           unsigned samples);
std::size_t lostFoliageGaps(std::span<const FoliageGap> gaps, std::span<const std::uint16_t> coverage,
                           unsigned samples);
