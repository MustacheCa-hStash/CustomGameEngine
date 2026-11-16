#include "VoxelCoverageOptimizer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace
{
constexpr unsigned cellsPerBrick = 64;
constexpr double minimumImprovement = 1e-10;

template<class Apply>
void forEachBit(std::uint64_t bits, const Apply& apply)
{
    while (bits)
    {
        apply(static_cast<unsigned>(std::countr_zero(bits)));
        bits &= bits - 1;
    }
}

struct ViewState
{
    const VoxelCoverageView& input;
    std::vector<std::uint32_t> rayHits;
    std::vector<int> coverage;
    std::uint64_t sourceHits;
    std::int64_t difference;
    std::int64_t coverageDelta;

    explicit ViewState(const VoxelCoverageView& view)
        : input(view), rayHits(view.sourceCoverage.size() * view.samplesPerPixel),
          coverage(view.sourceCoverage.size()),
          sourceHits(std::accumulate(view.sourceCoverage.begin(), view.sourceCoverage.end(), std::uint64_t{0})),
          difference(static_cast<std::int64_t>(sourceHits)), coverageDelta(-difference)
    {
        if (!sourceHits || !view.samplesPerPixel) throw std::runtime_error("Coverage fitting requires visible source foliage.");
    }

    void applyCell(std::size_t cell, bool add)
    {
        for (const auto ray : input.cellRays[cell])
        {
            auto& hits = rayHits[ray];
            const bool changed = add ? ++hits == 1 : hits-- == 1;
            if (!changed) continue;
            const auto pixel = ray / input.samplesPerPixel;
            const int target = input.sourceCoverage[pixel];
            auto& covered = coverage[pixel];
            difference -= std::abs(covered - target);
            covered += add ? 1 : -1;
            coverageDelta += add ? 1 : -1;
            difference += std::abs(covered - target);
        }
    }

    double error() const { return double(difference) / sourceHits; }
    double absoluteBias() const { return double(std::abs(coverageDelta)) / sourceHits; }
};

class CoverageFit
{
public:
    CoverageFit(const VoxelFoliage& eligible, std::span<const VoxelCoverageView> views, std::size_t budget)
        : eligible_(eligible), masks_(eligible.bricks.size()), budget_(budget)
    {
        states_.reserve(views.size());
        for (const auto& view : views)
        {
            if (view.cellRays.size() != eligible.bricks.size() * cellsPerBrick)
                throw std::runtime_error("Coverage rays do not match the eligible voxel grid.");
            const auto rayCount = view.sourceCoverage.size() * view.samplesPerPixel;
            for (const auto& rays : view.cellRays)
                if (std::ranges::any_of(rays, [rayCount](std::uint32_t ray) { return ray >= rayCount; }))
                    throw std::runtime_error("Coverage ray lies outside its view.");
            states_.emplace_back(view);
        }
    }

    void setMask(std::size_t brick, std::uint64_t mask)
    {
        const auto previous = masks_[brick];
        forEachBit(previous ^ mask, [&](unsigned bit) {
            const bool add = (mask & (std::uint64_t{1} << bit)) != 0;
            for (auto& state : states_) state.applyCell(brick * cellsPerBrick + bit, add);
        });
        if (previous && !mask) --occupiedBricks_;
        if (!previous && mask) ++occupiedBricks_;
        masks_[brick] = mask;
    }

    std::vector<double> errors() const
    {
        std::vector<double> result;
        for (const auto& state : states_) result.push_back(state.error());
        return result;
    }

    double score() const
    {
        double sum = 0, worst = 0, worstBias = 0;
        for (const auto& state : states_)
        {
            const auto error = state.error();
            sum += error;
            worst = std::max(worst, error);
            worstBias = std::max(worstBias, state.absoluteBias());
        }
        return sum / states_.size() + worst + worstBias;
    }

    void refineCells()
    {
        constexpr unsigned maxPasses = 16;
        for (unsigned pass = 0; pass < maxPasses; ++pass)
        {
            const auto previous = score();
            for (std::size_t brick = 0; brick < masks_.size(); ++brick)
                if (masks_[brick] || occupiedBricks_ < budget_) refineBrick(brick);
            if (score() >= previous - minimumImprovement) break;
        }
    }

    bool swapBrick()
    {
        if (occupiedBricks_ < budget_) return false;
        struct Removal { std::size_t brick; double score; };
        std::vector<Removal> removals;
        for (std::size_t brick = 0; brick < masks_.size(); ++brick)
        {
            const auto mask = masks_[brick];
            if (!mask) continue;
            setMask(brick, 0);
            removals.push_back({brick, score()});
            setMask(brick, mask);
        }
        std::stable_sort(removals.begin(), removals.end(), [](const Removal& a, const Removal& b) { return a.score < b.score; });
        auto bestScore = score();
        std::size_t remove = 0, add = 0;
        std::uint64_t bestMask = 0;
        // Try replacing each of the four least harmful bricks with any currently empty brick.
        const auto candidates = std::span(removals).first(std::min<std::size_t>(4, removals.size()));
        for (const auto& removal : candidates)
        {
            const auto mask = masks_[removal.brick];
            setMask(removal.brick, 0);
            for (std::size_t brick = 0; brick < masks_.size(); ++brick)
            {
                if (masks_[brick] || brick == removal.brick) continue;
                refineBrick(brick);
                const auto trial = score();
                if (masks_[brick] && trial < bestScore - minimumImprovement)
                {
                    bestScore = trial;
                    remove = removal.brick;
                    add = brick;
                    bestMask = masks_[brick];
                }
                setMask(brick, 0);
            }
            setMask(removal.brick, mask);
        }
        if (!bestMask) return false;
        setMask(remove, 0);
        setMask(add, bestMask);
        return true;
    }

    VoxelFoliage result() const
    {
        VoxelFoliage result{eligible_.sourceFingerprint, eligible_.meshIndex, eligible_.origin, eligible_.cellSize, {}};
        for (std::size_t brick = 0; brick < masks_.size(); ++brick)
            if (masks_[brick]) result.bricks.push_back({eligible_.bricks[brick].coordinate, masks_[brick]});
        return result;
    }

private:
    void refineBrick(std::size_t brick)
    {
        forEachBit(eligible_.bricks[brick].occupancy, [&](unsigned bit) {
            const auto previous = masks_[brick];
            if (!previous && occupiedBricks_ >= budget_) return;
            const auto before = score();
            setMask(brick, previous ^ (std::uint64_t{1} << bit));
            if (score() >= before - minimumImprovement) setMask(brick, previous);
        });
    }

    const VoxelFoliage& eligible_;
    std::vector<ViewState> states_;
    std::vector<std::uint64_t> masks_;
    std::size_t budget_;
    std::size_t occupiedBricks_ = 0;
};
}

VoxelCoverageFit fitVoxelCoverage(const VoxelFoliage& eligible, const VoxelFoliage& seed,
                                  std::span<const VoxelCoverageView> views, std::size_t brickBudget)
{
    if (eligible.sourceFingerprint != seed.sourceFingerprint || eligible.meshIndex != seed.meshIndex ||
        eligible.origin != seed.origin || eligible.cellSize != seed.cellSize)
        throw std::runtime_error("Seed and eligible voxels must come from the same source grid.");
    if (views.empty() || seed.bricks.empty() || seed.bricks.size() > brickBudget)
        throw std::runtime_error("Supply fitting views and a nonempty seed within the brick budget.");

    CoverageFit fit(eligible, views, brickBudget);
    for (const auto& brick : seed.bricks)
    {
        const auto found = std::lower_bound(eligible.bricks.begin(), eligible.bricks.end(), brick.coordinate,
                                            [](const VoxelBrick& item, const VoxelCoordinate& key) {
                                                return item.coordinate < key;
                                            });
        if (found == eligible.bricks.end() || found->coordinate != brick.coordinate || (brick.occupancy & ~found->occupancy))
            throw std::runtime_error("The seed contains cells outside the eligible set.");
        fit.setMask(static_cast<std::size_t>(found - eligible.bricks.begin()), brick.occupancy);
    }
    auto seedErrors = fit.errors();
    fit.refineCells();
    constexpr std::size_t maxSwaps = 16;
    std::size_t swaps = 0;
    while (swaps < maxSwaps && fit.swapBrick())
    {
        ++swaps;
        fit.refineCells();
    }
    return {fit.result(), std::move(seedErrors), fit.errors(), swaps};
}
