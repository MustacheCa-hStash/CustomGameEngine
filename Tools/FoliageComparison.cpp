#include "FoliageComparison.h"

#include <algorithm>
#include <utility>

namespace
{
bool mostlyClear(std::uint16_t hits, unsigned samples)
{
    return 4u * hits <= samples;
}
}

std::vector<FoliageGap> enclosedFoliageGaps(std::span<const std::uint16_t> coverage, unsigned width, unsigned height,
                                           unsigned samples)
{
    std::vector<FoliageGap> gaps;
    std::vector<bool> seen(coverage.size());
    std::vector<std::size_t> component;
    for (std::size_t start = 0; start < coverage.size(); ++start)
    {
        if (seen[start] || !mostlyClear(coverage[start], samples)) continue;
        component.clear();
        const auto add = [&](std::size_t index) {
            if (seen[index] || !mostlyClear(coverage[index], samples)) return;
            seen[index] = true;
            component.push_back(index);
        };
        add(start);
        bool exterior = false;
        for (std::size_t j = 0; j < component.size(); ++j)
        {
            const auto i = component[j];
            const auto x = i % width;
            const auto y = i / width;
            if (x == 0 || y == 0 || x + 1 == width || y + 1 == height) exterior = true;
            if (x > 0) add(i - 1);
            if (x + 1 < width) add(i + 1);
            if (y > 0) add(i - width);
            if (y + 1 < height) add(i + width);
        }
        if (!exterior && component.size() >= 4) gaps.push_back({component});
    }
    return gaps;
}

std::size_t lostFoliageGaps(std::span<const FoliageGap> gaps, std::span<const std::uint16_t> coverage,
                           unsigned samples)
{
    return std::count_if(gaps.begin(), gaps.end(), [&](const FoliageGap& gap) {
        const auto stillClear = std::ranges::count_if(gap.pixels, [&](std::size_t i) {
            return mostlyClear(coverage[i], samples);
        });
        return static_cast<std::size_t>(stillClear) * 2 < gap.pixels.size();
    });
}
