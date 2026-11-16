#include "BranchBenchmark.h"
#include "../Tools/FoliageTrace.h"
#include "SourceFingerprint.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <stdexcept>
#include <tuple>

using namespace DirectX;

namespace
{
constexpr double infinity = std::numeric_limits<double>::infinity();

bool processMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

TracePoint point(FXMVECTOR v) { return {XMVectorGetX(v), XMVectorGetY(v), XMVectorGetZ(v)}; }

void appendTriangles(std::vector<TraceTriangle>& into, const MeshData& mesh, FXMMATRIX localToTrace)
{
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
    {
        TraceTriangle triangle;
        for (unsigned j = 0; j < 3; ++j)
        {
            const auto& p = mesh.vertices[mesh.indices[i + j]].position;
            triangle[j] = point(XMVector3TransformCoord(XMLoadFloat3(&p), localToTrace));
        }
        into.push_back(triangle);
    }
}

struct Parity
{
    std::uint64_t cpuHits = 0, gpuHits = 0, mismatched = 0, agreed = 0;
    double depthMean = 0, depthP99 = 0;
};

Parity compareGpu(const ViewerScene& scene, const VoxelTrace& voxels, const XMFLOAT4X4& view,
                  std::span<const std::uint32_t> pixels, unsigned width, unsigned height, unsigned stride)
{
    const auto mesh = scene.fixedLod->meshIndex;
    const auto placement = scene.objects.front().placementMatrix();
    const auto localToWorld = XMLoadFloat4x4(&scene.asset->meshes[mesh].localToWorld) * placement;
    const auto worldToLocal = XMMatrixInverse(nullptr, localToWorld);
    std::vector<TraceTriangle> opaque;
    for (std::size_t i = 0; i < scene.asset->meshes.size(); ++i)
    {
        if (i == mesh) continue;
        const auto& part = scene.asset->meshes[i];
        appendTriangles(opaque, part, XMLoadFloat4x4(&part.localToWorld) * placement * worldToLocal);
    }
    appendTriangles(opaque, scene.ground, XMLoadFloat4x4(&scene.ground.localToWorld) * worldToLocal);
    const TriangleTrace occluders(std::move(opaque));
    const auto viewToLocal = XMMatrixInverse(nullptr, XMLoadFloat4x4(&view)) * worldToLocal;
    const auto origin = point(viewToLocal.r[3]);
    const auto right = point(viewToLocal.r[0]), up = point(viewToLocal.r[1]), forward = point(-viewToLocal.r[2]);
    const double halfY = std::tan(D3D12Renderer::cameraVerticalFov * 0.5f);
    const double halfX = halfY * width / height;
    const double farPlane = scene.farPlane();
    if (pixels.size() != static_cast<std::size_t>(width) * height) throw std::runtime_error("Bad voxel capture extent.");
    Parity result;
    std::vector<double> differences;
    for (unsigned y = 0; y < height; y += stride)
    {
        const double dy = (1 - 2 * (y + 0.5) / height) * halfY;
        for (unsigned x = 0; x < width; x += stride)
        {
            const double dx = (2 * (x + 0.5) / width - 1) * halfX;
            TraceRay ray{origin, {}};
            for (unsigned axis = 0; axis < 3; ++axis) ray.direction[axis] = forward[axis] + dx * right[axis] + dy * up[axis];
            TraceInterval clip{scene.nearPlane, farPlane};
            clip.exit = std::min(clip.exit, occluders.nearest(ray, clip));
            const double cpu = voxels.nearest(ray, clip);
            const auto pixel = pixels[static_cast<std::size_t>(y) * width + x];
            const bool gpuHit = (pixel >> 24) == 64;
            const bool cpuHit = cpu != infinity;
            result.cpuHits += cpuHit;
            result.gpuHits += gpuHit;
            result.mismatched += cpuHit != gpuHit;
            if (cpuHit && gpuHit)
            {
                const auto gpu = (pixel & 0xffffff) * farPlane / 16777215.0;
                const auto difference = std::abs(gpu - cpu);
                result.depthMean += difference;
                differences.push_back(difference);
            }
        }
    }
    result.agreed = differences.size();
    if (!differences.empty())
    {
        result.depthMean /= differences.size();
        const auto index = differences.size() - 1 - (differences.size() - 1) / 100;
        std::nth_element(differences.begin(), differences.begin() + index, differences.end());
        result.depthP99 = differences[index];
    }
    return result;
}

void verifyDepth(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report, bool farRoute)
{
    auto parityPath = report;
    parityPath.replace_filename(report.stem().native() + L"-parity.csv");
    std::ofstream out(parityPath, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.imbue(std::locale::classic());
    out << "pose,variant,pixel_stride,cpu_hits,gpu_hits,mismatched,agreed,mean_abs_view_depth,p99_abs_view_depth\n";
    scene.branchFrustumCulling = false;
    scene.branchVoxelDepthCapture = true;
    scene.setBranchGrid(1);
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const auto viewport = renderer.viewportSize();
    const auto& mesh = scene.asset->meshes[scene.fixedLod->meshIndex];
    std::vector<std::uint32_t> pixels;
    bool failed = false;
    for (const auto mode : {BranchVoxelView::Fine, BranchVoxelView::Coarse})
    {
        scene.branchVoxelView = mode;
        const auto& data = *scene.branchVoxels[*branchVoxelIndex(mode)];
        const VoxelTrace trace(data);
        const auto& brick = data.bricks[data.bricks.size() / 2];
        XMFLOAT3 eye;
        XMStoreFloat3(&eye, XMVector3TransformCoord(XMVectorSet(
            static_cast<float>(data.origin[0] + (brick.coordinate[0] * 4.0 + 2.1) * data.cellSize),
            static_cast<float>(data.origin[1] + (brick.coordinate[1] * 4.0 + 2.1) * data.cellSize),
            static_cast<float>(data.origin[2] + (brick.coordinate[2] * 4.0 + 3.5) * data.cellSize), 1),
            XMLoadFloat4x4(&mesh.localToWorld)));
        struct Pose { const char* name; CameraView camera; XMFLOAT4 placement; unsigned stride = 1; };
        const Pose nearPoses[]{
            {"retreat40", {focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), 40}, {0, 0, 0, 1}},
            {"rotated", {focus, XMConvertToRadians(32.0f), XMConvertToRadians(8.0f), 55}, {-0.8f, 2.0f, 1.3f, 0.85f}},
            {"inside_brick", {{eye.x, eye.y, eye.z - 1}, 0, 0, 1}, {0, 0, 0, 1}, 4},
        };
        const Pose farPoses[]{
            {"retreat300", {focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), 300}, {0, 0, 0, 1}},
            {"retreat3000", {focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), 3000}, {0, 0, 0, 1}},
            {"side3000", {focus, XMConvertToRadians(32.0f), XMConvertToRadians(8.0f), 3000}, {0, 0, 0, 1}},
        };
        const auto poses = farRoute ? std::span<const Pose>(farPoses) : std::span<const Pose>(nearPoses);
        for (const auto& pose : poses)
        {
            if (!processMessages()) throw std::runtime_error("Voxel benchmark was interrupted.");
            scene.objects.front().placement = pose.placement;
            renderer.setScene(scene);
            const auto view = pose.camera.matrix();
            renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
            const auto result = compareGpu(scene, trace, view, pixels, static_cast<unsigned>(viewport.x),
                                           static_cast<unsigned>(viewport.y), pose.stride);
            const auto label = mode == BranchVoxelView::Fine ? (farRoute ? "voxel_a" : "fine") : (farRoute ? "voxel_b" : "coarse");
            out << pose.name << ',' << label << ',' << pose.stride << ','
                << result.cpuHits << ',' << result.gpuHits << ',' << result.mismatched << ',' << result.agreed << ','
                << std::fixed << std::setprecision(9) << result.depthMean << ',' << result.depthP99 << '\n' << std::flush;
            const auto hitUnion = result.agreed + result.mismatched;
            // The far fixture has only a handful of covered pixels and float transforms at 3,000 units.
            const auto mismatchLimit = farRoute ? std::max(1.0, hitUnion * 0.005) : hitUnion * 0.005;
            if (!hitUnion || result.mismatched > mismatchLimit || result.depthP99 > (farRoute ? 0.01 : 0.001))
                failed = true;
        }
    }
    if (failed) throw std::runtime_error("GPU voxel depth or coverage exceeded CPU parity tolerance. See the parity CSV.");
    scene.branchVoxelDepthCapture = false;
}

std::string quotedAdapter(const std::wstring& adapter)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, adapter.data(), static_cast<int>(adapter.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size, '\0'), quoted = "\"";
    WideCharToMultiByte(CP_UTF8, 0, adapter.data(), static_cast<int>(adapter.size()), text.data(), size, nullptr, nullptr);
    for (char c : text)
    {
        if (c == '"') quoted += c;
        quoted += c;
    }
    return quoted + '"';
}

void verifyVoxelShading(ViewerScene& scene, D3D12Renderer& renderer, VoxelShadingComparison shading)
{
    scene.setBranchGrid(1);
    renderer.setScene(scene);
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const CameraView pose{focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), branchOrbitDistance};
    const auto view = pose.matrix();
    const auto capture = [&](BranchVoxelView mode, BranchAppearance appearance) {
        scene.branchVoxelView = mode;
        scene.branchVoxelDirectionalNormals = shading == VoxelShadingComparison::DirectionalNormals && mode == BranchVoxelView::Coarse;
        scene.branchVoxelShadowReceiver = shading == VoxelShadingComparison::ShadowReceiver && mode == BranchVoxelView::Coarse
            ? VoxelShadowReceiver::BakedLeaf : VoxelShadowReceiver::CellEntry;
        scene.branchAppearance = appearance;
        std::vector<std::uint32_t> pixels;
        renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
        return pixels;
    };
    const auto flatCoverage = capture(BranchVoxelView::Fine, BranchAppearance::Coverage);
    const auto weightedCoverage = capture(BranchVoxelView::Coarse, BranchAppearance::Coverage);
    if (flatCoverage != weightedCoverage)
        throw std::runtime_error("Voxel shading changed the coverage capture.");
    if (shading == VoxelShadingComparison::ShadowReceiver)
    {
        scene.branchVoxelDepthCapture = true;
        const auto entryDepth = capture(BranchVoxelView::Fine, BranchAppearance::Shaded);
        const auto bakedDepth = capture(BranchVoxelView::Coarse, BranchAppearance::Shaded);
        scene.branchVoxelDepthCapture = false;
        if (entryDepth != bakedDepth) throw std::runtime_error("The shadow receiver changed camera depth.");
        auto& receive = scene.meshShadows[scene.fixedLod->meshIndex].receive;
        const bool original = receive;
        receive = false;
        const auto entryUnshadowed = capture(BranchVoxelView::Fine, BranchAppearance::Shaded);
        const auto bakedUnshadowed = capture(BranchVoxelView::Coarse, BranchAppearance::Shaded);
        receive = original;
        if (entryUnshadowed != bakedUnshadowed) throw std::runtime_error("The shadow receiver changed unshadowed shading.");
    }
    const auto originalAzimuth = scene.sunAzimuth;
    for (const auto azimuth : {originalAzimuth, originalAzimuth + 180})
    {
        scene.sunAzimuth = azimuth;
        renderer.updateSun(scene);
        const auto flat = capture(BranchVoxelView::Fine, BranchAppearance::Shaded);
        const auto weighted = capture(BranchVoxelView::Coarse, BranchAppearance::Shaded);
        if (flat == weighted) throw std::runtime_error("The voxel shading variants produced identical shaded frames.");
    }
    scene.sunAzimuth = originalAzimuth;
    renderer.updateSun(scene);
    for (const auto mode : {BranchVoxelView::Lod6, BranchVoxelView::Coarse})
    {
        const auto coverage = capture(mode, BranchAppearance::Coverage);
        const auto before = capture(mode, BranchAppearance::BaseColor);
        scene.sunAzimuth = originalAzimuth + 180;
        renderer.updateSun(scene);
        const auto after = capture(mode, BranchAppearance::BaseColor);
        std::size_t compared = 0;
        for (std::size_t pixel = 0; pixel < coverage.size(); ++pixel)
        {
            constexpr std::uint32_t coverageColor = 0xff528c26; // (0.15, 0.55, 0.32, 1) in RGBA8.
            if (coverage[pixel] != coverageColor) continue;
            ++compared;
            if (before[pixel] != after[pixel])
                throw std::runtime_error("Light direction changed unlit foliage color.");
        }
        if (!compared) throw std::runtime_error("Unlit comparison found no foliage pixels.");
        scene.sunAzimuth = originalAzimuth;
        renderer.updateSun(scene);
    }
}

bool verifyProxyBounds(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report)
{
    auto path = report;
    path.replace_filename(report.stem().native() + L"-proxy-parity.csv");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.imbue(std::locale::classic());
    out << "pose,msaa,covered_pixels,coverage_mismatch,common_depth_pixels,depth_outliers,p99_abs_view_depth,max_abs_view_depth\n";
    scene.branchVoxelView = BranchVoxelView::Fine;
    scene.branchFrustumCulling = false;
    scene.setBranchGrid(1);
    renderer.setScene(scene);
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const auto& data = *scene.branchVoxels[0];
    const auto& brick = data.bricks[data.bricks.size() / 2];
    const auto cell = static_cast<unsigned>(std::countr_zero(brick.occupancy));
    const auto coordinate = [&](unsigned axis, unsigned index) {
        return static_cast<float>(data.origin[axis] + (brick.coordinate[axis] * 4.0 + index + 0.5) * data.cellSize);
    };
    XMFLOAT3 eye;
    XMStoreFloat3(&eye, XMVector3TransformCoord(XMVectorSet(coordinate(0, cell % 4), coordinate(1, cell / 4 % 4),
        coordinate(2, cell / 16), 1), XMLoadFloat4x4(&scene.asset->meshes[scene.fixedLod->meshIndex].localToWorld)));
    struct Pose { const char* label; CameraView camera; float nearPlane; float farPlane; };
    const CameraView retreat{focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), 40};
    const Pose poses[]{
        {"retreat40", retreat, 0.05f, 120},
        {"side700", {focus, XMConvertToRadians(32.0f), XMConvertToRadians(8.0f), 700}, 20, 3600},
        {"inside_brick", {{eye.x, eye.y, eye.z - 1}, 0, 0, 1}, 0.05f, 120},
        {"far_clip", retreat, 0.05f, 40},
    };
    bool passed = true;
    for (const bool msaa : {false, true})
    {
        if (msaa && !renderer.supportsMsaa()) continue;
        if (renderer.msaaEnabled() != msaa) renderer.toggleMsaa();
        for (const auto& pose : poses)
        {
            if (!processMessages()) return false;
            renderer.updateCameraProjection(pose.nearPlane, pose.farPlane);
            const auto view = pose.camera.matrix();
            const auto capture = [&](bool tight) {
                scene.branchVoxelTightProxies = tight;
                std::vector<std::uint32_t> pixels;
                renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
                return pixels;
            };
            scene.branchAppearance = BranchAppearance::CoverageMask;
            const auto fullCoverage = capture(false);
            const auto tightCoverage = capture(true);
            std::size_t covered = 0, mismatched = 0, depthOutliers = 0;
            double p99Depth = 0, maxDepth = 0;
            std::vector<double> depthDifferences;
            for (std::size_t pixel = 0; pixel < fullCoverage.size(); ++pixel)
            {
                const auto full = fullCoverage[pixel] >> 24;
                const auto tight = tightCoverage[pixel] >> 24;
                covered += std::min(full, tight) < 255;
                mismatched += full != tight;
            }
            // Encoded depth can only be decoded from a single-sample capture.
            const double depthTolerance = pose.farPlane > 120 ? 0.01 : 0.001;
            if (!msaa)
            {
                scene.branchVoxelDepthCapture = true;
                const auto fullDepth = capture(false);
                const auto tightDepth = capture(true);
                scene.branchVoxelDepthCapture = false;
                for (std::size_t pixel = 0; pixel < fullDepth.size(); ++pixel)
                {
                    if ((fullDepth[pixel] >> 24) != 64 || (tightDepth[pixel] >> 24) != 64) continue;
                    const auto full = static_cast<int>(fullDepth[pixel] & 0xffffff);
                    const auto tight = static_cast<int>(tightDepth[pixel] & 0xffffff);
                    const double difference = std::abs(tight - full) * static_cast<double>(pose.farPlane) / 16777215.0;
                    depthDifferences.push_back(difference);
                    depthOutliers += difference > depthTolerance;
                    maxDepth = std::max(maxDepth, difference);
                }
                if (!depthDifferences.empty())
                {
                    const auto rank = depthDifferences.size() - 1 - (depthDifferences.size() - 1) / 100;
                    std::nth_element(depthDifferences.begin(), depthDifferences.begin() + rank, depthDifferences.end());
                    p99Depth = depthDifferences[rank];
                }
            }
            out << pose.label << ',' << (msaa ? 4 : 1) << ',' << covered << ',' << mismatched << ',' << depthDifferences.size()
                << ',' << depthOutliers << ',' << std::fixed << std::setprecision(9) << p99Depth << ',' << maxDepth << '\n' << std::flush;
            passed &= covered && mismatched <= std::max(1.0, covered * 0.001) &&
                      (msaa || (!depthDifferences.empty() && p99Depth <= depthTolerance &&
                                depthOutliers <= std::max(1.0, depthDifferences.size() * 0.001)));
        }
    }
    if (renderer.msaaEnabled()) renderer.toggleMsaa();
    return passed;
}
}

int runBranchVoxelBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                            const std::filesystem::path& report,
                            const std::array<std::filesystem::path, 2>& farVoxelBakes, VoxelShadingComparison shading,
                            bool compareProxyBounds)
{
    const bool compareNormals = shading != VoxelShadingComparison::None;
    const bool directional = shading == VoxelShadingComparison::DirectionalNormals;
    const bool shadowReceiver = shading == VoxelShadingComparison::ShadowReceiver;
    const bool customBakes = !farVoxelBakes[0].empty();
    const bool fittedBake = compareNormals || compareProxyBounds;
    const bool farRoute = customBakes || fittedBake;
    const auto sourceHash = sourceFingerprint(scene.source);
    if (fittedBake)
    {
        const auto index = *branchVoxelIndex(BranchVoxelView::CellOneEighth);
        const auto mask = scene.branchVoxels[index];
        const auto normals = scene.branchVoxelNormals[index];
        if (!mask || !scene.branchVoxelCoverageOptimized[index] || !normals)
            throw std::runtime_error("Build oak_branch_normals before benchmarking the optimized voxel normals.");
        if (directional && !normals->hasDirectionalWeights)
            throw std::runtime_error("Rebuild oak_branch_normals for view-dependent weights.");
        if (shadowReceiver && !normals->hasShadowReceivers)
            throw std::runtime_error("Rebuild oak_branch_normals for baked leaf shadow positions.");
        scene.branchVoxels[0] = scene.branchVoxels[1] = mask;
        scene.branchVoxelNormals = {};
        scene.branchVoxelNormals[1] = normals;
        if (directional || shadowReceiver || compareProxyBounds) scene.branchVoxelNormals[0] = normals;
        scene.branchVoxelUseNormals = true;
    }
    if (customBakes && scene.fixedLod)
    {
        for (std::size_t i = 0; i < farVoxelBakes.size(); ++i)
        {
            auto data = readVoxelFoliage(farVoxelBakes[i]);
            if (data.sourceFingerprint != sourceHash || data.meshIndex != scene.fixedLod->meshIndex || data.bricks.empty())
                throw std::runtime_error("A far voxel bake is empty or does not match the branch GLB and foliage mesh.");
            scene.branchVoxels[i] = std::make_shared<VoxelFoliage>(std::move(data));
        }
    }
    if (farRoute)
    {
        // Keep sufficient depth precision for the distant bark and foliage comparison.
        scene.nearPlane = branchVoxelFarNearPlane;
        scene.minimumFarPlane = branchVoxelFarClipPlane;
    }
    if (!scene.fixedLod || !scene.branchVoxels[0] || !scene.branchVoxels[1])
        throw std::runtime_error("Rebuild the Oak Branch LOD and voxel assets before running the voxel benchmark.");
    for (auto& bake : std::span(scene.branchVoxels).subspan(farVoxelBakes.size())) bake.reset();
    scene.sunAzimuth = -100;
    scene.sunElevation = 54.5715f;
    if (compareProxyBounds)
    {
        if (!verifyProxyBounds(scene, renderer, report)) return 1;
    }
    else verifyDepth(scene, renderer, report, farRoute);
    if (compareNormals) verifyVoxelShading(scene, renderer, shading);
    scene.branchFrustumCulling = true;
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const auto viewport = renderer.viewportSize();
    constexpr BranchVoxelView modes[]{BranchVoxelView::Lod6, BranchVoxelView::Fine, BranchVoxelView::Coarse};
    constexpr const char* nearLabels[]{"lod6", "fine", "coarse"};
    constexpr const char* farLabels[]{"lod6", "voxel_a", "voxel_b"};
    constexpr const char* normalLabels[]{"lod6", "voxel_flat", "voxel_normals"};
    constexpr const char* directionalLabels[]{"lod6", "voxel_pooled", "voxel_directional"};
    constexpr const char* receiverLabels[]{"lod6", "voxel_entry_receiver", "voxel_baked_receiver"};
    constexpr const char* proxyLabels[]{"lod6", "voxel_full_proxy", "voxel_tight_proxy"};
    const char* const* labels = compareProxyBounds ? proxyLabels : shadowReceiver ? receiverLabels : directional ? directionalLabels :
        (compareNormals ? normalLabels : (farRoute ? farLabels : nearLabels));
    constexpr float nearDistances[]{18, 40, 60, 90, 120, 180, 240, 300};
    constexpr float normalDistances[]{branchOrbitDistance, 1200, 3000};
    const auto distances = fittedBake ? std::span<const float>(normalDistances)
        : (farRoute ? std::span<const float>(branchVoxelFarDistances) : std::span<const float>(nearDistances));
    constexpr unsigned repetitions = 9;
    const auto adapter = quotedAdapter(renderer.adapterName());
    std::ofstream out(report, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.imbue(std::locale::classic());
    out << "adapter,source_fingerprint,width,height,msaa,view,instances,distance,mode,repetition,camera_instances,"
           "shadow_instances,voxel_bricks,foliage_api_calls,camera_foliage_triangles,shadow_foliage_triangles,"
           "camera_triangles,shadow_triangles,cpu_ms,gpu_camera_ms,gpu_shadow_ms,gpu_frame_ms";
    if (farRoute) out << ",voxel_cell_size,voxel_fingerprint";
    out << '\n' << std::fixed << std::setprecision(6);
    std::array<std::uint64_t, 2> voxelHashes{};
    if (customBakes)
        for (std::size_t i = 0; i < farVoxelBakes.size(); ++i)
            voxelHashes[i] = sourceFingerprint(farVoxelBakes[i]);
    if (fittedBake)
    {
        auto maskPath = scene.source;
        maskPath.replace_extension(L".eighth.coverage.voxels");
        voxelHashes[0] = voxelHashes[1] = sourceFingerprint(maskPath);
    }
    const auto shadowTriCount = scene.fixedLod->levels[fixedLodTwoTriangleLevel].indices.size() / 3;
    for (int side : {1, branchGridSide})
    {
        scene.setBranchGrid(side);
        renderer.setScene(scene);
        for (const bool msaa : {false, true})
        {
            if (msaa && !renderer.supportsMsaa()) continue;
            if (renderer.msaaEnabled() != msaa) renderer.toggleMsaa();
            for (const bool coverage : {true, false})
            {
                scene.branchAppearance = coverage ? BranchAppearance::Coverage : BranchAppearance::Shaded;
                for (const float distance : distances)
                {
                    const auto title = L"Oak Branch voxel benchmark | " + std::to_wstring(scene.instanceCount()) +
                                       L" copies | distance " + std::to_wstring(static_cast<int>(distance));
                    SetWindowTextW(window, title.c_str());
                    const CameraView pose{focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), distance};
                    const auto view = pose.matrix();
                    for (unsigned repetition = 0; repetition < repetitions + 2; ++repetition)
                    {
                        unsigned cameraInstances = 0, shadowInstances = 0;
                        for (unsigned offset = 0; offset < std::size(modes); ++offset)
                        {
                            if (!processMessages()) return 2;
                            const auto index = (offset + repetition) % std::size(modes);
                            scene.branchVoxelView = modes[index];
                            scene.branchVoxelTightProxies = !compareProxyBounds || modes[index] != BranchVoxelView::Fine;
                            scene.branchVoxelDirectionalNormals = directional && modes[index] == BranchVoxelView::Coarse;
                            scene.branchVoxelShadowReceiver = shadowReceiver && modes[index] == BranchVoxelView::Coarse
                                ? VoxelShadowReceiver::BakedLeaf : VoxelShadowReceiver::CellEntry;
                            const auto timing = renderer.drawFrame(scene, view, 0, {});
                            if (!timing.gpuCameraMilliseconds || !timing.gpuShadowMilliseconds || !timing.gpuMilliseconds)
                                throw std::runtime_error("GPU timestamps are unavailable for the voxel benchmark.");
                            if (!offset)
                            {
                                cameraInstances = timing.camera.visibleInstances;
                                shadowInstances = timing.shadow.visibleInstances;
                            }
                            const auto voxelIndex = branchVoxelIndex(modes[index]);
                            const auto expectedBricks = voxelIndex ? scene.branchVoxels[*voxelIndex]->bricks.size() * cameraInstances : 0;
                            const auto expectedTriangles = voxelIndex ? 12 * expectedBricks : shadowTriCount * cameraInstances;
                            if ((farRoute && cameraInstances != scene.instanceCount()) ||
                                timing.camera.visibleInstances != cameraInstances || timing.shadow.visibleInstances != shadowInstances ||
                                timing.shadow.foliageTriangles != shadowTriCount * shadowInstances ||
                                timing.camera.voxelBricks != expectedBricks || timing.camera.foliageTriangles != expectedTriangles)
                                throw std::runtime_error("Voxel benchmark changed the caster set or submitted the wrong geometry.");
                            if (repetition < 2) continue;
                            out << adapter << ',' << std::hex << sourceHash << std::dec << ',' << viewport.x << ',' << viewport.y
                                << ',' << (msaa ? 4 : 1) << ',' << (coverage ? "coverage" : "shaded") << ',' << scene.instanceCount()
                                << ',' << distance << ',' << labels[index] << ',' << repetition - 2 << ',' << cameraInstances << ','
                                << shadowInstances << ',' << timing.camera.voxelBricks << ',' << timing.camera.foliageCalls << ','
                                << timing.camera.foliageTriangles << ',' << timing.shadow.foliageTriangles << ','
                                << timing.camera.triangles << ',' << timing.shadow.triangles << ',' << timing.cpuMilliseconds << ','
                                << *timing.gpuCameraMilliseconds << ',' << *timing.gpuShadowMilliseconds << ',' << *timing.gpuMilliseconds;
                            if (farRoute)
                            {
                                out << ',' << (voxelIndex ? scene.branchVoxels[*voxelIndex]->cellSize : 0) << ',';
                                if (voxelIndex) out << std::hex << voxelHashes[*voxelIndex] << std::dec;
                            }
                            out << '\n';
                        }
                    }
                    out.flush();
                }
            }
        }
    }
    out.close();
    return 0;
}

int runBranchLightingDiagnostics(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                                 bool directional, bool compareMidpoint, bool compareCasters)
{
    constexpr auto voxel = BranchVoxelView::CellOneEighth;
    constexpr auto triangle = BranchVoxelView::Lod6;
    const auto voxelIndex = *branchVoxelIndex(voxel);
    if (!scene.fixedLod || !scene.branchVoxelCoverageOptimized[voxelIndex] || !scene.branchVoxelNormals[voxelIndex])
        throw std::runtime_error("Build oak_branch_normals before running lighting diagnostics.");
    if (directional && !scene.branchVoxelNormals[voxelIndex]->hasDirectionalWeights)
        throw std::runtime_error("Rebuild oak_branch_normals for view-dependent weights.");
    const bool hasReceivers = scene.branchVoxelNormals[voxelIndex]->hasShadowReceivers;
    scene.branchVoxelUseNormals = true;
    scene.branchVoxelDirectionalNormals = directional;
    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
    scene.sunElevation = 54.5715f;
    scene.setBranchGrid(1);
    if (renderer.msaaEnabled()) renderer.toggleMsaa();
    renderer.setScene(scene);

    auto& foliageShadows = scene.meshShadows[scene.fixedLod->meshIndex];
    const auto casterTriangles = scene.fixedLod->levels[fixedLodTwoTriangleLevel].indices.size() / 3;
    const auto casterBricks = scene.branchVoxels[voxelIndex]->bricks.size();
    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::badbit | std::ios::failbit);
    csv.imbue(std::locale::classic());
    csv << "distance,orbit_seconds,yaw,pitch,sun_azimuth,shared_pixels,lod6_base,voxel_base,lod6_diffuse,voxel_diffuse,"
           "lod6_shadow_visibility,voxel_shadow_visibility,lod6_unshadowed,voxel_unshadowed,lod6_shaded,voxel_shaded,"
           "required_direct_gain,unshadowed_l1,gain_1025_l1,gain_105_l1,gain_110_l1,gain_115_l1,shadow_l1,shaded_l1,"
           "bias_025_visibility,bias_025_shadow_l1,bias_025_shaded_l1,bias_050_visibility,bias_050_shadow_l1,"
           "bias_050_shaded_l1,bias_100_visibility,bias_100_shadow_l1,bias_100_shaded_l1,"
           "reference_visibility_shaded,reference_visibility_shaded_l1";
    if (hasReceivers) csv << ",receiver_visibility,receiver_shadow_l1,receiver_shaded,receiver_shaded_l1";
    if (compareMidpoint) csv << ",midpoint_visibility,midpoint_shadow_l1,midpoint_shaded,midpoint_shaded_l1";
    if (compareCasters)
        csv << ",voxel_caster_entry_visibility,voxel_caster_entry_shadow_l1,voxel_caster_entry_shaded,voxel_caster_entry_shaded_l1"
               ",voxel_caster_midpoint_visibility,voxel_caster_midpoint_shadow_l1,voxel_caster_midpoint_shaded,voxel_caster_midpoint_shaded_l1";
    csv << '\n'
        << std::fixed << std::setprecision(8);

    const auto srgbToLinear = [](unsigned byte) {
        const double value = byte / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&](std::uint32_t pixel) {
        return 0.2126 * srgbToLinear(pixel & 255) + 0.7152 * srgbToLinear((pixel >> 8) & 255) +
               0.0722 * srgbToLinear((pixel >> 16) & 255);
    };
    const auto scalar = [](std::uint32_t pixel) { return (pixel & 255) / 255.0; };
    const auto lightFactor = [](double diffuse) { return 0.35 + 0.65 * diffuse; };

    for (const float distance : {250.0f, branchOrbitDistance})
    {
        const bool farView = distance >= branchVoxelFarDistances.front();
        renderer.updateCameraProjection(farView ? branchVoxelFarNearPlane : scene.nearPlane,
                                        farView ? branchVoxelFarClipPlane : scene.farPlane());
        for (const float time : {0.0f, 5.0f, 10.0f, 157.0f / 12.0f, 196.0f / 12.0f, 22.0f, 26.0f})
        {
            const auto pose = scene.branchOrbitView(time, distance);
            const auto view = pose.matrix();
            std::vector<std::uint32_t> frame;
            const auto capture = [&](BranchVoxelView mode, BranchAppearance appearance) {
                if (!processMessages()) throw std::runtime_error("Lighting diagnostic interrupted.");
                scene.branchVoxelView = mode;
                scene.branchAppearance = appearance;
                const auto timing = renderer.drawFrame(scene, view, 0, {}, {}, &frame);
                if (compareMidpoint)
                {
                    const auto expectedBricks = foliageShadows.cast && scene.diagnosticVoxelShadowCasters ? casterBricks : 0;
                    const auto expectedTriangles = foliageShadows.cast ? (expectedBricks ? 12 * expectedBricks : casterTriangles) : 0;
                    if (timing.shadow.voxelBricks != expectedBricks || timing.shadow.foliageTriangles != expectedTriangles)
                        throw std::runtime_error("Lighting diagnostic submitted the wrong foliage shadow casters.");
                }
            };
            capture(triangle, BranchAppearance::Coverage);
            const auto triangleCoverage = frame;
            capture(voxel, BranchAppearance::Coverage);
            std::vector<std::size_t> shared;
            constexpr std::uint32_t coverageColor = 0xff528c26;
            for (std::size_t i = 0; i < frame.size(); ++i)
                if (frame[i] == coverageColor && triangleCoverage[i] == coverageColor) shared.push_back(i);
            if (shared.empty()) throw std::runtime_error("Lighting diagnostic found no jointly covered pixels.");
            if (compareMidpoint)
            {
                const auto entryCoverage = frame;
                scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellMidpoint;
                capture(voxel, BranchAppearance::Coverage);
                if (frame != entryCoverage) throw std::runtime_error("Midpoint receiver changed camera coverage.");
                if (compareCasters)
                {
                    scene.diagnosticVoxelShadowCasters = true;
                    capture(voxel, BranchAppearance::Coverage);
                    for (std::size_t i = 0; i < frame.size(); ++i)
                        if ((frame[i] == coverageColor) != (entryCoverage[i] == coverageColor))
                            throw std::runtime_error("Changing shadow casters changed voxel camera coverage.");
                    scene.diagnosticVoxelShadowCasters = false;
                }
                scene.branchVoxelDepthCapture = true;
                capture(voxel, BranchAppearance::Shaded);
                const auto midpointDepth = frame;
                scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
                capture(voxel, BranchAppearance::Shaded);
                if (frame != midpointDepth) throw std::runtime_error("Midpoint receiver changed camera depth.");
                if (compareCasters)
                {
                    scene.diagnosticVoxelShadowCasters = true;
                    capture(voxel, BranchAppearance::Shaded);
                    for (std::size_t i = 0; i < frame.size(); ++i)
                        if (((frame[i] >> 24) == 64 || (midpointDepth[i] >> 24) == 64) && frame[i] != midpointDepth[i])
                            throw std::runtime_error("Changing shadow casters changed voxel camera depth.");
                    scene.diagnosticVoxelShadowCasters = false;
                }
                scene.branchVoxelDepthCapture = false;
            }
            const auto samples = [&](BranchVoxelView mode, BranchAppearance appearance, const auto& decode) {
                capture(mode, appearance);
                std::vector<double> values;
                values.reserve(shared.size());
                for (const auto index : shared) values.push_back(decode(frame[index]));
                return values;
            };
            const double count = static_cast<double>(shared.size());
            const auto mean = [count](const auto& values) {
                return std::accumulate(values.begin(), values.end(), 0.0) / count;
            };
            const auto distanceL1 = [&](const auto& lhs, const auto& rhs) {
                double sum = 0;
                for (std::size_t i = 0; i < shared.size(); ++i) sum += std::abs(lhs[i] - rhs[i]);
                return sum / count;
            };
            const auto tBase = samples(triangle, BranchAppearance::BaseColor, luminance);
            const auto vBase = samples(voxel, BranchAppearance::BaseColor, luminance);
            for (const float sun : {-100.0f, 80.0f})
            {
                scene.sunAzimuth = sun;
                renderer.updateSun(scene);
                foliageShadows.receive = false;
                const auto tUnshadowed = samples(triangle, BranchAppearance::Shaded, luminance);
                const auto vUnshadowed = samples(voxel, BranchAppearance::Shaded, luminance);
                if (compareMidpoint)
                {
                    const auto entryUnshadowed = frame;
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellMidpoint;
                    capture(voxel, BranchAppearance::Shaded);
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
                    if (frame != entryUnshadowed) throw std::runtime_error("Midpoint receiver changed unshadowed shading.");
                }
                const auto tDiffuse = samples(triangle, BranchAppearance::Diffuse, scalar);
                const auto vDiffuse = samples(voxel, BranchAppearance::Diffuse, scalar);
                foliageShadows.receive = true;
                const auto tVisibility = samples(triangle, BranchAppearance::ReceivedShadows, scalar);
                const auto vVisibility = samples(voxel, BranchAppearance::ReceivedShadows, scalar);
                const auto tShaded = samples(triangle, BranchAppearance::Shaded, luminance);
                const auto vShaded = samples(voxel, BranchAppearance::Shaded, luminance);

                constexpr std::array gains{1.025, 1.05, 1.10, 1.15};
                std::array<double, gains.size()> gainErrors{};
                double voxelDirect = 0;
                double reconstructionError = 0;
                double referenceVisibilityLuma = 0;
                double referenceVisibilityError = 0;
                for (std::size_t i = 0; i < shared.size(); ++i)
                {
                    voxelDirect += 0.65 * vBase[i] * vDiffuse[i];
                    const auto withReferenceVisibility = vBase[i] * lightFactor(vDiffuse[i] * tVisibility[i]);
                    referenceVisibilityLuma += withReferenceVisibility;
                    referenceVisibilityError += std::abs(tShaded[i] - withReferenceVisibility);
                    reconstructionError += std::abs(tUnshadowed[i] - tBase[i] * lightFactor(tDiffuse[i])) +
                                           std::abs(vUnshadowed[i] - vBase[i] * lightFactor(vDiffuse[i]));
                    for (std::size_t gain = 0; gain < gains.size(); ++gain)
                    {
                        const auto predicted = vBase[i] * lightFactor(std::min(1.0, gains[gain] * vDiffuse[i]));
                        gainErrors[gain] += std::abs(tUnshadowed[i] - predicted);
                    }
                }
                if (reconstructionError / (2 * count) > 0.002)
                    throw std::runtime_error("Diffuse diagnostics disagree with the unshadowed foliage renders.");
                const double requiredGain = (mean(tUnshadowed) - 0.35 * mean(vBase)) * count / voxelDirect;
                csv << distance << ',' << time << ',' << XMConvertToDegrees(pose.yaw) << ',' << XMConvertToDegrees(pose.pitch)
                    << ',' << sun << ',' << shared.size() << ',' << mean(tBase) << ',' << mean(vBase) << ',' << mean(tDiffuse)
                    << ',' << mean(vDiffuse) << ',' << mean(tVisibility) << ',' << mean(vVisibility) << ','
                    << mean(tUnshadowed) << ',' << mean(vUnshadowed) << ',' << mean(tShaded) << ',' << mean(vShaded)
                    << ',' << requiredGain << ',' << distanceL1(tUnshadowed, vUnshadowed);
                for (const auto error : gainErrors) csv << ',' << error / count;
                csv << ',' << distanceL1(tVisibility, vVisibility) << ',' << distanceL1(tShaded, vShaded);
                for (const float offset : {0.25f, 0.5f, 1.0f})
                {
                    scene.branchVoxelShadowOffsetTexels = offset;
                    const auto biased = samples(voxel, BranchAppearance::ReceivedShadows, scalar);
                    const auto shaded = samples(voxel, BranchAppearance::Shaded, luminance);
                    csv << ',' << mean(biased) << ',' << distanceL1(tVisibility, biased) << ',' << distanceL1(tShaded, shaded);
                }
                scene.branchVoxelShadowOffsetTexels = 0;
                csv << ',' << referenceVisibilityLuma / count << ',' << referenceVisibilityError / count;
                if (hasReceivers)
                {
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::BakedLeaf;
                    const auto visibility = samples(voxel, BranchAppearance::ReceivedShadows, scalar);
                    const auto shaded = samples(voxel, BranchAppearance::Shaded, luminance);
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
                    csv << ',' << mean(visibility) << ',' << distanceL1(tVisibility, visibility)
                        << ',' << mean(shaded) << ',' << distanceL1(tShaded, shaded);
                }
                if (compareMidpoint)
                {
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellMidpoint;
                    const auto visibility = samples(voxel, BranchAppearance::ReceivedShadows, scalar);
                    const auto shaded = samples(voxel, BranchAppearance::Shaded, luminance);
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
                    csv << ',' << mean(visibility) << ',' << distanceL1(tVisibility, visibility)
                        << ',' << mean(shaded) << ',' << distanceL1(tShaded, shaded);
                }
                if (compareCasters)
                {
                    scene.diagnosticVoxelShadowCasters = true;
                    for (const auto receiver : {VoxelShadowReceiver::CellEntry, VoxelShadowReceiver::CellMidpoint})
                    {
                        scene.branchVoxelShadowReceiver = receiver;
                        const auto visibility = samples(voxel, BranchAppearance::ReceivedShadows, scalar);
                        const auto shaded = samples(voxel, BranchAppearance::Shaded, luminance);
                        csv << ',' << mean(visibility) << ',' << distanceL1(tVisibility, visibility)
                            << ',' << mean(shaded) << ',' << distanceL1(tShaded, shaded);
                        if (distance == 250 && time == 0 && receiver == VoxelShadowReceiver::CellEntry)
                        {
                            foliageShadows.cast = false;
                            const auto withoutFoliage = samples(voxel, BranchAppearance::ReceivedShadows, scalar);
                            foliageShadows.cast = true;
                            if (visibility == withoutFoliage)
                                throw std::runtime_error("Voxel shadow casters did not affect shadow visibility.");
                        }
                    }
                    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
                    scene.diagnosticVoxelShadowCasters = false;
                }
                csv << '\n' << std::flush;
            }
        }
    }
    return 0;
}

namespace
{
struct TransitionCapture
{
    double coverageBias;
    double coverageError;
    double luminanceBias;
    double luminanceError;
    unsigned commonPixels;
};

TransitionCapture compareTransitionFrames(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after,
                                          std::span<const std::uint32_t> beforeMask, std::span<const std::uint32_t> afterMask)
{
    const auto luma = [](std::uint32_t p) {
        return 0.2126 * (p & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * ((p >> 16) & 255);
    };
    double referenceCoverage = 0, coverageDelta = 0, coverageError = 0;
    double referenceLuma = 0, lumaDelta = 0, lumaError = 0;
    unsigned common = 0;
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        const auto a = 255 - static_cast<int>(beforeMask[i] >> 24);
        const auto b = 255 - static_cast<int>(afterMask[i] >> 24);
        referenceCoverage += a;
        coverageDelta += b - a;
        coverageError += std::abs(b - a);
        // Compare luminance on pixels with the same majority foliage coverage.
        if (a != b || a < 191) continue;
        const double oldLuma = luma(before[i]);
        const double delta = luma(after[i]) - oldLuma;
        referenceLuma += oldLuma;
        lumaDelta += delta;
        lumaError += std::abs(delta);
        ++common;
    }
    if (!referenceCoverage || !referenceLuma) throw std::runtime_error("The transition capture found no comparable foliage.");
    return {100 * coverageDelta / referenceCoverage, 100 * coverageError / referenceCoverage,
            100 * lumaDelta / referenceLuma, 100 * lumaError / referenceLuma, common};
}

void writeTransitionPair(const std::filesystem::path& path, std::span<const std::uint32_t> before,
                         std::span<const std::uint32_t> after, std::span<const std::uint32_t> beforeMask,
                         std::span<const std::uint32_t> afterMask, int width, int height)
{
    int x0 = 0, y0 = 0, x1 = width - 1, y1 = height - 1;
    if (!beforeMask.empty())
    {
        x0 = width; y0 = height; x1 = 0; y1 = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto p = std::size_t(y) * width + x;
                if ((beforeMask[p] >> 24) == 255 && (afterMask[p] >> 24) == 255) continue;
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
        constexpr int padding = 12;
        x0 = std::max(0, x0 - padding); x1 = std::min(width - 1, x1 + padding);
        y0 = std::max(0, y0 - padding); y1 = std::min(height - 1, y1 + padding);
    }
    const int cropWidth = x1 - x0 + 1;
    const int cropHeight = y1 - y0 + 1;
    constexpr int gutter = 8;
    const int resultWidth = 2 * cropWidth + gutter;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = resultWidth;
    info.biHeight = -cropHeight;
    info.biPlanes = 1;
    info.biBitCount = 32;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + resultWidth * cropHeight * sizeof(std::uint32_t);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info), sizeof(info));
    std::vector<std::uint32_t> row(resultWidth, 0x00202020u);
    const auto toBgr = [](std::uint32_t rgb) { return ((rgb & 255) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 255); };
    for (int y = y0; y <= y1; ++y)
    {
        const auto start = std::size_t(y) * width + x0;
        std::transform(before.begin() + start, before.begin() + start + cropWidth, row.begin(), toBgr);
        std::transform(after.begin() + start, after.begin() + start + cropWidth, row.begin() + cropWidth + gutter, toBgr);
        file.write(reinterpret_cast<const char*>(row.data()), row.size() * sizeof(row.front()));
    }
}
}

int runBranchTransitionBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                                 float entryPixels)
{
    using Clock = std::chrono::steady_clock;
    if (!scene.hasAutomaticBranchVoxels()) throw std::runtime_error("Build oak_branch_normals before testing the transition.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The transition comparison requires 4x MSAA.");
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    scene.branchVoxelEnterPixels = entryPixels;
    scene.branchVoxelView = BranchVoxelView::Automatic;
    scene.branchVoxelUseNormals = true;
    scene.windStrength = 0;
    scene.sunElevation = 54.5715f;
    scene.setBranchGrid(1);
    renderer.setScene(scene);
    renderer.updateCameraProjection(branchVoxelFarNearPlane, branchVoxelFarClipPlane);
    const auto viewport = renderer.viewportSize();
    const auto bricks = scene.branchVoxels[automaticBranchVoxelIndex]->bricks.size();
    const auto lod6Triangles = scene.fixedLod->levels[fixedLodTwoTriangleLevel].indices.size() / 3;
    const auto selected = [&] { return std::ranges::count(scene.branchLodSelection.voxelCut, allVoxelRegions); };
    const auto capture = [&](BranchVoxelView mode, BranchAppearance appearance, const XMFLOAT4X4& view) {
        if (!processMessages()) throw std::runtime_error("Transition capture interrupted.");
        scene.branchVoxelView = mode;
        scene.branchAppearance = appearance;
        std::vector<std::uint32_t> pixels;
        renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
        return pixels;
    };
    const auto setPose = [&](float angle, float distance, BranchVoxelView mode = BranchVoxelView::Automatic) {
        const auto view = scene.branchOrbitView(angle, distance).matrix();
        scene.branchVoxelView = mode;
        renderer.updateBranchLod(scene, view);
        return view;
    };
    std::array<std::size_t, voxelBranchRegionCount> regionTriangles{}, regionBricks{};
    for (std::size_t region = 0; region < voxelBranchRegionCount; ++region)
    {
        const auto& lod = *scene.fixedLod;
        const auto first = voxelRegionGroupStart(region, lod.groups.size());
        const auto end = voxelRegionGroupStart(region + 1, lod.groups.size());
        regionTriangles[region] = lod.indicesForGroups(fixedLodTwoTriangleLevel, first, end - 1).indexCount / 3;
        if (scene.branchVoxelRegions) regionBricks[region] = (*scene.branchVoxelRegions)[region].foliage.bricks.size();
    }
    const auto expectedGeometry = [&](BranchVoxelView mode) {
        std::size_t voxelBricks = 0;
        std::size_t triangles = 0;
        unsigned fullVoxels = 0, mixed = 0, regions = 0;
        const auto& cut = scene.branchLodSelection.voxelCut;
        for (std::size_t instance = 0; instance < scene.objects.size(); ++instance)
        {
            const auto mask = automaticBranchVoxels(mode) ? cut[instance] : std::uint8_t{0};
            regions += std::popcount(mask);
            if (mask == allVoxelRegions)
            {
                ++fullVoxels;
                voxelBricks += bricks;
                continue;
            }
            mixed += mask != 0;
            for (std::size_t region = 0; region < voxelBranchRegionCount; ++region)
            {
                if (mask & (1u << region))
                    voxelBricks += regionBricks[region];
                else
                    triangles += regionTriangles[region];
            }
        }
        return std::tuple{voxelBricks, triangles + voxelBricks * 12, fullVoxels, mixed, regions};
    };

    const float center = branchOrbitDistance * branchVoxelDefaultEntryPixels / entryPixels;
    auto qualityReport = report;
    qualityReport.replace_filename(report.stem().native() + L"-quality.csv");
    std::ofstream quality(qualityReport, std::ios::binary | std::ios::trunc);
    quality.exceptions(std::ios::failbit | std::ios::badbit);
    quality.imbue(std::locale::classic());
    quality << "orbit_seconds,sun_azimuth,switch,distance,entry_pixels,coverage_bias_pct,coverage_l1_pct,"
               "display_luminance_bias_pct,display_luminance_l1_pct,common_pixels\n"
            << std::fixed << std::setprecision(6);
    constexpr float routeAngles[]{0, 5, 10, 157.0f / 12.0f, 196.0f / 12.0f, 22, 26};
    for (unsigned pose = 0; pose < std::size(routeAngles); ++pose)
    {
        const float angle = routeAngles[pose];
        setPose(angle, center * 0.8f);
        if (selected()) throw std::runtime_error("The transition route did not start with triangles.");
        for (const bool retreat : {true, false})
        {
            const auto previous = selected();
            const float start = center * (retreat ? 0.8f : 1.25f);
            const float end = center * (retreat ? 1.25f : 0.8f);
            for (unsigned step = 0; step <= 120; ++step)
            {
                const float distance = std::lerp(start, end, step / 120.0f);
                const auto view = setPose(angle, distance);
                if (selected() == previous) continue;
                scene.branchLodSelection.frozen = true;
                setPose(angle, end);
                setPose(angle, start);
                if (selected() == previous) throw std::runtime_error("Freeze cut did not hold the selected representation.");
                scene.branchLodSelection.frozen = false;
                const auto maskBefore = capture(BranchVoxelView::Lod6, BranchAppearance::CoverageMask, view);
                const auto maskAfter = capture(BranchVoxelView::CellOneEighth, BranchAppearance::CoverageMask, view);
                for (const float azimuth : {-100.0f, 80.0f})
                {
                    scene.sunAzimuth = azimuth;
                    renderer.updateSun(scene);
                    const auto before = capture(BranchVoxelView::Lod6, BranchAppearance::Shaded, view);
                    const auto after = capture(BranchVoxelView::CellOneEighth, BranchAppearance::Shaded, view);
                    const auto automatic = capture(BranchVoxelView::Automatic, BranchAppearance::Shaded, view);
                    if (automatic != (retreat ? after : before))
                        throw std::runtime_error("An automatic single-branch frame disagrees with its forced representation.");
                    const auto metrics = compareTransitionFrames(before, after, maskBefore, maskAfter);
                    quality << angle << ',' << azimuth << ',' << (retreat ? "retreat" : "approach") << ',' << distance
                            << ',' << entryPixels << ',' << metrics.coverageBias << ',' << metrics.coverageError
                            << ',' << metrics.luminanceBias << ',' << metrics.luminanceError << ',' << metrics.commonPixels << '\n';
                    if (azimuth == -100)
                    {
                        auto picture = report;
                        picture.replace_filename(report.stem().native() + L"-" + std::to_wstring(pose) +
                                                 (retreat ? L"-retreat.bmp" : L"-approach.bmp"));
                        writeTransitionPair(picture, before, after, maskBefore, maskAfter,
                                            static_cast<int>(viewport.x), static_cast<int>(viewport.y));
                    }
                }
                break;
            }
            if (selected() == previous) throw std::runtime_error("The single branch did not switch on the route.");
        }
    }
    quality.close();

    if (scene.branchVoxelRegions)
    {
        auto regionalReport = report;
        regionalReport.replace_filename(report.stem().native() + L"-regions.csv");
        std::ofstream regional(regionalReport, std::ios::binary | std::ios::trunc);
        regional.exceptions(std::ios::failbit | std::ios::badbit);
        regional.imbue(std::locale::classic());
        regional << "orbit_seconds,sun_azimuth,distance,voxel_mask,bricks,submitted_triangles,coverage_bias_pct,"
                    "coverage_l1_pct,display_luminance_bias_pct,display_luminance_l1_pct,common_pixels\n"
                 << std::fixed << std::setprecision(6);
        for (unsigned pose = 0; pose < std::size(routeAngles); ++pose)
        {
            const auto angle = routeAngles[pose];
            const auto mode = BranchVoxelView::Regional;
            setPose(angle, center * 0.85f, mode);
            float distance = 0;
            unsigned best = 0;
            for (unsigned step = 0; step <= 300; ++step)
            {
                const float candidate = center * std::lerp(0.95f, 1.1f, step / 300.0f);
                setPose(angle, candidate, mode);
                const auto active = static_cast<unsigned>(std::popcount(scene.branchLodSelection.voxelCut[0]));
                const auto balance = std::min(active, static_cast<unsigned>(voxelBranchRegionCount) - active);
                if (balance <= best) continue;
                best = balance;
                distance = candidate;
            }
            if (!best) throw std::runtime_error("The region route found no intra-branch mixed cut.");
            setPose(angle, center * 0.85f, mode);
            const auto view = setPose(angle, distance, mode);
            const auto mask = scene.branchLodSelection.voxelCut[0];
            const auto [drawBricks, drawTriangles, full, mixed, regions] = expectedGeometry(mode);
            if (full || mixed != 1 || !regions) throw std::runtime_error("The region benchmark did not select a mixed branch.");
            scene.branchLodSelection.frozen = true;
            setPose(angle, center * 0.85f, mode);
            if (scene.branchLodSelection.voxelCut[0] != mask) throw std::runtime_error("Freeze cut did not hold mixed regions.");
            scene.branchLodSelection.frozen = false;
            const auto triangleMask = capture(BranchVoxelView::Lod6, BranchAppearance::CoverageMask, view);
            const auto regionalMask = capture(mode, BranchAppearance::CoverageMask, view);
            const auto tints = capture(mode, BranchAppearance::Representation, view);
            bool blue = false, orange = false;
            for (std::size_t i = 0; i < tints.size(); ++i)
            {
                if ((regionalMask[i] >> 24) > 64) continue;
                const auto red = tints[i] & 255, b = (tints[i] >> 16) & 255;
                blue |= red < 100 && b > 180;
                orange |= red > 210 && b < 95;
            }
            if (!blue || !orange) throw std::runtime_error("The mixed branch did not render both representation tints.");
            for (const float azimuth : {-100.0f, 80.0f})
            {
                scene.sunAzimuth = azimuth;
                renderer.updateSun(scene);
                const auto triangles = capture(BranchVoxelView::Lod6, BranchAppearance::Shaded, view);
                const auto cut = capture(mode, BranchAppearance::Shaded, view);
                const auto metrics = compareTransitionFrames(triangles, cut, triangleMask, regionalMask);
                regional << angle << ',' << azimuth << ',' << distance << ',' << static_cast<unsigned>(mask) << ','
                         << drawBricks << ',' << drawTriangles << ',' << metrics.coverageBias << ',' << metrics.coverageError
                         << ',' << metrics.luminanceBias << ',' << metrics.luminanceError << ',' << metrics.commonPixels << '\n';
                if (azimuth == -100)
                {
                    auto picture = report;
                    picture.replace_filename(report.stem().native() + L"-" + std::to_wstring(pose) + L"-regional.bmp");
                    writeTransitionPair(picture, triangles, cut, triangleMask, regionalMask,
                                        static_cast<int>(viewport.x), static_cast<int>(viewport.y));
                    picture.replace_filename(report.stem().native() + L"-" + std::to_wstring(pose) + L"-tints.bmp");
                    writeTransitionPair(picture, cut, tints, regionalMask, regionalMask,
                                        static_cast<int>(viewport.x), static_cast<int>(viewport.y));
                }
            }
            for (unsigned bits = 0; bits <= allVoxelRegions; ++bits)
            {
                scene.branchVoxelView = mode;
                scene.branchLodSelection.voxelCut[0] = static_cast<std::uint8_t>(bits);
                const auto [wantedBricks, wantedTriangles, ignoredFull, ignoredMixed, ignoredRegions] = expectedGeometry(mode);
                const auto frame = renderer.drawFrame(scene, view, 0, {});
                if (frame.camera.voxelBricks != wantedBricks || frame.camera.foliageTriangles != wantedTriangles ||
                    frame.shadow.foliageTriangles != lod6Triangles)
                    throw std::runtime_error("A region cut dropped or duplicated geometry in the camera or shadow pass.");
            }
            setPose(angle, center * 1.25f, mode);
            const auto farView = scene.branchOrbitView(angle, center * 1.25f).matrix();
            const auto all = capture(mode, BranchAppearance::Shaded, farView);
            const auto whole = capture(BranchVoxelView::CellOneEighth, BranchAppearance::Shaded, farView);
            if (all != whole) throw std::runtime_error("The all-voxel regional cut differs from the existing whole-branch bake.");
        }
    }

    auto routeReport = report;
    routeReport.replace_filename(report.stem().native() + L"-route.csv");
    std::ofstream route(routeReport, std::ios::binary | std::ios::trunc);
    route.exceptions(std::ios::failbit | std::ios::badbit);
    route.imbue(std::locale::classic());
    route << "mode,orbit_seconds,distance,from_mask,to_mask,coverage_bias_pct,coverage_l1_pct,"
             "display_luminance_bias_pct,display_luminance_l1_pct,common_pixels\n" << std::fixed << std::setprecision(6);
    scene.sunAzimuth = -100;
    renderer.updateSun(scene);
    for (const auto candidate : {BranchVoxelView::Automatic, BranchVoxelView::Regional})
    {
        if (candidate == BranchVoxelView::Regional && !scene.branchVoxelRegions) continue;
        std::uint8_t previous = 0;
        unsigned changes = 0;
        constexpr unsigned samples = 240;
        for (unsigned step = 0; step <= samples; ++step)
        {
            if (!processMessages()) return 2;
            const float fraction = static_cast<float>(step) / samples;
            const float angle = branchOrbitDuration * fraction;
            const float distance = center * (1.05f - 0.15f * std::cos(XM_2PI * fraction));
            const auto view = setPose(angle, distance, candidate);
            auto& cut = scene.branchLodSelection.voxelCut.front();
            const auto current = cut;
            const auto [wantedBricks, wantedTriangles, ignoredFull, ignoredMixed, ignoredRegions] = expectedGeometry(candidate);
            scene.branchAppearance = BranchAppearance::Shaded;
            const auto frame = renderer.drawFrame(scene, view, 0, {});
            if (frame.camera.voxelBricks != wantedBricks || frame.camera.foliageTriangles != wantedTriangles ||
                frame.shadow.foliageTriangles != lod6Triangles)
                throw std::runtime_error("The moving route changed the expected camera or shadow geometry.");
            if (current == previous) continue;
            cut = previous;
            const auto beforeMask = capture(candidate, BranchAppearance::CoverageMask, view);
            const auto before = capture(candidate, BranchAppearance::Shaded, view);
            cut = current;
            const auto afterMask = capture(candidate, BranchAppearance::CoverageMask, view);
            const auto after = capture(candidate, BranchAppearance::Shaded, view);
            const auto metrics = compareTransitionFrames(before, after, beforeMask, afterMask);
            route << (candidate == BranchVoxelView::Regional ? "regions" : "branch") << ',' << angle << ',' << distance
                  << ',' << unsigned(previous) << ',' << unsigned(current) << ',' << metrics.coverageBias << ','
                  << metrics.coverageError << ',' << metrics.luminanceBias << ',' << metrics.luminanceError
                  << ',' << metrics.commonPixels << '\n';
            previous = current;
            ++changes;
        }
        if (previous || changes < 2)
            throw std::runtime_error("The moving orbit did not start and end with triangles after visiting voxels.");
    }
    route.close();

    scene.setBranchGrid(branchGridSide);
    scene.sunAzimuth = -100;
    renderer.setScene(scene);
    renderer.updateCameraProjection(branchVoxelFarNearPlane, branchVoxelFarClipPlane);
    scene.branchAppearance = BranchAppearance::Shaded;
    const auto count = scene.instanceCount();
    std::ofstream timing(report, std::ios::binary | std::ios::trunc);
    timing.exceptions(std::ios::failbit | std::ios::badbit);
    timing.imbue(std::locale::classic());
    timing << "adapter,orbit_seconds,distance,route,mode,repetition,triangle_branches,voxel_branches,mixed_branches,voxel_regions,voxel_bricks,"
              "camera_foliage_triangles,shadow_foliage_triangles,camera_foliage_calls,cpu_select_ms,cpu_render_ms,"
              "gpu_camera_ms,gpu_shadow_ms,gpu_frame_ms\n" << std::fixed << std::setprecision(6);
    for (const float angle : {routeAngles[0], routeAngles[3], routeAngles[4]})
    {
        setPose(angle, center * 0.8f);
        float mixedDistance = center;
        unsigned bestBalance = 0;
        for (unsigned step = 0; step <= 200; ++step)
        {
            const float distance = center * std::lerp(0.8f, 1.2f, step / 200.0f);
            setPose(angle, distance);
            const auto voxels = static_cast<unsigned>(selected());
            const auto balance = std::min(voxels, count - voxels);
            if (balance <= bestBalance) continue;
            bestBalance = balance;
            mixedDistance = distance;
        }
        if (!bestBalance) throw std::runtime_error("The control grid found no mixed triangle and voxel cut.");
        for (const auto candidate : {BranchVoxelView::Automatic, BranchVoxelView::Regional})
        {
            if (candidate == BranchVoxelView::Regional && !scene.branchVoxelRegions) continue;
            float candidateMixedDistance = mixedDistance;
            if (candidate == BranchVoxelView::Regional)
            {
                setPose(angle, center * 0.8f, candidate);
                unsigned bestMixed = 0;
                for (unsigned step = 0; step <= 300; ++step)
                {
                    const float distance = center * std::lerp(0.95f, 1.1f, step / 300.0f);
                    setPose(angle, distance, candidate);
                    const auto mixed = static_cast<unsigned>(std::ranges::count_if(scene.branchLodSelection.voxelCut,
                        [](auto mask) { return mask && mask != allVoxelRegions; }));
                    if (mixed <= bestMixed) continue;
                    bestMixed = mixed;
                    candidateMixedDistance = distance;
                }
                if (!bestMixed) throw std::runtime_error("The control grid found no intra-branch mixed cut.");
            }
            for (const bool mixed : {true, false})
            {
                const float distance = mixed ? candidateMixedDistance : center * 1.23f;
                setPose(angle, center * 0.8f, candidate);
                const auto view = setPose(angle, distance, candidate);
                const auto [expectedBricks, expectedTriangles, expectedVoxels, expectedMixed, expectedRegions] = expectedGeometry(candidate);
                if ((mixed && (candidate == BranchVoxelView::Regional ? !expectedMixed : (!expectedVoxels || expectedVoxels == count))) ||
                    (!mixed && expectedVoxels != count))
                    throw std::runtime_error("The control pose selected an unexpected triangle and voxel mix.");
                for (unsigned rep = 0; rep < 11; ++rep)
                {
                    for (unsigned offset = 0; offset < 2; ++offset)
                    {
                        if (!processMessages()) return 2;
                        const bool automatic = (rep + offset) % 2 != 0;
                        scene.branchVoxelView = automatic ? candidate : BranchVoxelView::Off;
                        const auto start = Clock::now();
                        renderer.updateBranchLod(scene, view);
                        const double selectionMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                        const auto frame = renderer.drawFrame(scene, view, 0, {});
                        const auto voxelCount = automatic ? expectedVoxels : 0;
                        const auto mixedCount = automatic ? expectedMixed : 0;
                        if (frame.camera.visibleInstances != count || frame.shadow.visibleInstances != count ||
                            frame.shadow.foliageTriangles != count * lod6Triangles ||
                            frame.camera.voxelBricks != (automatic ? expectedBricks : 0) ||
                            frame.camera.foliageTriangles != (automatic ? expectedTriangles : count * lod6Triangles) ||
                            !frame.gpuCameraMilliseconds || !frame.gpuShadowMilliseconds || !frame.gpuMilliseconds)
                            throw std::runtime_error("The mixed render changed caster triangles, omitted branches, or doubled submissions.");
                        if (rep < 2) continue;
                        timing << quotedAdapter(renderer.adapterName()) << ',' << angle << ',' << distance << ','
                               << (candidate == BranchVoxelView::Regional ? (mixed ? "regional_mixed" : "regional_far") :
                                                                          (mixed ? "mixed" : "far"))
                               << ',' << (automatic ? "automatic" : "triangles") << ',' << rep - 2
                               << ',' << count - voxelCount - mixedCount << ',' << voxelCount << ',' << mixedCount << ','
                               << (automatic ? expectedRegions : 0) << ',' << frame.camera.voxelBricks << ','
                               << frame.camera.foliageTriangles << ',' << frame.shadow.foliageTriangles << ',' << frame.camera.foliageCalls
                               << ',' << selectionMs << ',' << frame.cpuMilliseconds << ',' << *frame.gpuCameraMilliseconds << ','
                               << *frame.gpuShadowMilliseconds << ',' << *frame.gpuMilliseconds << '\n';
                    }
                }
            }
        }
    }
    return 0;
}

int runBranchRegionBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                             unsigned region, const std::filesystem::path& candidate)
{
    if (!scene.branchVoxelRegions || region >= voxelBranchRegionCount)
        throw std::runtime_error("The region benchmark needs a baked region and a valid index.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The region benchmark requires 4x MSAA.");
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    if (!candidate.empty())
    {
        const auto& original = (*scene.branchVoxelRegions)[region].foliage;
        auto foliage = readVoxelFoliage(candidate);
        if (foliage.sourceFingerprint != original.sourceFingerprint || foliage.meshIndex != original.meshIndex ||
            foliage.origin != original.origin || foliage.cellSize != original.cellSize ||
            foliage.bricks.empty() || foliage.bricks.size() > original.bricks.size())
            throw std::runtime_error("The pilot bake must match the source, grid, and region brick budget.");
        auto normalPath = candidate;
        normalPath.replace_extension(L".normals");
        auto normals = readVoxelNormals(normalPath, sourceFingerprint(candidate), occupiedVoxelCount(foliage));
        auto regions = std::make_shared<std::array<BranchVoxelRegion, voxelBranchRegionCount>>(*scene.branchVoxelRegions);
        (*regions)[region] = {std::move(foliage), std::move(normals)};
        scene.branchVoxelRegions = std::move(regions);
    }
    const auto variant = candidate.empty() ? "shared-mask" : "independent";
    const auto bit = static_cast<std::uint8_t>(1u << region);
    const auto& lod = *scene.fixedLod;
    const auto entire = lod.levels[fixedLodTwoTriangleLevel].indices.size() / 3;
    const auto first = voxelRegionGroupStart(region, lod.groups.size());
    const auto end = voxelRegionGroupStart(region + 1, lod.groups.size());
    const auto replaced = lod.indicesForGroups(fixedLodTwoTriangleLevel, first, end - 1).indexCount / 3;
    const auto bricks = (*scene.branchVoxelRegions)[region].foliage.bricks.size();
    scene.branchVoxelView = BranchVoxelView::Regional;
    scene.branchVoxelUseNormals = true;
    scene.branchVoxelDirectionalNormals = false;
    scene.branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
    scene.sunElevation = 54.5715f;
    scene.setBranchGrid(1);
    renderer.setScene(scene);
    renderer.updateCameraProjection(branchVoxelFarNearPlane, branchVoxelFarClipPlane);
    const auto viewport = renderer.viewportSize();
    auto& selection = scene.branchLodSelection;
    const auto capture = [&](std::uint8_t mask, BranchAppearance appearance, const XMFLOAT4X4& view) {
        if (!processMessages()) throw std::runtime_error("Region benchmark interrupted.");
        selection.voxelCut.front() = mask;
        scene.branchAppearance = appearance;
        std::vector<std::uint32_t> pixels;
        const auto frame = renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
        if (frame.shadow.foliageTriangles != entire ||
            (mask == bit && (frame.camera.voxelBricks != bricks ||
             frame.camera.foliageTriangles != entire - replaced + bricks * 12)))
            throw std::runtime_error("The region capture changed the expected geometry.");
        return pixels;
    };
    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "variant,region,orbit_seconds,sun_azimuth,switch,distance,neighbors,coverage_bias_pct,coverage_l1_pct,"
           "display_luminance_bias_pct,display_luminance_l1_pct,common_pixels\n" << std::fixed << std::setprecision(6);
    constexpr float angles[]{0, 5, 10, 157.0f / 12.0f, 196.0f / 12.0f, 22, 26};
    for (std::size_t pose = 0; pose < std::size(angles); ++pose)
    {
        const float angle = angles[pose];
        for (const bool retreat : {true, false})
        {
            const float from = retreat ? 650.0f : 810.0f;
            const float to = retreat ? 810.0f : 650.0f;
            const auto startView = scene.branchOrbitView(angle, from).matrix();
            renderer.updateBranchLod(scene, startView);
            bool crossed = false;
            for (unsigned step = 1; step <= 160; ++step)
            {
                const float distance = std::lerp(from, to, step / 160.0f);
                const auto view = scene.branchOrbitView(angle, distance).matrix();
                renderer.updateBranchLod(scene, view);
                const auto current = selection.voxelCut.front();
                if (((current & bit) != 0) != retreat) continue;
                const auto activeNeighbors = static_cast<std::uint8_t>(current & ~bit);
                for (const auto neighbors : {std::uint8_t{0}, activeNeighbors})
                {
                    if (neighbors == activeNeighbors && (neighbors | bit) == allVoxelRegions) continue;
                    const auto withPilot = static_cast<std::uint8_t>(neighbors | bit);
                    const auto beforeMask = capture(neighbors, BranchAppearance::CoverageMask, view);
                    const auto afterMask = capture(withPilot, BranchAppearance::CoverageMask, view);
                    for (const float azimuth : {-100.0f, 80.0f})
                    {
                        scene.sunAzimuth = azimuth;
                        renderer.updateSun(scene);
                        const auto before = capture(neighbors, BranchAppearance::Shaded, view);
                        const auto after = capture(withPilot, BranchAppearance::Shaded, view);
                        const auto metrics = compareTransitionFrames(before, after, beforeMask, afterMask);
                        csv << variant << ',' << region << ',' << angle << ',' << azimuth << ',' << (retreat ? "retreat" : "approach")
                            << ',' << distance << ',' << unsigned(neighbors) << ',' << metrics.coverageBias << ','
                            << metrics.coverageError << ',' << metrics.luminanceBias << ',' << metrics.luminanceError
                            << ',' << metrics.commonPixels << '\n';
                        if (!neighbors && retreat && azimuth == -100)
                        {
                            auto image = report;
                            image.replace_filename(report.stem().native() + L"-" + std::to_wstring(pose) + L".bmp");
                            writeTransitionPair(image, before, after, beforeMask, afterMask,
                                                static_cast<int>(viewport.x), static_cast<int>(viewport.y));
                        }
                    }
                    if (!activeNeighbors) break;
                }
                selection.voxelCut.front() = current;
                crossed = true;
                break;
            }
            if (!crossed) throw std::runtime_error("The pilot region did not reach its switch on the route.");
        }
    }
    csv.close();

    scene.sunAzimuth = -100;
    scene.setBranchGrid(branchGridSide);
    renderer.setScene(scene);
    renderer.updateCameraProjection(branchVoxelFarNearPlane, branchVoxelFarClipPlane);
    scene.branchAppearance = BranchAppearance::Shaded;
    const auto count = scene.instanceCount();
    auto gpuReport = report;
    gpuReport.replace_filename(report.stem().native() + L"-gpu.csv");
    std::ofstream gpu(gpuReport, std::ios::binary | std::ios::trunc);
    gpu.exceptions(std::ios::failbit | std::ios::badbit);
    gpu.imbue(std::locale::classic());
    gpu << "adapter,variant,orbit_seconds,mode,repetition,voxel_bricks,camera_foliage_triangles,gpu_camera_ms,gpu_frame_ms\n"
        << std::fixed << std::setprecision(6);
    for (const float angle : {angles[0], angles[3], angles[4]})
    {
        const auto view = scene.branchOrbitView(angle, 735).matrix();
        renderer.updateBranchLod(scene, view);
        for (unsigned rep = 0; rep < 11; ++rep)
        {
            for (unsigned pass = 0; pass < 2; ++pass)
            {
                if (!processMessages()) return 2;
                const bool pilot = (rep + pass) % 2 != 0;
                std::ranges::fill(selection.voxelCut, pilot ? bit : std::uint8_t{0});
                const auto frame = renderer.drawFrame(scene, view, 0, {});
                const auto expected = pilot ? entire - replaced + bricks * 12 : entire;
                if (frame.camera.visibleInstances != count || frame.shadow.foliageTriangles != entire * count ||
                    frame.camera.foliageTriangles != expected * count || frame.camera.voxelBricks != (pilot ? bricks * count : 0) ||
                    !frame.gpuCameraMilliseconds || !frame.gpuMilliseconds)
                    throw std::runtime_error("The pilot benchmark submitted an unexpected workload.");
                if (rep < 2) continue;
                gpu << quotedAdapter(renderer.adapterName()) << ',' << variant << ',' << angle << ','
                    << (pilot ? "mixed" : "triangles") << ',' << rep - 2 << ',' << frame.camera.voxelBricks << ','
                    << frame.camera.foliageTriangles << ',' << *frame.gpuCameraMilliseconds << ',' << *frame.gpuMilliseconds << '\n';
            }
        }
    }
    gpu.close();
    return 0;
}

int runOakWalkBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
        throw std::runtime_error("Build oak_crown_lod before running the Modeled Oak Walk benchmark.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The walk comparison requires 4x MSAA.");
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    const auto& lod = *scene.fixedLod;
    const auto sourceTris = scene.asset->meshes[lod.meshIndex].indices.size() / 3;
    const auto shadowTris = lod.levels[fixedLodFarLevel].indices.size() / 3;
    scene.branchShadowLod = static_cast<int>(fixedLodFarLevel);
    scene.branchGroupFrustumCulling = false;
    scene.windStrength = 0;
    renderer.setScene(scene);

    // Trace the eye-height paths against original triangles near that height.
    std::vector<TraceTriangle> eyeHeightTriangles;
    for (const auto& mesh : scene.asset->meshes)
    {
        const auto localToWorld = XMLoadFloat4x4(&mesh.localToWorld);
        std::vector<TracePoint> points;
        points.reserve(mesh.vertices.size());
        for (const auto& vertex : mesh.vertices)
            points.push_back(point(XMVector3TransformCoord(XMLoadFloat3(&vertex.position), localToWorld)));
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            const TraceTriangle triangle{points[mesh.indices[i]], points[mesh.indices[i + 1]], points[mesh.indices[i + 2]]};
            const auto [lo, hi] = std::minmax({triangle[0][1], triangle[1][1], triangle[2][1]});
            if (lo <= oakWalkEyeHeight && hi >= oakWalkEyeHeight) eyeHeightTriangles.push_back(triangle);
        }
    }
    const TriangleTrace clearance(std::move(eyeHeightTriangles));
    for (const auto route : {OakWalkRoute::Approach, OakWalkRoute::Circle})
    {
        auto previous = oakWalkView(route, 0.0f).eye();
        constexpr unsigned steps = 256;
        for (unsigned i = 1; i <= steps; ++i)
        {
            const float seconds = oakWalkDuration(route) * i / steps;
            const auto current = oakWalkView(route, seconds).eye();
            if (std::abs(current.y - oakWalkEyeHeight) > 0.00001f)
                throw std::runtime_error("The walk left the eye-height plane.");
            const TraceRay ray{{previous.x, previous.y, previous.z},
                               {current.x - previous.x, current.y - previous.y, current.z - previous.z}};
            if (std::isfinite(clearance.nearest(ray, {0.0, 1.0})))
                throw std::runtime_error("The eye-height path intersects modeled geometry at " +
                                         std::to_string(seconds) + " seconds.");
            previous = current;
        }
    }

    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "adapter,route,seconds,eye_x,eye_y,eye_z,trunk_range,trunk_ndc_x,mode,repetition,"
           "groups_lod0,groups_lod2,groups_lod3,groups_lod4,groups_lod6,camera_foliage_triangles,shadow_foliage_triangles,"
           "camera_foliage_calls,cpu_select_ms,cpu_draw_list_ms,cpu_render_ms,gpu_camera_ms,gpu_shadow_ms,gpu_frame_ms\n"
        << std::fixed << std::setprecision(6);
    const auto viewport = renderer.viewportSize();
    const auto projection = XMMatrixPerspectiveFovRH(D3D12Renderer::cameraVerticalFov, viewport.x / viewport.y,
                                                     scene.nearPlane, scene.farPlane());
    const auto adapter = quotedAdapter(renderer.adapterName());
    auto& selection = scene.branchLodSelection;
    const auto measure = [&](OakWalkRoute route, float seconds, bool picture) {
        const auto camera = oakWalkView(route, seconds);
        const auto eye = camera.eye();
        const auto view = camera.matrix();
        const auto trunk = XMVector3TransformCoord(XMVectorSet(0, oakWalkEyeHeight, 0, 1),
                                                   XMLoadFloat4x4(&view) * projection);
        const auto name = route == OakWalkRoute::Approach ? "walk" : "circle";
        std::array<unsigned, fixedLodLevelCount> counts{};
        std::size_t expected = 0;
        scene.branchLod = automaticBranchLod;
        renderer.updateBranchLod(scene, view);
        if (selection.groupLevels.size() != lod.groups.size())
            throw std::runtime_error("The tree cut omitted foliage groups.");
        for (std::size_t i = 0; i < lod.groups.size(); ++i)
        {
            const auto level = selection.groupLevels[i];
            if (level >= counts.size() || level == 1 || level == 5)
                throw std::runtime_error("The tree cut selected a level outside the automatic chain.");
            ++counts[level];
            expected += lod.indicesForGroups(level, i, i).indexCount / 3;
        }
        const auto cut = selection.groupLevels;
        selection.frozen = true;
        renderer.updateBranchLod(scene, oakWalkView(route, seconds == 0.0f ? oakWalkDuration(route) * 0.5f : 0.0f).matrix());
        if (selection.groupLevels != cut) throw std::runtime_error("Freeze cut changed the tree's representation.");
        selection.frozen = false;

        if (picture)
        {
            const auto snap = [&](int level) {
                scene.branchLod = level;
                std::vector<std::uint32_t> pixels;
                renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
                std::vector<std::uint32_t> preview;
                const auto width = static_cast<unsigned>(viewport.x);
                const auto height = static_cast<unsigned>(viewport.y);
                for (unsigned y = 0; y < height / 4; ++y)
                    for (unsigned x = 0; x < width / 4; ++x)
                        preview.push_back(pixels[(y * 4 + 1) * width + x * 4 + 1]);
                return preview;
            };
            const auto source = snap(sourceBranchLod);
            const auto automatic = snap(automaticBranchLod);
            auto image = report;
            image.replace_filename(report.stem().native() + L"-" +
                (route == OakWalkRoute::Approach ? std::wstring(L"walk") : std::wstring(L"circle")) + L"-" +
                std::to_wstring(static_cast<int>(seconds * 10)) + L".bmp");
            writeTransitionPair(image, source, automatic, {}, {}, static_cast<int>(viewport.x) / 4,
                                static_cast<int>(viewport.y) / 4);
        }
        for (unsigned rep = 0; rep < 7; ++rep)
        {
            for (unsigned pass = 0; pass < 2; ++pass)
            {
                if (!processMessages()) throw std::runtime_error("The oak walk benchmark was interrupted.");
                const bool automatic = (rep + pass) % 2 != 0;
                scene.branchLod = automatic ? automaticBranchLod : sourceBranchLod;
                const auto start = std::chrono::steady_clock::now();
                renderer.updateBranchLod(scene, view);
                const double selectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                const auto timing = renderer.drawFrame(scene, view, 0, {});
                if (timing.camera.visibleInstances != 1 || timing.shadow.visibleInstances != 1 ||
                    timing.camera.foliageTriangles != (automatic ? expected : sourceTris) ||
                    timing.shadow.foliageTriangles != shadowTris ||
                    !timing.gpuCameraMilliseconds || !timing.gpuShadowMilliseconds || !timing.gpuMilliseconds)
                    throw std::runtime_error("Oak walk submissions differed from their expected camera or shadow cut.");
                if (rep < 2) continue;
                csv << adapter << ',' << name << ',' << seconds << ',' << eye.x << ',' << eye.y << ',' << eye.z << ','
                    << std::hypot(eye.x, eye.z) << ',' << XMVectorGetX(trunk) << ',' << (automatic ? "automatic" : "source")
                    << ',' << rep - 2;
                for (const auto level : {0u, 2u, 3u, 4u, 6u}) csv << ',' << (automatic ? counts[level] : 0u);
                csv << ',' << timing.camera.foliageTriangles << ',' << timing.shadow.foliageTriangles << ','
                    << timing.camera.foliageCalls << ',' << selectMs << ',' << timing.cpuBranchDrawListMilliseconds << ','
                    << timing.cpuMilliseconds << ',' << *timing.gpuCameraMilliseconds << ',' << *timing.gpuShadowMilliseconds
                    << ',' << *timing.gpuMilliseconds << '\n';
            }
        }
    };
    for (const float seconds : {0.0f, 8.0f, 16.0f, 21.5f, 25.0f, 29.5f, 32.5f, 35.0f})
        measure(OakWalkRoute::Approach, seconds, seconds == 0.0f || seconds == 21.5f || seconds == 29.5f || seconds == 35.0f);
    for (unsigned i = 0; i < 8; ++i)
        measure(OakWalkRoute::Circle, oakWalkDuration(OakWalkRoute::Circle) * i / 8, i == 2 || i == 6);
    csv.close();
    return 0;
}

int runOakWalkCullBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
        throw std::runtime_error("Build oak_crown_lod before running the group culling benchmark.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The group culling comparison requires 4x MSAA.");
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    scene.branchFrustumCulling = true;
    scene.windStrength = 0;
    renderer.setScene(scene);
    auto& selection = scene.branchLodSelection;
    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    const auto adapter = quotedAdapter(renderer.adapterName());
    csv << "adapter,route,seconds,mode,repetition,groups_considered,groups_visible,camera_foliage_triangles,"
           "shadow_foliage_triangles,camera_foliage_draws,camera_foliage_calls,cpu_draw_list_ms,cpu_render_ms,"
           "gpu_camera_ms,gpu_shadow_ms,gpu_frame_ms\n" << std::fixed << std::setprecision(6);
    unsigned pairedImages = 0;
    const auto verify = [&](const XMFLOAT4X4& view, float phase) {
        if (!processMessages()) throw std::runtime_error("Group culling capture was interrupted.");
        std::vector<std::uint32_t> before, after;
        scene.branchGroupFrustumCulling = false;
        const auto baseline = renderer.drawFrame(scene, view, phase, {}, {}, &before);
        scene.branchGroupFrustumCulling = true;
        const auto culled = renderer.drawFrame(scene, view, phase, {}, {}, &after);
        if (baseline.shadow.foliageTriangles != culled.shadow.foliageTriangles ||
            baseline.shadow.foliageDraws != culled.shadow.foliageDraws ||
            baseline.camera.foliageGroupsConsidered != culled.camera.foliageGroupsConsidered ||
            culled.camera.foliageGroupsVisible > baseline.camera.foliageGroupsVisible ||
            culled.camera.foliageTriangles > baseline.camera.foliageTriangles || before != after)
            throw std::runtime_error("Group frustum culling changed the image, shadow cut, or draw count unexpectedly.");
        ++pairedImages;
        return std::pair{baseline, culled};
    };
    const auto measure = [&](OakWalkRoute route, float seconds) {
        const auto view = oakWalkView(route, seconds).matrix();
        scene.branchLod = automaticBranchLod;
        selection.frozen = false;
        renderer.updateBranchLod(scene, view);
        selection.frozen = true;
        const auto [baseline, culled] = verify(view, 0.0f);
        if (route == OakWalkRoute::Approach && seconds == oakWalkDuration(route) &&
            (culled.camera.foliageGroupsVisible == baseline.camera.foliageGroupsVisible || !culled.camera.foliageGroupsVisible))
            throw std::runtime_error("The near walk did not exercise a partial foliage cut.");
        for (unsigned rep = 0; rep < 11; ++rep)
        {
            for (unsigned pass = 0; pass < 2; ++pass)
            {
                if (!processMessages()) throw std::runtime_error("Group culling benchmark was interrupted.");
                const bool enabled = (rep + pass) % 2 != 0;
                scene.branchGroupFrustumCulling = enabled;
                const auto frame = renderer.drawFrame(scene, view, 0, {});
                const auto& expected = enabled ? culled : baseline;
                if (frame.camera.foliageGroupsVisible != expected.camera.foliageGroupsVisible ||
                    frame.camera.foliageTriangles != expected.camera.foliageTriangles ||
                    frame.shadow.foliageTriangles != expected.shadow.foliageTriangles ||
                    !frame.gpuCameraMilliseconds || !frame.gpuShadowMilliseconds || !frame.gpuMilliseconds)
                    throw std::runtime_error("Group culling timed a different cut than the captures.");
                if (rep < 2) continue;
                csv << adapter << ',' << (route == OakWalkRoute::Approach ? "walk" : "circle") << ',' << seconds
                    << ',' << (enabled ? "groups" : "whole_tree") << ',' << rep - 2 << ','
                    << frame.camera.foliageGroupsConsidered << ',' << frame.camera.foliageGroupsVisible << ','
                    << frame.camera.foliageTriangles << ',' << frame.shadow.foliageTriangles << ','
                    << frame.camera.foliageDraws << ',' << frame.camera.foliageCalls << ','
                    << frame.cpuBranchDrawListMilliseconds << ',' << frame.cpuMilliseconds << ','
                    << *frame.gpuCameraMilliseconds << ',' << *frame.gpuShadowMilliseconds << ','
                    << *frame.gpuMilliseconds << '\n';
            }
        }
        selection.frozen = false;
    };
    for (const float seconds : {0.0f, 8.0f, 16.0f, 21.5f, 25.0f, 29.5f, 32.5f, 35.0f})
        measure(OakWalkRoute::Approach, seconds);
    for (unsigned i = 0; i < 8; ++i)
        measure(OakWalkRoute::Circle, oakWalkDuration(OakWalkRoute::Circle) * i / 8);
    csv.close();

    scene.windStrength = 2;
    for (const bool msaa : {false, true})
    {
        if (msaa != renderer.msaaEnabled()) renderer.toggleMsaa();
        for (const int level : {automaticBranchLod, 0, static_cast<int>(fixedLodTwoTriangleLevel)})
        {
            scene.branchLod = level;
            scene.branchShadowLod = level == automaticBranchLod ? matchCameraShadowLod : static_cast<int>(fixedLodFarLevel);
            for (const float seconds : {21.5f, 29.5f})
            {
                const auto view = oakWalkView(OakWalkRoute::Approach, seconds).matrix();
                renderer.updateBranchLod(scene, view);
                // Exercise both list submission paths and opposing wind offsets.
                for (const bool indirect : {false, true})
                {
                    selection.indirectDraws = indirect;
                    verify(view, indirect ? 0.83f : 4.24f);
                }
            }
        }
    }
    auto validationPath = report;
    validationPath.replace_filename(report.stem().native() + L"-parity.txt");
    std::ofstream validation(validationPath);
    validation.exceptions(std::ios::failbit | std::ios::badbit);
    validation << pairedImages << " paired captures matched pixel for pixel with the same shadow submissions.\n";
    validation.close();
    return 0;
}

int runSunCascadeBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                           bool stabilized)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
        throw std::runtime_error("Build oak_crown_lod before running the sun cascade benchmark.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The sun cascade benchmark requires 4x MSAA.");
    scene.stabilizeSunCascades = stabilized;
    scene.blendSunCascades = false;
    scene.filterSunCascades = false;
    scene.cullSunCascadeCasters = scene.coarsenSunCascadeFoliage = false;
    scene.windStrength = 0;
    renderer.setScene(scene);
    const auto viewport = renderer.viewportSize();
    const auto width = static_cast<unsigned>(viewport.x);
    const auto height = static_cast<unsigned>(viewport.y);
    const auto cameraProjection = XMMatrixPerspectiveFovRH(D3D12Renderer::cameraVerticalFov, viewport.x / viewport.y,
                                                           scene.nearPlane, scene.farPlane());
    const auto viewAt = [](float range) {
        return CameraView{{0.0f, oakWalkEyeHeight, 0.0f}, 0.0f, 0.0f, range}.matrix();
    };
    constexpr auto count = sunCascadeSplits.size();
    constexpr std::array<std::uint32_t, count + 1> colors{0x4851e7u, 0x59b941u, 0xef8540u, 0x68605cu};
    using Pixels = std::vector<std::uint32_t>;
    const auto capture = [&](const XMFLOAT4X4& view, Pixels& pixels) {
        if (!processMessages()) throw std::runtime_error("Sun cascade capture was interrupted.");
        renderer.updateBranchLod(scene, view);
        return renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
    };
    const auto preview = [&](const Pixels& pixels) {
        Pixels downsampled;
        downsampled.reserve((width / 4) * (height / 4));
        for (unsigned y = 0; y < height / 4; ++y)
            for (unsigned x = 0; x < width / 4; ++x)
                downsampled.push_back(pixels[(4 * y + 1) * width + 4 * x + 1]);
        return downsampled;
    };
    const auto savePair = [&](const wchar_t* suffix, const Pixels& a, const Pixels& b) {
        auto path = report;
        path.replace_filename(report.stem().native() + suffix);
        writeTransitionPair(path, preview(a), preview(b), {}, {}, static_cast<int>(width / 4), static_cast<int>(height / 4));
    };
    const auto foliageTris = scene.fixedLod->levels[fixedLodFarLevel].indices.size() / 3;
    const auto originalFlags = scene.meshShadows;
    const auto disableCasters = [&] {
        for (auto& mesh : scene.meshShadows) mesh.cast = false;
    };
    const auto darkerGround = [](std::uint32_t shadow, std::uint32_t unshadowed) {
        return (shadow >> 24) == 64 && (unshadowed >> 24) == 64 && (shadow & 255) + 31 < (unshadowed & 255);
    };

    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "test,msaa,adapter,mode,camera_trees,shadow_trees,shadow_foliage_triangles,ground_shadow_pixels,"
           "gpu_shadow_ms,gpu_frame_ms\n" << std::fixed << std::setprecision(6);
    const auto row = [&](const char* test, const D3D12Renderer::FrameTiming& frame, std::size_t shadowPixels = 0) {
        if (!frame.gpuShadowMilliseconds || !frame.gpuMilliseconds)
            throw std::runtime_error("Sun cascade benchmark GPU timestamps are unavailable.");
        csv << test << ',' << (renderer.msaaEnabled() ? 4 : 1) << ',' << quotedAdapter(renderer.adapterName()) << ','
            << (scene.useSunCascades ? "cascades" : "single") << ',' << frame.camera.visibleInstances << ','
            << frame.shadow.visibleInstances << ',' << frame.shadow.foliageTriangles << ',' << shadowPixels << ','
            << *frame.gpuShadowMilliseconds << ',' << *frame.gpuMilliseconds << '\n';
    };

    for (const bool msaa : {false, true})
    {
        if (msaa != renderer.msaaEnabled()) renderer.toggleMsaa();
        scene.useSunCascades = true;
        scene.showCascadeColors = true;
        const auto view = viewAt(75);
        const auto frame = [&] {
            Pixels debug;
            const auto timing = capture(view, debug);
            constexpr float testDepths[]{14, 32, 80, 125};
            for (std::size_t i = 0; i < std::size(testDepths); ++i)
            {
                const float depth = testDepths[i];
                const float x = 0.45f * depth / XMVectorGetX(cameraProjection.r[0]);
                const auto point = XMVector3Project(XMVectorSet(x, 0, 75 - depth, 1),
                    0, 0, viewport.x, viewport.y, 0, 1, cameraProjection, XMLoadFloat4x4(&view), XMMatrixIdentity());
                const auto px = static_cast<unsigned>(XMVectorGetX(point));
                const auto py = static_cast<unsigned>(XMVectorGetY(point));
                if (px >= width || py >= height || (debug[py * width + px] & 0xffffff) != colors[i])
                    throw std::runtime_error("The ground probe selected the wrong sun cascade.");
            }
            if (msaa)
            {
                Pixels shaded;
                scene.showCascadeColors = false;
                capture(view, shaded);
                savePair(L"-colors.bmp", shaded, debug);
            }
            return timing;
        }();
        row("split_colors", frame);
        scene.showCascadeColors = false;
    }

    scene.showShadowFactor = true;
    constexpr float ranges[]{8, 32, 64};
    constexpr const char* sliceNames[]{"near_ground", "middle_ground", "far_ground"};
    for (std::size_t slice = 0; slice < std::size(ranges); ++slice)
    {
        const auto view = viewAt(ranges[slice]);
        Pixels bands, caster, clear;
        scene.showCascadeColors = true;
        capture(view, bands);
        scene.showCascadeColors = false;
        const auto frame = capture(view, caster);
        disableCasters();
        capture(view, clear);
        scene.meshShadows = originalFlags;
        std::size_t shadowPixels = 0;
        for (std::size_t i = 0; i < bands.size(); ++i)
            if ((bands[i] & 0xffffff) == colors[slice] && darkerGround(caster[i], clear[i])) ++shadowPixels;
        if (!shadowPixels || frame.shadow.foliageTriangles != count * foliageTris)
            throw std::runtime_error("A sun cascade did not cast a measurable shadow on the ground.");
        row(sliceNames[slice], frame, shadowPixels);
    }

    scene.showShadowFactor = false;
    for (const auto seconds : {0.0f, 21.5f, 35.0f})
    {
        const auto view = oakWalkView(OakWalkRoute::Approach, seconds).matrix();
        Pixels singleBefore, cascaded, singleAfter;
        scene.useSunCascades = false;
        const auto single = capture(view, singleBefore);
        scene.useSunCascades = true;
        const auto three = capture(view, cascaded);
        scene.useSunCascades = false;
        capture(view, singleAfter);
        if (singleBefore != singleAfter || single.camera.foliageTriangles != three.camera.foliageTriangles ||
            single.shadow.foliageTriangles != foliageTris || three.shadow.foliageTriangles != count * foliageTris)
            throw std::runtime_error("Changing sun shadow modes altered the single-map control or caster workload.");
        if (seconds == 21.5f) savePair(L"-walk.bmp", singleBefore, cascaded);
        constexpr unsigned samples = 9;
        for (unsigned rep = 0; rep < samples + 2; ++rep)
        {
            for (unsigned pass = 0; pass < 2; ++pass)
            {
                if (!processMessages()) return 2;
                scene.useSunCascades = (rep + pass) % 2 != 0;
                const auto frame = renderer.drawFrame(scene, view, 0, {});
                if (rep < 2) continue;
                row(seconds == 0.0f ? "walk_start" : (seconds == 35.0f ? "walk_end" : "walk_side"), frame);
            }
        }
    }

    // Put the entire tree outside the camera, on the sunward side of the path.
    scene.objects.front().placement.x = 33.0f;
    scene.sunAzimuth = 90.0f;
    scene.sunElevation = 25.0f;
    scene.useSunCascades = true;
    scene.showShadowFactor = true;
    renderer.setScene(scene);
    const auto offView = viewAt(20);
    Pixels shaded, unshadowed;
    const auto offscreen = capture(offView, shaded);
    disableCasters();
    capture(offView, unshadowed);
    std::size_t offscreenPixels = 0;
    for (std::size_t i = 0; i < shaded.size(); ++i)
        if (darkerGround(shaded[i], unshadowed[i])) ++offscreenPixels;
    if (offscreen.camera.visibleInstances != 0 || offscreen.shadow.visibleInstances != 1 ||
        offscreen.shadow.foliageTriangles != count * foliageTris || !offscreenPixels)
        throw std::runtime_error("An off-camera tree did not cast a shadow onto the visible ground.");
    row("off_camera_caster", offscreen, offscreenPixels);
    savePair(L"-off-camera.bmp", unshadowed, shaded);
    csv.close();
    return 0;
}

int runSunCascadeStabilityBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
        throw std::runtime_error("Build oak_crown_lod before running the cascade stability benchmark.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The cascade stability benchmark requires 4x MSAA.");
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    scene.useSunCascades = true;
    scene.blendSunCascades = false;
    scene.filterSunCascades = false;
    scene.cullSunCascadeCasters = scene.coarsenSunCascadeFoliage = false;
    scene.showShadowFactor = true;
    scene.windStrength = 0.0f;
    renderer.setScene(scene);
    const auto viewport = renderer.viewportSize();
    const auto width = static_cast<unsigned>(viewport.x);
    const auto height = static_cast<unsigned>(viewport.y);
    const auto foliageTris = scene.fixedLod->levels[fixedLodFarLevel].indices.size() / 3 * sunCascadeSplits.size();
    const auto viewAt = [](float eyeZ, float angle) {
        return CameraView{{-std::sin(angle), oakWalkEyeHeight, eyeZ - std::cos(angle)}, angle, 0.0f, 1.0f}.matrix();
    };
    struct Probe { const char* name; float eyeZ, minDepth, maxDepth; };
    constexpr Probe probes[]{{"near", 8, 5, 13}, {"middle", 32, 23, 40}, {"far", 64, 54, 76}};

    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "cascade,motion,fit,frame,ground_pixels,changed_pixels,changed_ge_8,abs_error_sum,"
           "near_texel,mid_texel,far_texel,gpu_shadow_ms\n" << std::fixed << std::setprecision(8);
    for (const auto& probe : probes)
    {
        const auto observation = viewAt(probe.eyeZ, 0.0f);
        renderer.updateBranchLod(scene, observation);
        std::vector<std::size_t> groundRegion;
        const double heightScale = oakWalkEyeHeight / std::tan(D3D12Renderer::cameraVerticalFov * 0.5);
        for (unsigned y = height / 2; y < height; ++y)
        {
            const double depth = heightScale / (2.0 * (y + 0.5) / height - 1.0);
            if (depth < probe.minDepth || depth > probe.maxDepth) continue;
            for (unsigned x = width * 34 / 100; x < width * 66 / 100; ++x)
                groundRegion.push_back(static_cast<std::size_t>(y) * width + x);
        }
        for (const bool rotate : {true, false})
        {
            std::array<std::uint64_t, 2> errors{};
            for (const bool stabilized : {false, true})
            {
                scene.stabilizeSunCascades = stabilized;
                std::vector<std::uint32_t> previous, pixels;
                std::array<float, sunCascadeSplits.size()> initialWidths{};
                constexpr unsigned frames = 25;
                for (unsigned i = 0; i < frames; ++i)
                {
                    if (!processMessages()) return 2;
                    const auto fraction = static_cast<float>(i) / (frames - 1);
                    const auto fit = viewAt(probe.eyeZ - (rotate ? 0.0f : 0.60f * fraction),
                                            rotate ? XMConvertToRadians(std::lerp(-12.0f, 12.0f, fraction)) : 0.0f);
                    // A fixed observer samples the same world points while only the cascade fit moves.
                    const auto frame = renderer.drawFrame(scene, observation, 0.0f, {}, {}, &pixels, &fit);
                    if (frame.shadow.foliageTriangles != foliageTris || !frame.gpuShadowMilliseconds)
                        throw std::runtime_error("The sun stability benchmark changed the caster workload.");
                    const auto& widths = renderer.sunCascadeTexelWidths();
                    if (i == 0) initialWidths = widths;
                    if (stabilized && widths != initialWidths)
                        throw std::runtime_error("A stabilized cascade changed its texel footprint during motion.");
                    std::uint64_t shared = 0, changed = 0, substantial = 0, error = 0;
                    if (!previous.empty())
                    {
                        for (const auto pixel : groundRegion)
                        {
                            const auto a = previous[pixel], b = pixels[pixel];
                            if ((a >> 24) != 64 || (b >> 24) != 64) continue;
                            ++shared;
                            const auto delta = std::abs(static_cast<int>(a & 255) - static_cast<int>(b & 255));
                            changed += delta != 0;
                            substantial += delta >= 8;
                            error += delta;
                        }
                        if (!shared) throw std::runtime_error("The cascade stability benchmark lost its ground probe.");
                        errors[stabilized] += error;
                        csv << probe.name << ',' << (rotate ? "turn" : "walk") << ',' << (stabilized ? "stable" : "tight")
                            << ',' << i << ',' << shared << ',' << changed << ',' << substantial << ',' << error << ','
                            << widths[0] << ',' << widths[1] << ',' << widths[2] << ',' << *frame.gpuShadowMilliseconds << '\n';
                    }
                    previous.swap(pixels);
                }
            }
            if (!errors[0] || errors[1] >= errors[0])
                throw std::runtime_error("Stable cascades did not reduce temporal shadow error in a ground probe.");
        }
    }
    csv.close();
    return 0;
}

int runSunCascadeTransitionBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
        throw std::runtime_error("Build oak_crown_lod before running the cascade transition benchmark.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The cascade transition benchmark requires 4x MSAA.");
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    const auto& foliage = scene.asset->meshes[scene.fixedLod->meshIndex];
    const auto& leaf = scene.asset->materials[foliage.material];
    if (!leaf.colorTexture || !(foliage.uvMask & (1u << leaf.textureCoordinateSet)))
        throw std::runtime_error("The modeled oak leaf material is missing its textured UV set.");
    const auto& leafImage = scene.asset->textures[*leaf.colorTexture].mips.front();
    if (leafImage.width != 512 || leafImage.height != 512)
        throw std::runtime_error("The modeled oak leaf material is missing the 512x512 base color bake.");

    scene.useSunCascades = true;
    scene.windStrength = 0;
    scene.showShadowFactor = true;
    scene.cullSunCascadeCasters = scene.coarsenSunCascadeFoliage = false;
    renderer.setScene(scene);
    const auto viewport = renderer.viewportSize();
    const auto width = static_cast<unsigned>(viewport.x);
    const auto height = static_cast<unsigned>(viewport.y);
    const float focalY = 0.5f * height / std::tan(D3D12Renderer::cameraVerticalFov * 0.5f);
    const auto viewAt = [](float range) {
        return CameraView{{0, oakWalkEyeHeight, 0}, 0, 0, range}.matrix();
    };
    using Pixels = std::vector<std::uint32_t>;
    const auto capture = [&](const XMFLOAT4X4& view, Pixels& pixels) {
        if (!processMessages()) throw std::runtime_error("Sun cascade capture was interrupted.");
        renderer.updateBranchLod(scene, view);
        return renderer.drawFrame(scene, view, 0, {}, {}, &pixels);
    };
    const auto savePair = [&](const wchar_t* suffix, const Pixels& left, const Pixels& right) {
        auto path = report;
        path.replace_filename(report.stem().native() + suffix);
        writeTransitionPair(path, left, right, {}, {}, static_cast<int>(width), static_cast<int>(height));
    };
    const auto isGround = [](std::uint32_t pixel) { return (pixel >> 24) == 64; };
    const auto visibility = [](std::uint32_t pixel) { return static_cast<int>(pixel & 255); };
    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "test,configuration,receiver_pixels,changed_pixels,mean_code_delta,gpu_camera_ms,gpu_shadow_ms\n"
        << std::fixed << std::setprecision(6);
    const auto row = [&](const char* test, const char* setup, std::size_t pixels, std::size_t changed,
                         std::uint64_t error, const D3D12Renderer::FrameTiming& timing) {
        if (!pixels || !timing.gpuCameraMilliseconds || !timing.gpuShadowMilliseconds)
            throw std::runtime_error("A transition probe lost its samples or GPU timings.");
        csv << test << ',' << setup << ',' << pixels << ',' << changed << ',' << double(error) / pixels << ','
            << *timing.gpuCameraMilliseconds << ',' << *timing.gpuShadowMilliseconds << '\n';
    };

    // Use level views so ground depth can be reconstructed from each screen row.
    constexpr const char* names[]{"near_to_middle", "middle_to_far", "range_fade"};
    for (std::size_t i = 0; i < sunCascadeSplits.size(); ++i)
    {
        const float end = sunCascadeSplits[i];
        const float start = end - sunCascadeBlendWidths[i];
        const auto view = viewAt(0.5f * (start + end));
        Pixels hard, blended, narrow;
        scene.blendSunCascades = false;
        const auto hardTiming = capture(view, hard);
        scene.blendSunCascades = true;
        const auto wideTiming = capture(view, blended);
        scene.filterSunCascades = false;
        const auto narrowTiming = capture(view, narrow);
        scene.filterSunCascades = true;
        std::size_t pixels = 0, blendChanges = 0, filterChanges = 0;
        std::uint64_t blendError = 0, filterError = 0;
        for (unsigned y = height / 2 + 1; y < height; ++y)
        {
            // Keep all subsamples in the same transition region at 4x MSAA.
            const auto closestDepth = focalY * oakWalkEyeHeight / (y + 1.0f - height * 0.5f);
            const auto farthestDepth = focalY * oakWalkEyeHeight / (y - height * 0.5f);
            if (closestDepth <= start || farthestDepth >= end) continue;
            for (unsigned x = 0; x < width; ++x)
            {
                const auto p = std::size_t(y) * width + x;
                if (!isGround(hard[p]) || !isGround(blended[p]) || !isGround(narrow[p])) continue;
                ++pixels;
                const auto blendDelta = visibility(blended[p]) - visibility(hard[p]);
                if (i + 1 == sunCascadeSplits.size() && blendDelta < 0)
                    throw std::runtime_error("The last cascade became darker while fading to full light.");
                blendChanges += blendDelta != 0;
                blendError += std::abs(blendDelta);
                const auto filterDelta = visibility(blended[p]) - visibility(narrow[p]);
                filterChanges += filterDelta != 0;
                filterError += std::abs(filterDelta);
            }
        }
        if (!blendChanges || !filterChanges)
            throw std::runtime_error("A cascade transition probe did not capture blending or filtering.");
        row(names[i], "blend_vs_hard", pixels, blendChanges, blendError, wideTiming);
        row(names[i], "wide_vs_narrow", pixels, filterChanges, filterError, narrowTiming);
        const auto boundaryY = static_cast<unsigned>(std::round(height * 0.5f + focalY * oakWalkEyeHeight / end - 1.0f));
        for (const bool blend : {false, true})
        {
            const auto& image = blend ? blended : hard;
            std::size_t shared = 0, changed = 0;
            std::uint64_t error = 0;
            for (unsigned x = 0; x < width; ++x)
            {
                const auto p = std::size_t(boundaryY) * width + x;
                if (!isGround(image[p]) || !isGround(image[p + width])) continue;
                ++shared;
                const auto delta = std::abs(visibility(image[p]) - visibility(image[p + width]));
                changed += delta != 0;
                error += delta;
            }
            row(names[i], blend ? "blended_edge" : "hard_edge", shared, changed, error, blend ? wideTiming : hardTiming);
        }
        if (!i) savePair(L"-near-shadow.bmp", hard, blended);
    }

    // Toggle ground casting with the oak casting disabled to isolate ground acne.
    // Then restore the oak and measure lost occlusion in a patch around its base.
    const auto casters = scene.meshShadows;
    const auto biasView = viewAt(8);
    const auto defaultBias = scene.shadowBias;
    for (const auto elevation : {54.5715f, 25.0f})
    {
        scene.sunElevation = elevation;
        renderer.setScene(scene);
        std::uint64_t zeroBiasContact = 0;
        for (const float bias : {0.0f, 0.25f, 0.5f, 1.0f})
        {
            scene.shadowBias.samplingTexels = bias;
            const auto setup = std::to_string(elevation) + "deg_" + std::to_string(bias) + "texel";
            for (auto& mesh : scene.meshShadows) mesh.cast = false;
            scene.groundShadows.cast = true;
            Pixels groundOnly, treeAndGround;
            const auto timing = capture(biasView, groundOnly);
            scene.meshShadows = casters;
            capture(biasView, treeAndGround);
            std::size_t ground = 0, acne = 0, contactGround = 0, contactShadow = 0;
            std::uint64_t acneError = 0, contactError = 0;
            for (unsigned y = height / 2 + 1; y < height; ++y)
            {
                const float depth = focalY * oakWalkEyeHeight / (y + 0.5f - height * 0.5f);
                for (unsigned x = 0; x < width; ++x)
                {
                    const auto p = std::size_t(y) * width + x;
                    if (!isGround(groundOnly[p])) continue;
                    ++ground;
                    const auto lostLight = 255 - visibility(groundOnly[p]);
                    acne += lostLight > 0;
                    acneError += lostLight;
                    const float worldX = (x + 0.5f - 0.5f * width) * depth / focalY;
                    if (std::abs(depth - 8.0f) > 1.5f || std::abs(worldX) > 1.5f) continue;
                    ++contactGround;
                    const auto treeShadow = std::max(0, visibility(groundOnly[p]) - visibility(treeAndGround[p]));
                    contactShadow += treeShadow > 0;
                    contactError += treeShadow;
                }
            }
            row("ground_acne", setup.c_str(), ground, acne, acneError, timing);
            row("trunk_contact", setup.c_str(), contactGround, contactShadow, contactError, timing);
            if (bias == 0.0f) zeroBiasContact = contactError;
            if (bias == defaultBias.samplingTexels &&
                (acneError > ground / 255 || !zeroBiasContact || contactError < 0.95 * zeroBiasContact))
                throw std::runtime_error("Default cascade bias left ground acne or lost trunk contact shadow.");
        }
    }
    scene.shadowBias = defaultBias;
    scene.groundShadows.cast = false;
    scene.sunElevation = 54.5715f;
    scene.showShadowFactor = false;
    renderer.setScene(scene);
    const auto walk = oakWalkView(OakWalkRoute::Approach, 21.5f).matrix();
    Pixels hard, blended;
    scene.blendSunCascades = scene.filterSunCascades = false;
    capture(walk, hard);
    scene.blendSunCascades = scene.filterSunCascades = true;
    capture(walk, blended);
    savePair(L"-walk.bmp", hard, blended);
    constexpr std::size_t repeats = 9;
    std::array<std::array<double, repeats>, 2> cameraTimes{}, shadowTimes{};
    for (std::size_t i = 0; i < repeats + 2; ++i)
    {
        for (std::size_t pass = 0; pass < 2; ++pass)
        {
            const auto mode = (i + pass) % 2;
            scene.blendSunCascades = scene.filterSunCascades = mode != 0;
            const auto timing = renderer.drawFrame(scene, walk, 0, {});
            if (!timing.gpuCameraMilliseconds || !timing.gpuShadowMilliseconds)
                throw std::runtime_error("A transition timing sample is missing GPU timestamps.");
            if (i < 2) continue;
            cameraTimes[mode][i - 2] = *timing.gpuCameraMilliseconds;
            shadowTimes[mode][i - 2] = *timing.gpuShadowMilliseconds;
        }
    }
    for (std::size_t i = 0; i < cameraTimes.size(); ++i)
    {
        std::ranges::sort(cameraTimes[i]);
        std::ranges::sort(shadowTimes[i]);
        csv << "walk_median," << (i ? "blended_wide" : "hard_narrow") << ",,,,"
            << cameraTimes[i][repeats / 2] << ',' << shadowTimes[i][repeats / 2] << '\n';
    }
    scene.blendSunCascades = scene.filterSunCascades = true;
    const auto close = scene.cameraView(CameraPreset::CloseUp).matrix();
    capture(close, blended);
    scene.branchLod = sourceBranchLod;
    capture(close, hard);
    savePair(L"-textured-crown.bmp", hard, blended);

    scene.branchLod = automaticBranchLod;
    scene.objects.front().placement.x = 33.0f;
    scene.sunAzimuth = 90.0f;
    scene.sunElevation = 25.0f;
    scene.showShadowFactor = true;
    renderer.setScene(scene);
    const auto offCamera = viewAt(20);
    const auto timing = capture(offCamera, blended);
    for (auto& mesh : scene.meshShadows) mesh.cast = false;
    capture(offCamera, hard);
    std::size_t ground = 0, shadow = 0;
    std::uint64_t error = 0;
    for (std::size_t p = 0; p < hard.size(); ++p)
    {
        if (!isGround(hard[p]) || !isGround(blended[p])) continue;
        ++ground;
        const auto delta = std::max(0, visibility(hard[p]) - visibility(blended[p]));
        shadow += delta > 31;
        error += delta;
    }
    if (timing.camera.visibleInstances || timing.shadow.visibleInstances != 1 || !shadow)
        throw std::runtime_error("The blended cascades lost the off-camera caster's shadow.");
    row("off_camera_caster", "blended_wide", ground, shadow, error, timing);
    csv.close();
    return 0;
}

int runSunCascadeCasterBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
        throw std::runtime_error("Build oak_crown_lod before running the cascade caster benchmark.");
    if (!renderer.supportsMsaa()) throw std::runtime_error("The cascade caster benchmark requires 4x MSAA.");
    scene.useSunCascades = true;
    scene.showShadowFactor = true;
    const auto setMode = [&](unsigned mode) {
        scene.cullSunCascadeCasters = mode >= 1;
        scene.coarsenSunCascadeFoliage = mode >= 2;
    };
    using Pixels = std::vector<std::uint32_t>;
    using Timing = D3D12Renderer::FrameTiming;
    struct ReceiverDelta
    {
        std::size_t count = 0, changed = 0, substantial = 0;
        std::uint64_t error = 0;
    };
    std::ofstream csv(report, std::ios::binary | std::ios::trunc);
    csv.exceptions(std::ios::failbit | std::ios::badbit);
    csv.imbue(std::locale::classic());
    csv << "pose,mode,adapter,width,height,msaa,wind,camera_trees,shadow_trees,shadow_triangles,shadow_foliage_triangles,shadow_draws,shadow_foliage_calls,"
           "near_trees,middle_trees,far_trees,near_foliage,middle_foliage,far_foliage,near_groups,middle_groups,far_groups,"
           "near_lod6,middle_lod6,far_lod6,ground_samples,ground_changed,ground_ge8,ground_mae,tree_samples,tree_changed,tree_ge8,"
           "tree_mae,gpu_shadow_ms,cpu_ms,cpu_drawlist_ms\n" << std::fixed << std::setprecision(6);
    bool culledTree = false, culledGroup = false, mixedInstances = false;
    const auto viewport = renderer.viewportSize();
    const auto adapter = quotedAdapter(renderer.adapterName());
    const auto compare = [&](const char* pose, const XMFLOAT4X4& view, float phase, bool measure, bool offCamera = false,
                             unsigned modes = 3) {
        renderer.updateBranchLod(scene, view);
        std::array<Timing, 3> timings{};
        Pixels baseline, pixels;
        std::array<std::array<ReceiverDelta, 2>, 3> quality{};
        for (unsigned mode = 0; mode < modes; ++mode)
        {
            if (!processMessages()) throw std::runtime_error("Cascade caster capture was interrupted.");
            setMode(mode);
            auto& frame = timings[mode];
            frame = renderer.drawFrame(scene, view, phase, {}, {}, mode ? &pixels : &baseline);
            if (mode == 1 && pixels != baseline)
                throw std::runtime_error(std::string("Caster culling changed the shadow image at ") + pose + '.');
            if (mode && frame.camera.foliageTriangles != timings[0].camera.foliageTriangles)
                throw std::runtime_error("Cascade caster selection changed the camera cut.");
            if (offCamera && (frame.camera.visibleInstances != 0 || frame.shadow.visibleInstances != 1))
                throw std::runtime_error("The low-sun probe lost its off-camera caster.");
            for (const auto& slice : frame.shadowCascades)
            {
                culledTree |= slice.visibleInstances < scene.instanceCount();
                culledGroup |= slice.foliageGroupsVisible < slice.foliageGroupsConsidered;
                mixedInstances |= mode == 2 && slice.proxyFoliageTriangles && slice.proxyFoliageTriangles < slice.foliageTriangles;
            }
            for (std::size_t p = 0; p < baseline.size(); ++p)
            {
                const auto a = baseline[p];
                const auto receiver = a >> 24;
                if (receiver != 64 && receiver != 191) continue;
                const auto b = mode ? pixels[p] : a;
                if (receiver != (b >> 24)) throw std::runtime_error("Cascade casters changed camera receiver coverage.");
                auto& sample = quality[mode][receiver == 191];
                const auto delta = std::abs(static_cast<int>(a & 255) - static_cast<int>(b & 255));
                ++sample.count;
                sample.changed += delta != 0;
                sample.substantial += delta >= 8;
                sample.error += delta;
            }
        }

        constexpr std::size_t samples = 9;
        std::array<std::array<double, samples>, 3> gpu{}, cpu{}, drawList{};
        if (measure)
        {
            for (std::size_t i = 0; i < samples + 2; ++i)
            {
                for (unsigned pass = 0; pass < modes; ++pass)
                {
                    const auto mode = static_cast<unsigned>((i + pass) % modes);
                    setMode(mode);
                    const auto frame = renderer.drawFrame(scene, view, phase, {});
                    if (!frame.gpuShadowMilliseconds) throw std::runtime_error("GPU shadow timings are unavailable.");
                    if (i < 2) continue;
                    gpu[mode][i - 2] = *frame.gpuShadowMilliseconds;
                    cpu[mode][i - 2] = frame.cpuMilliseconds;
                    drawList[mode][i - 2] = frame.cpuBranchDrawListMilliseconds;
                }
            }
        }
        const auto median = [](auto& values) {
            std::ranges::sort(values);
            return values[values.size() / 2];
        };
        constexpr const char* modeNames[]{"full", "culled", "culled_auto"};
        for (unsigned mode = 0; mode < modes; ++mode)
        {
            const auto& frame = timings[mode];
            csv << pose << ',' << modeNames[mode] << ',' << adapter << ',' << static_cast<unsigned>(viewport.x) << ','
                << static_cast<unsigned>(viewport.y) << ',' << (renderer.msaaEnabled() ? 4 : 1) << ',' << scene.windStrength << ','
                << frame.camera.visibleInstances << ',' << frame.shadow.visibleInstances << ',' << frame.shadow.triangles << ','
                << frame.shadow.foliageTriangles << ',' << frame.shadow.draws << ',' << frame.shadow.foliageCalls;
            for (const auto& slice : frame.shadowCascades) csv << ',' << slice.visibleInstances;
            for (const auto& slice : frame.shadowCascades) csv << ',' << slice.foliageTriangles;
            for (const auto& slice : frame.shadowCascades) csv << ',' << slice.foliageGroupsVisible;
            for (const auto& slice : frame.shadowCascades) csv << ',' << slice.proxyFoliageTriangles;
            for (const auto& receiver : quality[mode])
                csv << ',' << receiver.count << ',' << receiver.changed << ',' << receiver.substantial << ','
                    << (receiver.count ? double(receiver.error) / receiver.count : 0.0);
            if (measure)
                csv << ',' << median(gpu[mode]) << ',' << median(cpu[mode]) << ',' << median(drawList[mode]);
            else
                csv << ",,,";
            csv << '\n';
        }
    };

    renderer.setScene(scene);
    if (!renderer.msaaEnabled()) renderer.toggleMsaa();
    const auto start = oakWalkView(OakWalkRoute::Approach, 0).matrix();
    compare("walk_start", start, 0, true);
    {
        scene.showShadowFactor = false;
        Pixels baseline, coarse;
        setMode(1);
        renderer.drawFrame(scene, start, 0, {}, {}, &baseline);
        setMode(2);
        renderer.drawFrame(scene, start, 0, {}, {}, &coarse);
        auto image = report;
        image.replace_filename(report.stem().native() + L"-walk-start.bmp");
        writeTransitionPair(image, baseline, coarse, {}, {}, static_cast<int>(viewport.x), static_cast<int>(viewport.y));
        scene.showShadowFactor = true;
    }
    compare("walk_glance", oakWalkView(OakWalkRoute::Approach, 21.5f).matrix(), 0, true);
    compare("walk_end", oakWalkView(OakWalkRoute::Approach, 35).matrix(), 0, true);
    scene.windStrength = 1.5f;
    compare("trunk_circle_wind", oakWalkView(OakWalkRoute::Circle, 4.0f).matrix(), 1.9f, false);
    renderer.toggleMsaa();
    scene.branchLodSelection.indirectDraws = false;
    compare("direct_single_sample_wind", oakWalkView(OakWalkRoute::Circle, 4.0f).matrix(), 1.9f, false);
    scene.branchLodSelection.indirectDraws = true;
    renderer.toggleMsaa();

    scene.windStrength = 0;
    scene.objects.front().placement.x = 33.0f;
    scene.sunAzimuth = 90.0f;
    scene.sunElevation = 25.0f;
    renderer.setScene(scene);
    const auto sideView = CameraView{{0, oakWalkEyeHeight, 0}, 0, 0, 20}.matrix();
    compare("off_camera_low_sun", sideView, 0, true, true);
    setMode(2);
    Pixels cast, clear;
    renderer.drawFrame(scene, sideView, 0, {}, {}, &cast);
    const auto flags = scene.meshShadows;
    for (auto& mesh : scene.meshShadows) mesh.cast = false;
    renderer.drawFrame(scene, sideView, 0, {}, {}, &clear);
    scene.meshShadows = flags;
    if (std::ranges::equal(cast, clear)) throw std::runtime_error("The off-camera oak no longer shades the ground.");

    scene.windStrength = 1.0f;
    scene.sunAzimuth = -100;
    scene.sunElevation = 54.5715f;
    scene.objects.front().placement.x = 0;
    for (const auto& p : std::array<XMFLOAT4, 5>{{{-36, -34, 0.4f, 1.0f}, {0, -40, -0.5f, 1.65f},
        {38, -36, 1.9f, 0.9f}, {-35, 17, 1.0f, 0.8f}, {37, 19, -0.6f, 1.05f}}})
        scene.objects.push_back({scene.instanceCount() + 1, p});
    renderer.setScene(scene);
    compare("scattered_oaks_wind", oakWalkView(OakWalkRoute::Approach, 21.5f).matrix(), 1.2f, true);
    scene.branchLod = sourceBranchLod;
    scene.shadowLodOffCameraOnly = true;
    compare("source_off_camera_override", oakWalkView(OakWalkRoute::Approach, 21.5f).matrix(), 1.2f, false, false, 2);
    if (!culledTree || !culledGroup || !mixedInstances)
        throw std::runtime_error("The cascade caster probes missed tree rejection, group rejection, or a mixed shadow LOD list.");
    csv.close();
    return 0;
}
