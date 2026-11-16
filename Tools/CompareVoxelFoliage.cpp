#include "D3D12Renderer.h"
#include "FoliageComparison.h"
#include "FoliageTrace.h"
#include "SourceFingerprint.h"
#include "VoxelBake.h"
#include "VoxelCoverageOptimizer.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <span>
#include <stdexcept>
#include <string_view>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double depthImageRange = 0.2;

struct ImageRegion
{
    unsigned width, height;
    unsigned viewportWidth, viewportHeight;
    float nearPlane, farPlane;
};

struct Options
{
    std::vector<std::filesystem::path> bakeDirectories;
    unsigned samplesPerSide = 2;
    bool imagePosesOnly = false;
    bool farRoute = false;
    bool coverageReview = false;
    bool nearFit = false;
    std::filesystem::path eligibleVoxels, seedVoxels;
    std::size_t brickBudget = 0;
    std::optional<unsigned> regionAb;
};

Options parseOptions(int argc, wchar_t* argv[])
{
    Options options;
    for (int i = 3; i < argc; ++i)
    {
        const std::wstring_view value(argv[i]);
        if (value == L"--image-poses-only") options.imagePosesOnly = true;
        else if (value == L"--far") options.farRoute = true;
        else if (value == L"--coverage-review") options.coverageReview = true;
        else if (value == L"--near-fit") options.nearFit = true;
        else if (value == L"--region-ab")
        {
            if (options.regionAb || ++i == argc)
                throw std::runtime_error("Supply --region-ab with one region index.");
            const std::wstring_view region(argv[i]);
            if (region.size() != 1 || region[0] < L'0' ||
                region[0] >= L'0' + static_cast<wchar_t>(voxelBranchRegionCount))
                throw std::runtime_error("The region index is out of range.");
            options.regionAb = static_cast<unsigned>(region[0] - L'0');
        }
        else if (value == L"--optimize")
        {
            if (!options.eligibleVoxels.empty() || i + 2 >= argc)
                throw std::runtime_error("Supply --optimize with the eligible and seed voxel files.");
            options.eligibleVoxels = argv[++i];
            options.seedVoxels = argv[++i];
        }
        else if (value == L"--brick-budget")
        {
            if (++i == argc) throw std::runtime_error("Supply --brick-budget with a positive brick count.");
            const std::wstring count = argv[i];
            if (count.empty() || count.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::runtime_error("Supply --brick-budget with a positive brick count.");
            options.brickBudget = std::stoull(count);
            if (!options.brickBudget) throw std::runtime_error("The brick budget must be positive.");
        }
        else if (value == L"--samples")
        {
            if (++i == argc) throw std::runtime_error("Supply --samples with an integer from 1 through 8.");
            const std::wstring_view side(argv[i]);
            if (side.size() != 1 || side[0] < L'1' || side[0] > L'8')
                throw std::runtime_error("Supply --samples with an integer from 1 through 8.");
            options.samplesPerSide = static_cast<unsigned>(side[0] - L'0');
        }
        else if (value.starts_with(L"--")) throw std::runtime_error("Unknown comparison option.");
        else options.bakeDirectories.emplace_back(argv[i]);
    }
    if (options.regionAb)
    {
        if (!options.eligibleVoxels.empty() || !options.bakeDirectories.empty() || options.brickBudget ||
            options.imagePosesOnly || options.farRoute || options.coverageReview || options.nearFit)
            throw std::runtime_error("The region comparison only accepts --region-ab and --samples.");
    }
    else if (!options.eligibleVoxels.empty())
    {
        if (!options.bakeDirectories.empty() || options.farRoute || options.coverageReview || options.imagePosesOnly)
            throw std::runtime_error("Optimization uses its own fitting route and accepts no bake directories.");
    }
    else if (options.bakeDirectories.empty() || options.brickBudget || options.nearFit)
        throw std::runtime_error("Supply voxel bake directories for comparison or --optimize for fitting.");
    return options;
}

struct Pose
{
    const char* name;
    const char* route;
    float distance;
    float yaw;
    float pitch;
    bool images = false;
    double sampleOffsetX = 0;
    double sampleOffsetY = 0;
};

constexpr Pose poses[]{
    {"retreat-018", "retreat", 18, -125, 68}, {"retreat-026", "retreat", 26, -125, 68},
    {"retreat-040", "retreat", 40, -125, 68, true}, {"retreat-060", "retreat", 60, -125, 68},
    {"retreat-090", "retreat", 90, -125, 68}, {"retreat-120", "retreat", 120, -125, 68, true},
    {"retreat-180", "retreat", 180, -125, 68}, {"retreat-240", "retreat", 240, -125, 68},
    {"orbit-01", "orbit", 60, -147, 34}, {"orbit-02", "orbit", 60, -141, 34},
    {"orbit-03", "orbit", 60, -135, 34}, {"orbit-04", "orbit", 60, -129, 34, true},
    {"orbit-05", "orbit", 60, -123, 34}, {"orbit-06", "orbit", 60, -117, 34},
    {"orbit-07", "orbit", 60, -111, 34}, {"orbit-08", "orbit", 60, -105, 34},
    {"holdout-side", "holdout", 55, 32, 8, true}, {"holdout-back", "holdout", 105, 71, 61, true}};

constexpr Pose farPoses[]{
    {"retreat-0300", "retreat", 300, -125, 68, true}, {"retreat-0600", "retreat", 600, -125, 68, true},
    {"retreat-1200", "retreat", 1200, -125, 68, true}, {"retreat-2400", "retreat", 2400, -125, 68, true},
    {"retreat-3000", "retreat", 3000, -125, 68, true},
    {"offset-3000-a", "offset", 3000, -125, 68, true, 0.25, 0.25},
    {"offset-3000-b", "offset", 3000, -125, 68, true, 0.50, 0.50},
    {"side-1200", "holdout", 1200, 32, 8, true}, {"back-1200", "holdout", 1200, 71, 61, true},
    {"side-3000", "holdout", 3000, 32, 8, true}, {"back-3000", "holdout", 3000, 71, 61, true}};

constexpr Pose fittingPoses[]{
    {"fit-01", "fit", 300, -117, 65}, {"fit-02", "fit", 340, -35, 26},
    {"fit-03", "fit", 380, 28, 19}, {"fit-04", "fit", 410, 127, 42},
    {"fit-05", "fit", 575, -136, 54}, {"fit-06", "fit", 635, 54, 50},
    {"fit-07", "fit", 730, 148, 17}, {"fit-08", "fit", 800, -63, 39},
    {"fit-09", "fit", 1050, -112, 71}, {"fit-10", "fit", 1150, 22, 11},
    {"fit-11", "fit", 1450, 80, 56}, {"fit-12", "fit", 1680, -167, 28},
    {"fit-13", "fit", 2400, 44, 24}, {"fit-14", "fit", 2675, -121, 61},
    {"fit-15", "fit", 2890, 68, 74}, {"fit-16", "fit", 2950, 136, 33}};

constexpr Pose nearFittingPoses[]{
    {"fit-01", "fit", 155, -120, 64}, {"fit-02", "fit", 170, 67, 57},
    {"fit-03", "fit", 165, 23, 3}, {"fit-04", "fit", 190, 40, 14},
    {"fit-05", "fit", 250, -44, 26}, {"fit-06", "fit", 310, 132, 31},
    {"fit-07", "fit", 320, 24, 12}, {"fit-08", "fit", 475, 39, 5},
    {"fit-09", "fit", 460, -131, 70}, {"fit-10", "fit", 420, 76, 65},
    {"fit-11", "fit", 690, -116, 59}, {"fit-12", "fit", 790, -159, 40},
    {"fit-13", "fit", 850, 26, 3}, {"fit-14", "fit", 1350, 48, 17},
    {"fit-15", "fit", 1650, 37, 13}, {"fit-16", "fit", 2100, 68, 74}};

constexpr Pose coverageReviewPoses[]{
    {"retreat-0150", "retreat", 150, -125, 68, true}, {"retreat-0500", "retreat", 500, -125, 68, true},
    {"retreat-0800", "retreat", 800, -125, 68, true}, {"retreat-3000", "retreat", 3000, -125, 68, true},
    {"side-0150", "side", 150, 32, 8, true}, {"side-0375", "side", 375, 32, 8, true},
    {"side-0800", "side", 800, 32, 8, true}, {"side-1800", "side", 1800, 32, 8, true},
    {"side-3000", "side", 3000, 32, 8, true}, {"side-orbit-a", "side", 375, 28, 9, true},
    {"side-orbit-b", "side", 375, 36, 7, true, 0.25, 0.25},
    {"rear-0150", "rear", 150, 71, 61, true}, {"rear-0375", "rear", 375, 71, 61, true},
    {"rear-0800", "rear", 800, 71, 61, true}};

constexpr ImageRegion farRegion{192, 192, 3200, 1800, branchVoxelFarNearPlane, branchVoxelFarClipPlane};

struct BakeVariant
{
    std::string name;
    std::uintmax_t bytes;
    double cellSize;
    std::size_t brickCount;
    VoxelTrace trace;
};

std::vector<BakeVariant> readBakes(const Options& options, std::uint64_t fingerprint, std::uint32_t meshIndex)
{
    std::vector<BakeVariant> result;
    for (std::size_t i = 0; i < options.bakeDirectories.size(); ++i)
    {
        const auto& directory = options.bakeDirectories[i];
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".voxels") paths.push_back(entry.path());
        }
        if (paths.empty()) throw std::runtime_error("A bake directory contains no .voxels files.");
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths)
        {
            auto data = readVoxelFoliage(path);
            if (data.sourceFingerprint != fingerprint || data.meshIndex != meshIndex)
                throw std::runtime_error("A voxel bake belongs to a different source mesh or GLB.");
            auto name = "bake-" + std::to_string(i + 1) + "-" + path.stem().string();
            if (name.find_first_of(",\r\n\"") != std::string::npos)
                throw std::runtime_error("Voxel filenames must be usable as CSV and image labels.");
            result.push_back({std::move(name), std::filesystem::file_size(path), data.cellSize, data.bricks.size(),
                              VoxelTrace(std::move(data))});
        }
    }
    return result;
}

TracePoint point(FXMVECTOR v)
{
    return {XMVectorGetX(v), XMVectorGetY(v), XMVectorGetZ(v)};
}

void appendTriangles(std::vector<TraceTriangle>& into, std::span<const Vertex> vertices,
                     std::span<const std::uint32_t> indices, FXMMATRIX matrix)
{
    std::vector<TracePoint> points;
    points.reserve(vertices.size());
    for (const auto& vertex : vertices)
        points.push_back(point(XMVector3TransformCoord(XMLoadFloat3(&vertex.position), matrix)));
    into.reserve(into.size() + indices.size() / 3);
    for (std::size_t i = 0; i < indices.size(); i += 3)
        into.push_back({points[indices[i]], points[indices[i + 1]], points[indices[i + 2]]});
}

TriangleTrace traceOtherMeshes(const ViewerScene& scene, std::size_t meshIndex, FXMMATRIX worldToMesh)
{
    std::vector<TraceTriangle> triangles;
    for (std::size_t i = 0; i < scene.asset->meshes.size(); ++i)
    {
        if (i == meshIndex) continue;
        const auto& part = scene.asset->meshes[i];
        appendTriangles(triangles, part.vertices, part.indices, XMLoadFloat4x4(&part.localToWorld) * worldToMesh);
    }
    return TriangleTrace(std::move(triangles));
}

struct Crop
{
    unsigned x, y, width, height, stride;
};

Crop extractCrop(const std::vector<std::uint16_t>& reference, unsigned width, unsigned height)
{
    unsigned left = width, top = height, right = 0, bottom = 0;
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        if (!reference[i]) continue;
        const auto x = static_cast<unsigned>(i % width), y = static_cast<unsigned>(i / width);
        left = std::min(left, x);
        right = std::max(right, x + 1);
        top = std::min(top, y);
        bottom = std::max(bottom, y + 1);
    }
    if (left >= right || top >= bottom) throw std::runtime_error("Comparison pose has no foliage in view.");
    constexpr unsigned padding = 12;
    left -= std::min(left, padding);
    top -= std::min(top, padding);
    right = std::min(right + padding, width);
    bottom = std::min(bottom + padding, height);
    return {left, top, right - left, bottom - top, width};
}

struct RaySample
{
    TracePoint direction;
    TraceInterval interval;
    double source;
};

struct ReferenceFrame
{
    TracePoint origin;
    std::vector<RaySample> rays;
    std::vector<std::uint16_t> coverage;
    std::vector<FoliageGap> gaps;
    std::uint64_t hits = 0;
    unsigned width, height;
};

ReferenceFrame makeReference(const CameraView& camera, FXMMATRIX worldToMesh, const ImageRegion& region,
                             const Pose& pose, unsigned samplesPerSide, const TriangleTrace& source,
                             const TriangleTrace& bark)
{
    const auto view = camera.matrix();
    const auto viewToMesh = XMMatrixInverse(nullptr, XMLoadFloat4x4(&view)) * worldToMesh;
    const auto [width, height, viewportWidth, viewportHeight, nearPlane, farPlane] = region;
    const double offsetX = (viewportWidth - width) / 2.0 + pose.sampleOffsetX;
    const double offsetY = (viewportHeight - height) / 2.0 + pose.sampleOffsetY;
    ReferenceFrame frame{point(viewToMesh.r[3]), {}, std::vector<std::uint16_t>(width * height), {}, 0, width, height};
    frame.rays.reserve(static_cast<std::size_t>(width) * height * samplesPerSide * samplesPerSide);
    const auto right = point(viewToMesh.r[0]), up = point(viewToMesh.r[1]), forward = point(-viewToMesh.r[2]);
    const double scaleY = std::tan(D3D12Renderer::cameraVerticalFov * 0.5);
    const double scaleX = scaleY * viewportWidth / viewportHeight;
    for (unsigned y = 0; y < height; ++y)
    {
        for (unsigned x = 0; x < width; ++x)
        {
            auto& hits = frame.coverage[y * width + x];
            for (unsigned v = 0; v < samplesPerSide; ++v)
            {
                const double dy = (1 - 2 * (offsetY + y + (v + 0.5) / samplesPerSide) / viewportHeight) * scaleY;
                for (unsigned u = 0; u < samplesPerSide; ++u)
                {
                    const double dx = (2 * (offsetX + x + (u + 0.5) / samplesPerSide) / viewportWidth - 1) * scaleX;
                    const double length = std::sqrt(1 + dx * dx + dy * dy);
                    TraceRay ray{frame.origin, {}};
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        ray.direction[axis] = (forward[axis] + dx * right[axis] + dy * up[axis]) / length;
                    TraceInterval interval{nearPlane * length, farPlane * length};
                    interval.exit = std::min(interval.exit, bark.nearest(ray, interval));
                    const double reference = source.nearest(ray, interval);
                    hits += std::isfinite(reference);
                    frame.rays.push_back({ray.direction, interval, reference});
                }
            }
            frame.hits += hits;
        }
    }
    frame.gaps = enclosedFoliageGaps(frame.coverage, width, height, samplesPerSide * samplesPerSide);
    if (!frame.hits) throw std::runtime_error("Comparison pose has no source foliage in view.");
    return frame;
}

VoxelCoverageView collectFitRays(ReferenceFrame&& reference, const VoxelTrace& eligible, std::size_t brickCount,
                                 unsigned samplesPerPixel)
{
    VoxelCoverageView view{samplesPerPixel, std::move(reference.coverage), {}};
    view.cellRays.resize(brickCount * 64);
    std::vector<VoxelTrace::OccupiedCell> cells;
    for (std::uint32_t index = 0; index < reference.rays.size(); ++index)
    {
        const auto& sample = reference.rays[index];
        cells.clear();
        eligible.appendOccupiedCells({reference.origin, sample.direction}, sample.interval, cells);
        for (const auto cell : cells) view.cellRays[cell.brick * 64 + cell.bit].push_back(index);
    }
    return view;
}

void optimizeBake(const Options& options, const ViewerScene& scene, const std::filesystem::path& output)
{
    const auto eligible = readVoxelFoliage(options.eligibleVoxels);
    const auto seed = readVoxelFoliage(options.seedVoxels);
    const auto fingerprint = sourceFingerprint(scene.source);
    if (eligible.sourceFingerprint != fingerprint || eligible.meshIndex >= scene.asset->meshes.size())
        throw std::runtime_error("The eligible voxel bake belongs to a different source mesh or GLB.");
    const auto& mesh = scene.asset->meshes[eligible.meshIndex];
    const auto worldToMesh = XMMatrixInverse(nullptr, XMLoadFloat4x4(&mesh.localToWorld));
    std::vector<TraceTriangle> triangles;
    appendTriangles(triangles, mesh.vertices, mesh.indices, XMMatrixIdentity());
    const TriangleTrace source(std::move(triangles));
    const auto bark = traceOtherMeshes(scene, eligible.meshIndex, worldToMesh);
    const VoxelTrace eligibleTrace(eligible);
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const auto samples = options.samplesPerSide;
    std::vector<VoxelCoverageView> views;
    const auto route = options.nearFit ? std::span<const Pose>(nearFittingPoses) : std::span<const Pose>(fittingPoses);
    for (const auto& pose : route)
    {
        const CameraView camera{focus, XMConvertToRadians(pose.yaw), XMConvertToRadians(pose.pitch), pose.distance};
        auto reference = makeReference(camera, worldToMesh, farRegion, pose, samples, source, bark);
        views.push_back(collectFitRays(std::move(reference), eligibleTrace, eligible.bricks.size(), samples * samples));
    }
    const auto budget = options.brickBudget ? options.brickBudget : seed.bricks.size();
    const auto result = fitVoxelCoverage(eligible, seed, views, budget);
    std::filesystem::create_directories(output);
    const auto path = output / "coverage-optimized.voxels";
    writeVoxelFoliage(path, result.foliage);
    if (readVoxelFoliage(path) != result.foliage)
        throw std::runtime_error("Coverage-optimized bake round-trip mismatch.");
    std::ofstream report(output / "fit.csv");
    report.exceptions(std::ios::badbit | std::ios::failbit);
    report.imbue(std::locale::classic());
    report << "pose,distance,yaw,pitch,samples_per_pixel,brick_budget,seed_bricks,optimized_bricks,brick_swaps,"
              "seed_resolved_l1_over_source,optimized_resolved_l1_over_source\n" << std::setprecision(10);
    for (std::size_t i = 0; i < route.size(); ++i)
    {
        const auto& pose = route[i];
        report << pose.name << ',' << pose.distance << ',' << pose.yaw << ',' << pose.pitch << ',' << samples * samples
               << ',' << budget << ',' << seed.bricks.size() << ',' << result.foliage.bricks.size() << ',' << result.brickSwaps
               << ',' << result.seedErrors[i] << ',' << result.optimizedErrors[i] << '\n';
    }
    report.close();
    std::cout << "Wrote " << path << " with " << result.foliage.bricks.size() << " bricks and "
              << result.brickSwaps << " brick swaps.\n";
}

struct Pixel
{
    std::uint16_t gained = 0;
    std::uint16_t lost = 0;
    std::uint16_t shared = 0;
    float depth = 0;
};

struct Comparison
{
    std::vector<Pixel> pixels;
    std::vector<std::uint16_t> coverage;
    std::uint64_t gained = 0, lost = 0, shared = 0;
    std::uint64_t resolvedDifference = 0;
    double meanDepth = 0, meanAbsoluteDepth = 0, p95Depth = 0;
    std::size_t closedGaps = 0, newGaps = 0, candidateGaps = 0;
};

template<class Trace>
Comparison compare(const ReferenceFrame& reference, const Trace& trace, unsigned samples)
{
    const auto width = reference.width, height = reference.height;
    Comparison result{std::vector<Pixel>(width * height), std::vector<std::uint16_t>(width * height)};
    std::vector<double> depthErrors;
    depthErrors.reserve(static_cast<std::size_t>(reference.hits));
    for (std::size_t i = 0; i < result.pixels.size(); ++i)
    {
        auto& pixel = result.pixels[i];
        auto& hits = result.coverage[i];
        for (const auto& sample : std::span(reference.rays).subspan(i * samples, samples))
        {
            const double hit = trace.nearest({reference.origin, sample.direction}, sample.interval);
            const bool sourceHit = std::isfinite(sample.source);
            const bool candidateHit = std::isfinite(hit);
            hits += candidateHit;
            if (sourceHit && candidateHit)
            {
                const double delta = hit - sample.source;
                ++pixel.shared;
                pixel.depth += static_cast<float>(delta);
                result.meanDepth += delta;
                result.meanAbsoluteDepth += std::abs(delta);
                depthErrors.push_back(std::abs(delta));
            }
            else
            {
                pixel.gained += candidateHit;
                pixel.lost += sourceHit;
            }
        }
        result.gained += pixel.gained;
        result.lost += pixel.lost;
        result.shared += pixel.shared;
        result.resolvedDifference += std::abs(int(hits) - int(reference.coverage[i]));
    }
    if (!depthErrors.empty())
    {
        const auto count = depthErrors.size();
        const auto p95 = (count - 1) - (count - 1) / 20;
        std::nth_element(depthErrors.begin(), depthErrors.begin() + p95, depthErrors.end());
        result.p95Depth = depthErrors[p95];
        result.meanDepth /= count;
        result.meanAbsoluteDepth /= count;
    }
    auto candidate = enclosedFoliageGaps(result.coverage, width, height, samples);
    result.candidateGaps = candidate.size();
    result.closedGaps = lostFoliageGaps(reference.gaps, result.coverage, samples);
    result.newGaps = lostFoliageGaps(candidate, reference.coverage, samples);
    return result;
}

void checkImage(HRESULT status)
{
    if (FAILED(status)) throw std::runtime_error("Could not encode a foliage comparison PNG.");
}

struct ComApartment
{
    ComApartment() { checkImage(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComApartment() { CoUninitialize(); }
};

class PngWriter
{
public:
    PngWriter()
    {
        checkImage(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_)));
    }

    template<class Color>
    void write(const std::filesystem::path& path, Crop crop, const Color& color) const
    {
        ComPtr<IWICStream> stream;
        checkImage(factory_->CreateStream(&stream));
        checkImage(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;
        checkImage(factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
        checkImage(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        checkImage(encoder->CreateNewFrame(&frame, nullptr));
        checkImage(frame->Initialize(nullptr));
        checkImage(frame->SetSize(crop.width, crop.height));
        auto format = GUID_WICPixelFormat24bppBGR;
        checkImage(frame->SetPixelFormat(&format));
        if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("PNG encoder changed the pixel format.");
        std::vector<std::uint8_t> pixels(crop.width * crop.height * 3);
        for (unsigned y = 0; y < crop.height; ++y)
        {
            for (unsigned x = 0; x < crop.width; ++x)
            {
                const auto rgb = color(static_cast<std::size_t>(crop.y + y) * crop.stride + crop.x + x);
                const auto i = 3 * (y * crop.width + x);
                for (unsigned c = 0; c < 3; ++c) pixels[i + c] = rgb[2 - c];
            }
        }
        checkImage(frame->WritePixels(crop.height, 3 * crop.width, static_cast<UINT>(pixels.size()), pixels.data()));
        checkImage(frame->Commit());
        checkImage(encoder->Commit());
    }

private:
    ComApartment apartment_;
    ComPtr<IWICImagingFactory> factory_;
};

using Color = std::array<std::uint8_t, 3>;
constexpr Color white{255, 255, 255};

Color mix(Color to, double weight)
{
    for (auto& channel : to) channel = static_cast<std::uint8_t>(std::lround(255 + weight * (channel - 255)));
    return to;
}

void coverageImage(const PngWriter& png, const std::filesystem::path& output, Crop crop,
                   std::span<const std::uint16_t> coverage, unsigned samples)
{
    png.write(output, crop, [&](std::size_t i) { return mix({30, 85, 61}, double(coverage[i]) / samples); });
}

void comparisonImages(const PngWriter& png, const std::filesystem::path& directory, const std::string& name,
                      Crop crop, const Comparison& result, unsigned samples)
{
    coverageImage(png, directory / (name + "-coverage.png"), crop, result.coverage, samples);
    png.write(directory / (name + "-gained.png"), crop, [&](std::size_t i) {
        return mix({193, 42, 34}, double(result.pixels[i].gained) / samples);
    });
    png.write(directory / (name + "-lost.png"), crop, [&](std::size_t i) {
        return mix({34, 102, 188}, double(result.pixels[i].lost) / samples);
    });
    png.write(directory / (name + "-depth.png"), crop, [&](std::size_t i) {
        const auto& pixel = result.pixels[i];
        if (!pixel.shared) return white;
        const double delta = pixel.depth / pixel.shared;
        return mix(delta < 0 ? Color{193, 42, 34} : Color{34, 102, 188},
                   std::min(1.0, std::abs(delta) / depthImageRange));
    });
}

struct VariantId
{
    std::string name;
    double cellSize = 0;
    std::size_t bricks = 0;
    std::uintmax_t bytes = 0;
};

template<class Trace>
void runVariant(std::ostream& csv, const PngWriter& png, const std::filesystem::path& directory,
                const Pose& pose, Crop crop, unsigned side, const ReferenceFrame& reference,
                const VariantId& id, const Trace& trace, bool farRoute)
{
    const auto samples = side * side;
    const auto result = compare(reference, trace, samples);
    if (result.shared + result.lost != reference.hits) throw std::runtime_error("Coverage counts do not reconcile.");
    csv << pose.name << ',' << pose.route << ',' << pose.distance << ',' << pose.yaw << ',' << pose.pitch << ','
        << side << ',' << id.name << ',' << id.cellSize << ',' << id.bricks << ',' << id.bytes << ',' << reference.hits
        << ',' << result.gained << ',' << result.lost << ',' << result.shared << ','
        << double(result.gained + result.lost) / (reference.hits + result.gained) << ',';
    if (result.shared) csv << result.meanDepth << ',' << result.meanAbsoluteDepth << ',' << result.p95Depth;
    else csv << ",,";
    csv << ',' << reference.gaps.size() << ',' << result.candidateGaps << ',' << result.closedGaps << ',' << result.newGaps
        << ',' << crop.x << ',' << crop.y << ',' << crop.width << ',' << crop.height;
    if (farRoute)
    {
        csv << ',' << double(reference.hits) / samples
            << ',' << double(result.shared + result.gained) / samples
            << ',' << double(result.resolvedDifference) / reference.hits
            << ',' << (double(result.gained) - double(result.lost)) / reference.hits
            << ',' << pose.sampleOffsetX << ',' << pose.sampleOffsetY;
    }
    csv << '\n';
    if (pose.images) comparisonImages(png, directory, id.name, crop, result, samples);
}

struct MixedRegionTrace
{
    const TriangleTrace& neighbors;
    const VoxelTrace& region;

    double nearest(const TraceRay& ray, TraceInterval interval) const
    {
        return std::min(neighbors.nearest(ray, interval), region.nearest(ray, interval));
    }
};

std::size_t overlappingCells(const VoxelFoliage& a, const VoxelFoliage& b)
{
    std::size_t overlap = 0;
    auto i = a.bricks.begin(), j = b.bricks.begin();
    while (i != a.bricks.end() && j != b.bricks.end())
    {
        if (i->coordinate < j->coordinate) ++i;
        else if (j->coordinate < i->coordinate) ++j;
        else
        {
            overlap += std::popcount(i->occupancy & j->occupancy);
            ++i;
            ++j;
        }
    }
    return overlap;
}

void compareRegion(const Options& options, const ViewerScene& scene, const std::filesystem::path& output)
{
    if (!scene.fixedLod || !scene.branchVoxelRegions)
        throw std::runtime_error("The region comparison requires the fitted Oak Branch mask and region ownership bake.");
    const auto id = *options.regionAb;
    const auto& lod = *scene.fixedLod;
    const auto& mesh = scene.asset->meshes[lod.meshIndex];
    const auto& control = (*scene.branchVoxelRegions)[id].foliage;
    const auto first = voxelRegionGroupStart(id, lod.groups.size());
    const auto end = voxelRegionGroupStart(id + 1, lod.groups.size());
    const auto indices = [&](std::size_t level) {
        const auto range = lod.indicesForGroups(level, first, end - 1);
        return std::span(lod.levels[level].indices).subspan(range.firstIndex, range.indexCount);
    };
    const auto sourceIndices = indices(0);
    const auto lod6Indices = indices(fixedLodTwoTriangleLevel);
    MeshData pilot{};
    pilot.vertices = mesh.vertices;
    pilot.indices.assign(sourceIndices.begin(), sourceIndices.end());
    const auto local = XMMatrixIdentity();
    const auto worldToMesh = XMMatrixInverse(nullptr, XMLoadFloat4x4(&mesh.localToWorld));
    const auto bark = traceOtherMeshes(scene, lod.meshIndex, worldToMesh);
    std::vector<TraceTriangle> sourceTriangles, pilotLod6, wholeLod6, neighborLod6;
    appendTriangles(sourceTriangles, mesh.vertices, sourceIndices, local);
    appendTriangles(pilotLod6, lod.proxyVertices, lod6Indices, local);
    const auto all = std::span(lod.levels[fixedLodTwoTriangleLevel].indices);
    appendTriangles(wholeLod6, lod.proxyVertices, all, local);
    const auto pilotStart = lod6Indices.data() - all.data();
    appendTriangles(neighborLod6, lod.proxyVertices, all.first(pilotStart), local);
    appendTriangles(neighborLod6, lod.proxyVertices, all.subspan(pilotStart + lod6Indices.size()), local);
    const TriangleTrace source(std::move(sourceTriangles));
    const TriangleTrace isolatedLod6(std::move(pilotLod6));
    const TriangleTrace fullLod6(std::move(wholeLod6));
    const TriangleTrace neighbors(std::move(neighborLod6));

    constexpr unsigned bakeSamples = 16;
    const auto bake = sampleVoxelCoverage(pilot, control.cellSize, bakeSamples);
    const auto eligible = packVoxelFoliage(bake, control.sourceFingerprint, control.meshIndex, std::nullopt);
    const VoxelTrace eligibleTrace(eligible);
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    constexpr ImageRegion image{96, 96, 3200, 1800, branchVoxelFarNearPlane, branchVoxelFarClipPlane};
    constexpr Pose fitPoses[]{
        {"fit-1", "fit", 555, -138, 58}, {"fit-2", "fit", 605, -79, 31},
        {"fit-3", "fit", 660, -33, 18}, {"fit-4", "fit", 710, 8, 14},
        {"fit-5", "fit", 755, 52, 29}, {"fit-6", "fit", 835, 94, 54},
        {"fit-7", "fit", 940, 130, 27}, {"fit-8", "fit", 1110, 177, 48}};
    constexpr unsigned fitSide = 4;
    const auto cameraFor = [&](const Pose& pose) {
        return CameraView{focus, XMConvertToRadians(pose.yaw), XMConvertToRadians(pose.pitch), pose.distance};
    };
    std::vector<VoxelCoverageView> views;
    for (const auto& pose : fitPoses)
    {
        auto reference = makeReference(cameraFor(pose), worldToMesh, image, pose, fitSide, source, bark);
        views.push_back(collectFitRays(std::move(reference), eligibleTrace, eligible.bricks.size(), fitSide * fitSide));
    }
    const auto result = fitVoxelCoverage(eligible, control, views, control.bricks.size());
    std::filesystem::create_directories(output);
    const auto independent = output / "independent.voxels";
    writeVoxelFoliage(independent, result.foliage);
    const auto normals = bakeVoxelNormals(pilot, result.foliage, bakeSamples);
    writeVoxelNormals(output / "independent.normals", sourceFingerprint(independent), normals.cells);
    writeVoxelFoliage(output / "shared-mask.voxels", control);
    std::ofstream meta(output / "settings.txt");
    meta.exceptions(std::ios::failbit | std::ios::badbit);
    meta << "Region " << id << ", groups " << first << " through " << end - 1 << '\n'
         << "Source triangles " << sourceIndices.size() / 3 << ", LOD 6 triangles " << lod6Indices.size() / 3 << '\n'
         << "Cell size " << control.cellSize << ", brick budget " << control.bricks.size() << '\n'
         << "Shared-mask mask " << control.bricks.size() << " bricks, " << occupiedVoxelCount(control) << " cells\n"
         << "Independent mask " << result.foliage.bricks.size() << " bricks, " << occupiedVoxelCount(result.foliage) << " cells\n"
         << "Common cells " << overlappingCells(control, result.foliage) << ", brick swaps " << result.brickSwaps << '\n';
    for (std::size_t neighbor = 0; neighbor < scene.branchVoxelRegions->size(); ++neighbor)
        if (neighbor != id)
            meta << "Independent cells also in region " << neighbor << ": "
                 << overlappingCells(result.foliage, (*scene.branchVoxelRegions)[neighbor].foliage) << '\n';
    meta << "3200 x 1800, FOV 45 degrees, bark occlusion enabled, mesh at rest.\n"
         << "Fitting uses the isolated source region at " << fitSide * fitSide << " samples per pixel.\n";
    meta.close();
    std::ofstream fit(output / "fit.csv");
    fit.exceptions(std::ios::failbit | std::ios::badbit);
    fit.imbue(std::locale::classic());
    fit << "pose,distance,yaw,pitch,shared_mask_l1,independent_l1\n";
    for (std::size_t i = 0; i < std::size(fitPoses); ++i)
        fit << fitPoses[i].name << ',' << fitPoses[i].distance << ',' << fitPoses[i].yaw << ',' << fitPoses[i].pitch
            << ',' << result.seedErrors[i] << ',' << result.optimizedErrors[i] << '\n';
    fit.close();
    std::cout << "Baked region " << id << " with " << result.foliage.bricks.size() << " bricks.\n" << std::flush;

    constexpr Pose holdout[]{
        {"retreat-650", "", 650, -125, 68}, {"retreat-700", "", 700, -125, 68, true},
        {"retreat-735", "", 735, -125, 68}, {"side-700", "", 700, 32, 8, true},
        {"side-offset-735", "", 735, 35, 9, false, 0.25, 0.25}, {"rear-700", "", 700, 71, 61, true},
        {"rear-780", "", 780, 75, 56}, {"oblique-a", "", 695, -56, 34}, {"oblique-b", "", 725, 152, 24},
        {"oblique-c", "", 760, -169, 47}, {"side-1200", "", 1200, -12, 23}, {"rear-1200", "", 1200, 105, 35}};
    std::ofstream csv(output / "metrics.csv", std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "pose,context,distance,yaw,pitch,samples_per_side,variant,cell_size,bricks,voxel_file_bytes,source_hit_rays,"
           "gained_rays,lost_rays,shared_rays,symmetric_error_over_union,mean_signed_depth,mean_abs_depth,p95_abs_depth,"
           "source_enclosed_gaps,candidate_enclosed_gaps,closed_source_gaps,new_candidate_gaps,crop_x,crop_y,crop_width,crop_height,"
           "source_coverage_px,candidate_coverage_px,resolved_l1_over_source,coverage_bias_over_source,"
           "sample_offset_x_px,sample_offset_y_px\n" << std::fixed << std::setprecision(8);
    const auto describe = [&](std::string name, const VoxelFoliage& data) {
        return VariantId{std::move(name), data.cellSize, data.bricks.size(), 60 + 20 * data.bricks.size()};
    };
    const auto a = describe("shared-mask", control), b = describe("independent", result.foliage);
    const VoxelTrace traceA(control), traceB(result.foliage);
    const auto side = options.samplesPerSide;
    const PngWriter png;
    for (const auto& pose : holdout)
    {
        const auto compareContext = [&](const char* context, const TriangleTrace& triangles, const auto& candidateA,
                                        const auto& candidateB) {
            auto labeled = pose;
            labeled.route = context;
            const auto reference = makeReference(cameraFor(pose), worldToMesh, image, pose, side, triangles, bark);
            const auto crop = extractCrop(reference.coverage, image.width, image.height);
            if (crop.x == 0 || crop.y == 0 || crop.x + crop.width == image.width || crop.y + crop.height == image.height)
                throw std::runtime_error("The evaluation crop approached the trace raster boundary.");
            const auto directory = output / pose.name / context;
            if (pose.images)
            {
                std::filesystem::create_directories(directory);
                coverageImage(png, directory / "reference-coverage.png", crop, reference.coverage, side * side);
            }
            runVariant(csv, png, directory, labeled, crop, side, reference, a, candidateA, true);
            runVariant(csv, png, directory, labeled, crop, side, reference, b, candidateB, true);
        };
        compareContext("isolated-source", source, traceA, traceB);
        compareContext("isolated-lod6", isolatedLod6, traceA, traceB);
        compareContext("mixed-lod6", fullLod6, MixedRegionTrace{neighbors, traceA}, MixedRegionTrace{neighbors, traceB});
        std::cout << pose.name << " compared.\n" << std::flush;
    }
    csv.close();
}
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        if (argc < 4)
            throw std::runtime_error("Usage: CompareVoxelFoliage viewer-assets report-directory bake-directory "
                                     "[bake-directory ...] [--samples N] [--image-poses-only] [--far | --coverage-review]\n"
                                     "       CompareVoxelFoliage viewer-assets report-directory --optimize eligible.voxels "
                                     "seed.voxels [--samples N] [--brick-budget N] [--near-fit]\n"
                                     "       CompareVoxelFoliage viewer-assets report-directory --region-ab INDEX [--samples N]");
        const auto options = parseOptions(argc, argv);
        const std::filesystem::path output = argv[2];
        ViewerScenes scenes{std::filesystem::path(argv[1])};
        const auto& scene = scenes.open(SceneKind::OakBranch);
        if (options.regionAb)
        {
            compareRegion(options, scene, output);
            return 0;
        }
        if (!options.eligibleVoxels.empty())
        {
            optimizeBake(options, scene, output);
            return 0;
        }
        if (!scene.fixedLod || scene.fixedLod->levels.size() <= fixedLodTwoTriangleLevel)
            throw std::runtime_error("The viewer assets must include the congruent Oak Branch GLB and a complete fixed LOD bake.");
        const auto& lod = *scene.fixedLod;
        const auto& meshes = scene.asset->meshes;
        const auto& mesh = meshes[lod.meshIndex];
        const auto worldToMesh = XMMatrixInverse(nullptr, XMLoadFloat4x4(&mesh.localToWorld));
        std::vector<TraceTriangle> sourceTriangles, proxyTriangles;
        appendTriangles(sourceTriangles, mesh.vertices, mesh.indices, XMMatrixIdentity());
        appendTriangles(proxyTriangles, lod.proxyVertices, lod.levels[fixedLodTwoTriangleLevel].indices, XMMatrixIdentity());
        const TriangleTrace source(std::move(sourceTriangles)), proxy(std::move(proxyTriangles));
        const auto bark = traceOtherMeshes(scene, lod.meshIndex, worldToMesh);
        const auto fingerprint = sourceFingerprint(scene.source);
        const auto bakes = readBakes(options, fingerprint, lod.meshIndex);
        const auto focus = scene.cameraView(CameraPreset::Overview).focus;
        const bool reportResolved = options.farRoute || options.coverageReview;
        const ImageRegion region = reportResolved ? farRegion : ImageRegion{800, 450, 800, 450, 0.05f, scene.farPlane()};
        std::filesystem::create_directories(output);
        std::ofstream csv(output / "metrics.csv", std::ios::binary | std::ios::trunc);
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv.imbue(std::locale::classic());
        csv << "pose,route,distance,yaw,pitch,samples_per_side,variant,cell_size,bricks,voxel_file_bytes,source_hit_rays,"
               "gained_rays,lost_rays,shared_rays,symmetric_error_over_union,mean_signed_depth,mean_abs_depth,p95_abs_depth,"
               "source_enclosed_gaps,candidate_enclosed_gaps,closed_source_gaps,new_candidate_gaps,crop_x,crop_y,crop_width,crop_height";
        if (reportResolved)
            csv << ",source_coverage_px,candidate_coverage_px,resolved_l1_over_source,coverage_bias_over_source,"
                   "sample_offset_x_px,sample_offset_y_px";
        csv << '\n' << std::fixed << std::setprecision(8);
        std::ofstream settings(output / "settings.txt", std::ios::binary | std::ios::trunc);
        settings.exceptions(std::ios::failbit | std::ios::badbit);
        settings << "Source GLB FNV-1a " << std::hex << fingerprint << "\nFixed LOD FNV-1a "
                 << sourceFingerprint(fixedLodPath(scene.source)) << std::dec << "\nMesh " << mesh.name
                 << "\nViewport " << region.viewportWidth << " x " << region.viewportHeight
                 << "\nSamples per pixel side " << options.samplesPerSide
                 << "\nFOV 45 degrees\nFocus " << focus.x << ' ' << focus.y << ' ' << focus.z << '\n';
        if (reportResolved)
            settings << "Centered trace region " << region.width << " x " << region.height << " pixels\n";
        for (std::size_t i = 0; i < options.bakeDirectories.size(); ++i)
            settings << "bake-" << i + 1 << " = " << options.bakeDirectories[i] << '\n';
        settings.close();

        const PngWriter png;
        auto route = options.farRoute ? std::span<const Pose>(farPoses) : std::span<const Pose>(poses);
        if (options.coverageReview) route = coverageReviewPoses;
        for (const auto& pose : route)
        {
            if (options.imagePosesOnly && !pose.images) continue;
            const auto start = std::chrono::steady_clock::now();
            const CameraView camera{focus, XMConvertToRadians(pose.yaw), XMConvertToRadians(pose.pitch), pose.distance};
            const auto reference = makeReference(camera, worldToMesh, region, pose, options.samplesPerSide, source, bark);
            const auto crop = extractCrop(reference.coverage, region.width, region.height);
            const auto directory = output / pose.name;
            if (pose.images)
            {
                std::filesystem::create_directories(directory);
                coverageImage(png, directory / "source-coverage.png", crop, reference.coverage,
                              options.samplesPerSide * options.samplesPerSide);
            }
            runVariant(csv, png, directory, pose, crop, options.samplesPerSide, reference, {"lod6"}, proxy, reportResolved);
            for (const auto& bake : bakes)
                runVariant(csv, png, directory, pose, crop, options.samplesPerSide, reference,
                           {bake.name, bake.cellSize, bake.brickCount, bake.bytes}, bake.trace, reportResolved);
            csv.flush();
            const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
            std::cout << pose.name << " completed in " << elapsed.count() << " s\n" << std::flush;
        }
        csv.close();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "CompareVoxelFoliage: " << error.what() << '\n';
        return 1;
    }
}
