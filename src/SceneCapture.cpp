#include "SceneCapture.h"

#include "D3D12Renderer.h"
#include "ViewerSettings.h"

#include <wincodec.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <format>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
struct CaptureOptions
{
    fs::path linearHdr;
    SceneKind scene = SceneKind::Cube;
    fs::path imported;
    fs::path settings;
    CameraPreset camera = CameraPreset::Overview;
    UINT width = D3D12Renderer::defaultRenderWidth;
    UINT height = D3D12Renderer::defaultRenderHeight;
    UINT frames = 1;
    double fps = 60.0;
    unsigned msaa = 1;
    bool animateWind = false;
    double windTime = 0;
    std::optional<float> windStrength;
    std::optional<float> yaw;
    std::optional<float> pitch;
    std::optional<float> endPitch;
    std::optional<float> distance;
    std::optional<float> endDistance;
    float orbitDegrees = 0;
    std::optional<int> lod;
    std::optional<SprayTraversal> sprayTraversal;
    std::optional<TreeSubmission> treeSubmission;
    std::optional<unsigned> treeCount;
    std::optional<bool> treeOcclusion;
    std::optional<bool> treeClusterCulling;
    std::optional<bool> treeBatchOcclusion;
    std::optional<FixedLodTint> sprayTint;
    std::optional<bool> sprayCulling;
    std::optional<bool> sprayLocalLod;
    std::optional<bool> leafGroupBounds;
    std::optional<float> sprayPixelLimit;
    std::optional<float> sprayTemplatePixelLimit;
    fs::path boughBake;
    UINT timingSamples = 0;
    bool sequenceTimingsOnly = false;
    std::optional<float> sunAzimuth;
    std::optional<float> sunElevation;
    std::optional<float> backlighting;
    std::optional<float> exposureEv;
    bool noLeafShadowReception = false;
    bool geometryMask = false;
    bool cullMeshBackfaces = false;
    bool cullSprayWoodBackfaces = false;
    fs::path boughVoxelBake;
    fs::path liveCut;
    bool liveCutGpu = false;
    bool profileOakRaster = false;
    bool liveCutUnbatched = false;
    bool liveCutPackAssembly = false;
    bool cameraPipelineStats = false;
    float liveCutPixelError = 1.0f;
    std::optional<float> liveCutErrorDistanceProduct;
    bool softwareBoughRaster = false;
    bool boughDepth = false;
    bool boughMaterials = false;
    CaptureLighting lighting = CaptureLighting::Combined;
    std::optional<ProbeLightingMode> probeLighting;
    std::optional<float> probeWindRefreshVoxels;
    bool forceBough = false;
};

double number(const wchar_t* value, double low, double high, const char* label, bool integer = false)
{
    wchar_t* end = nullptr;
    const double result = std::wcstod(value, &end);
    if (end == value || *end || !std::isfinite(result) || result < low || result > high ||
        (integer && std::trunc(result) != result))
    {
        throw std::runtime_error(std::format("{} must be {}between {} and {}.", label,
                                            integer ? "an integer " : "", low, high));
    }
    return result;
}

CaptureOptions parseOptions(std::span<wchar_t* const> args)
{
    CaptureOptions options;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring_view flag = args[i];
        if (flag == L"--sequence-timings-only")
        {
            options.sequenceTimingsOnly = true;
            continue;
        }
        if (flag == L"--animate-wind")
        {
            options.animateWind = true;
            continue;
        }
        if (flag == L"--no-leaf-shadow-reception")
        {
            options.noLeafShadowReception = true;
            continue;
        }
        if (flag == L"--geometry-mask")
        {
            options.geometryMask = true;
            continue;
        }
        if (flag == L"--cull-mesh-backfaces")
        {
            options.cullMeshBackfaces = true;
            continue;
        }
        if (flag == L"--cull-spray-wood-backfaces")
        {
            options.cullSprayWoodBackfaces = true;
            continue;
        }
        if (flag == L"--brick-raster") { options.softwareBoughRaster = true; continue; }
        if (flag == L"--bough-depth")
        {
            options.boughDepth = true;
            continue;
        }
        if (flag == L"--bough-materials") { options.boughMaterials = true; continue; }
        if (flag == L"--live-cut-gpu") { options.liveCutGpu = true; continue; }
        if (flag == L"--profile-oak-raster") { options.profileOakRaster = true; continue; }
        if (flag == L"--live-cut-unbatched") { options.liveCutUnbatched = true; continue; }
        if (flag == L"--live-cut-pack-assembly") { options.liveCutPackAssembly = true; continue; }
        if (flag == L"--camera-pipeline-stats") { options.cameraPipelineStats = true; continue; }
        if (flag == L"--sky-only")
        {
            options.lighting = CaptureLighting::SkyOnly;
            continue;
        }
        if (flag == L"--force-bough")
        {
            options.forceBough = true;
            continue;
        }
        if (++i == args.size() || !args[i][0] || std::wstring_view(args[i]).starts_with(L"--"))
            throw std::runtime_error("A capture option is missing its value.");
        const auto* value = args[i];
        const std::wstring_view choice = value;
        if (flag == L"--scene")
        {
            const auto kind = findSceneKind(choice);
            options.scene = kind.value_or(SceneKind::Imported);
            options.imported = kind ? fs::path{} : fs::path(value);
        }
        else if (flag == L"--settings") options.settings = value;
        else if (flag == L"--bough-voxel-bake") options.boughVoxelBake = value;
        else if (flag == L"--live-cut") options.liveCut = value;
        else if (flag == L"--live-cut-pixel-error") options.liveCutPixelError = static_cast<float>(number(value,.001,16,"--live-cut-pixel-error"));
        else if (flag == L"--live-cut-error-distance-product")
            options.liveCutErrorDistanceProduct = static_cast<float>(number(value,.001,1e6,"--live-cut-error-distance-product"));
        else if (flag == L"--camera")
        {
            if (choice == L"overview") options.camera = CameraPreset::Overview;
            else if (choice == L"close-up") options.camera = CameraPreset::CloseUp;
            else if (choice == L"under-canopy") options.camera = CameraPreset::UnderCanopy;
            else if (choice == L"distant") options.camera = CameraPreset::Distant;
            else throw std::runtime_error("--camera must be overview, close-up, under-canopy, or distant.");
        }
        else if (flag == L"--width")
            options.width = static_cast<UINT>(number(value, 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "--width", true));
        else if (flag == L"--height")
            options.height = static_cast<UINT>(number(value, 1, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "--height", true));
        else if (flag == L"--frames") options.frames = static_cast<UINT>(number(value, 1, 1'000'000, "--frames", true));
        else if (flag == L"--fps") options.fps = number(value, 1, 240, "--fps");
        else if (flag == L"--msaa")
        {
            if (choice == L"1") options.msaa = 1;
            else if (choice == L"4") options.msaa = 4;
            else throw std::runtime_error("--msaa must be 1 or 4.");
        }
        else if (flag == L"--wind-time") options.windTime = number(value, 0, 1'000'000, "--wind-time");
        else if (flag == L"--wind-strength")
            options.windStrength = static_cast<float>(number(value, 0, maxWindStrength, "--wind-strength"));
        else if (flag == L"--yaw-degrees") options.yaw = static_cast<float>(number(value, -360, 360, "--yaw-degrees"));
        else if (flag == L"--pitch-degrees")
            options.pitch = static_cast<float>(number(value, -83, 83, "--pitch-degrees"));
        else if (flag == L"--end-pitch-degrees")
            options.endPitch = static_cast<float>(number(value, -83, 83, "--end-pitch-degrees"));
        else if (flag == L"--orbit-degrees")
            options.orbitDegrees = static_cast<float>(number(value, -360, 360, "--orbit-degrees"));
        else if (flag == L"--distance")
            options.distance = static_cast<float>(number(value, minimumOrbitDistance, 100'000, "--distance"));
        else if (flag == L"--end-distance")
            options.endDistance = static_cast<float>(number(value, minimumOrbitDistance, 100'000, "--end-distance"));
        else if (flag == L"--sun-azimuth") options.sunAzimuth = static_cast<float>(number(value, -180, 180, "--sun-azimuth"));
        else if (flag == L"--sun-elevation") options.sunElevation = static_cast<float>(number(value, 25, 85, "--sun-elevation"));
        else if (flag == L"--backlighting") options.backlighting = static_cast<float>(number(value, 0, 1, "--backlighting"));
        else if (flag == L"--exposure-ev")
            options.exposureEv = static_cast<float>(number(value, ViewerScene::minExposureEv, ViewerScene::maxExposureEv,
                                                          "--exposure-ev"));
        else if (flag == L"--linear-hdr") options.linearHdr = value;
        else if (flag == L"--lighting")
        {
            if (choice == L"combined") options.lighting = CaptureLighting::Combined;
            else if (choice == L"sun-only") options.lighting = CaptureLighting::SunOnly;
            else if (choice == L"sky-only") options.lighting = CaptureLighting::SkyOnly;
            else if (choice == L"transmission") options.lighting = CaptureLighting::Transmission;
            else if (choice == L"shadow-visibility") options.lighting = CaptureLighting::ShadowVisibility;
            else throw std::runtime_error("--lighting must be combined, sun-only, sky-only, transmission, or shadow-visibility.");
        }
        else if (flag == L"--probe-gi")
        {
            if (choice == L"off") options.probeLighting = ProbeLightingMode::Off;
            else if (choice == L"sky") options.probeLighting = ProbeLightingMode::SkyVisibility;
            else if (choice == L"bounce") options.probeLighting = ProbeLightingMode::GroundBounce;
            else throw std::runtime_error("--probe-gi must be off, sky, or bounce.");
        }
        else if (flag == L"--probe-wind-voxels")
            options.probeWindRefreshVoxels = static_cast<float>(number(value, 0, 1, "--probe-wind-voxels"));
        else if (flag == L"--spray-culling")
        {
            if (choice == L"on") options.sprayCulling = true;
            else if (choice == L"off") options.sprayCulling = false;
            else throw std::runtime_error("--spray-culling must be on or off.");
        }
        else if (flag == L"--spray-local-lod")
        {
            if (choice == L"on") options.sprayLocalLod = true;
            else if (choice == L"off") options.sprayLocalLod = false;
            else throw std::runtime_error("--spray-local-lod must be on or off.");
        }
        else if (flag == L"--leaf-group-bounds")
        {
            if (choice == L"on") options.leafGroupBounds = true;
            else if (choice == L"off") options.leafGroupBounds = false;
            else throw std::runtime_error("--leaf-group-bounds must be on or off.");
        }
        else if (flag == L"--spray-mode")
        {
            if (choice == L"flat") options.sprayTraversal = SprayTraversal::Flat;
            else if (choice == L"grouped") options.sprayTraversal = SprayTraversal::Grouped;
            else if (choice == L"bough") options.sprayTraversal = SprayTraversal::BoughProxy;
            else if (choice == L"hierarchy") options.sprayTraversal = SprayTraversal::Hierarchy;
            else throw std::runtime_error("--spray-mode must be flat, grouped, bough, or hierarchy.");
        }
        else if (flag == L"--tree-submit")
        {
            if (choice == L"direct") options.treeSubmission = TreeSubmission::Direct;
            else if (choice == L"indirect") options.treeSubmission = TreeSubmission::CpuIndirect;
            else if (choice == L"gpu") options.treeSubmission = TreeSubmission::GpuPerPart;
            else if (choice == L"batched") options.treeSubmission = TreeSubmission::GpuBatched;
            else throw std::runtime_error("--tree-submit must be direct, indirect, gpu, or batched.");
        }
        else if (flag == L"--tree-count")
            options.treeCount = static_cast<unsigned>(number(value, 1, 64, "--tree-count", true));
        else if (flag == L"--tree-hzb")
        {
            if (choice == L"on") options.treeOcclusion = true;
            else if (choice == L"off") options.treeOcclusion = false;
            else throw std::runtime_error("--tree-hzb must be on or off.");
        }
        else if (flag == L"--tree-clusters")
        {
            if (choice == L"on") options.treeClusterCulling = true;
            else if (choice == L"off") options.treeClusterCulling = false;
            else throw std::runtime_error("--tree-clusters must be on or off.");
        }
        else if (flag == L"--tree-occlusion")
        {
            if (choice == L"on") options.treeBatchOcclusion = true;
            else if (choice == L"off") options.treeBatchOcclusion = false;
            else throw std::runtime_error("--tree-occlusion must be on or off.");
        }
        else if (flag == L"--spray-color")
        {
            if (choice == L"shaded") options.sprayTint = FixedLodTint::Shaded;
            else if (choice == L"group") options.sprayTint = FixedLodTint::SprayGroup;
            else if (choice == L"lod") options.sprayTint = FixedLodTint::Level;
            else throw std::runtime_error("--spray-color must be shaded, group, or lod.");
        }
        else if (flag == L"--spray-pixel-error")
            options.sprayPixelLimit = static_cast<float>(number(value, 0.05, 4, "--spray-pixel-error"));
        else if (flag == L"--spray-template-pixel-error")
            options.sprayTemplatePixelLimit = static_cast<float>(number(value, 0.05, 4, "--spray-template-pixel-error"));
        else if (flag == L"--bough-bake") options.boughBake = value;
        else if (flag == L"--timing-samples")
            options.timingSamples = static_cast<UINT>(number(value, 1, 200, "--timing-samples", true));
        else if (flag == L"--lod")
        {
            if (choice == L"auto") options.lod = automaticBranchLod;
            else if (choice == L"source") options.lod = sourceBranchLod;
            else options.lod = static_cast<int>(number(value, 0, fixedLodLevelCount - 1, "--lod", true));
        }
        else throw std::runtime_error("Unknown capture option. See Windowless scene capture in README.md.");
    }
    if (options.liveCutErrorDistanceProduct && (options.frames < 2 || options.liveCut.empty()))
        throw std::runtime_error("--live-cut-error-distance-product requires a live-cut sequence.");
    if (options.sequenceTimingsOnly && options.frames < 2)
        throw std::runtime_error("--sequence-timings-only requires a camera sequence.");
    if (options.frames == 1 && (options.orbitDegrees != 0 || options.endDistance || options.endPitch || options.animateWind))
        throw std::runtime_error("Motion options require --frames greater than 1. Use --wind-time for a fixed pose.");
    if (options.frames > 1 && options.timingSamples) throw std::runtime_error("--timing-samples requires a still capture.");
    if (!options.linearHdr.empty() && (options.frames != 1 || options.geometryMask ||
                                      _wcsicmp(options.linearHdr.extension().c_str(), L".dds") != 0))
        throw std::runtime_error("--linear-hdr requires a shaded still capture and a .dds path.");
    if ((options.sprayTraversal || options.treeSubmission || options.treeCount || options.treeOcclusion ||
         options.treeClusterCulling || options.treeBatchOcclusion ||
         options.sprayTint || options.sprayCulling || options.sprayLocalLod || options.leafGroupBounds || options.sprayPixelLimit || options.sprayTemplatePixelLimit ||
         !options.boughBake.empty() || options.forceBough) &&
        options.scene != SceneKind::ForkedOak)
        throw std::runtime_error("Spray settings require --scene forked-oak.");
    if (options.scene == SceneKind::Imported && _wcsicmp(options.imported.extension().c_str(), L".glb") != 0)
        throw std::runtime_error("--scene must name a built-in scene or give the path to a .glb file.");
    return options;
}

void applyOverrides(ViewerScene& scene, const CaptureOptions& options)
{
    if (!options.settings.empty())
    {
        if (!fs::is_regular_file(options.settings)) throw std::runtime_error("Could not find the requested settings file.");
        ViewerSettings(options.settings).apply(scene);
    }
    if (options.lod)
    {
        const int lod = *options.lod;
        const bool sprays = scene.kind == SceneKind::ForkedOak && scene.sprayLods[0];
        const auto levels = sprays ? scene.sprayLods[0]->levels.size() : scene.fixedLod ? scene.fixedLod->levels.size() : 0;
        if ((lod == automaticBranchLod && !sprays && levels <= fixedLodTwoTriangleLevel) ||
            (lod >= 0 && static_cast<std::size_t>(lod) >= levels))
        {
            throw std::runtime_error("The requested triangle LOD requires a compatible bake for this scene.");
        }
        scene.branchVoxelView = BranchVoxelView::Off;
        scene.branchLod = lod;
    }
    scene.sprayFrustumCulling = options.sprayCulling.value_or(scene.sprayFrustumCulling);
    scene.sprayLocalLod = options.sprayLocalLod.value_or(scene.sprayLocalLod);
    scene.leafGroupBounds = options.leafGroupBounds.value_or(scene.leafGroupBounds);
    if (options.sprayTint && *options.sprayTint != FixedLodTint::Shaded && !scene.sprayLods[0])
        throw std::runtime_error("Spray colors require the installed spray bakes.");
    scene.lodTint = options.sprayTint.value_or(scene.lodTint);
    if (!options.boughBake.empty())
    {
        if (!scene.sprayLods[0] || !scene.sprayLods[1])
            throw std::runtime_error("A bough override requires the matching spray LOD bakes.");
        scene.sprayBough = std::make_shared<SprayBough>(
            readSprayBough(sprayBoughSources(scene.source), *scene.asset, scene.sprayLods, options.boughBake));
        scene.sprayTraversal = SprayTraversal::BoughProxy;
    }
    scene.sprayTraversal = options.sprayTraversal.value_or(options.forceBough ? SprayTraversal::BoughProxy : scene.sprayTraversal);
    if (scene.sprayTraversal == SprayTraversal::BoughProxy && !scene.sprayBough)
        throw std::runtime_error("Bough proxy requires an installed bake. " + scene.sprayBoughStatus);
    if (scene.sprayTraversal == SprayTraversal::Hierarchy && !scene.treeHierarchy)
        throw std::runtime_error("Hierarchy traversal requires an installed bake. " + scene.treeHierarchyStatus);
    if (options.treeSubmission && (scene.sprayTraversal != SprayTraversal::Hierarchy || scene.branchLod != automaticBranchLod))
        throw std::runtime_error("--tree-submit requires automatic LOD and hierarchy traversal.");
    scene.treeSubmission = options.treeSubmission.value_or(scene.treeSubmission);
    if (options.leafGroupBounds.value_or(false) && (!scene.sprayLocalLod ||
        scene.sprayTraversal == SprayTraversal::Flat || scene.branchLod != automaticBranchLod ||
        (scene.sprayTraversal == SprayTraversal::Hierarchy &&
         (scene.treeSubmission == TreeSubmission::GpuPerPart || scene.treeSubmission == TreeSubmission::GpuBatched))))
        throw std::runtime_error("--leaf-group-bounds on requires automatic local LOD with CPU selection.");
    scene.treeOcclusion = options.treeOcclusion.value_or(scene.treeOcclusion);
    scene.treeClusterCulling = options.treeClusterCulling.value_or(scene.treeClusterCulling);
    scene.treeBatchOcclusion = options.treeBatchOcclusion.value_or(scene.treeBatchOcclusion);
    if (scene.treeOcclusion && scene.treeSubmission != TreeSubmission::GpuPerPart)
        throw std::runtime_error("--tree-hzb on requires --tree-submit gpu.");
    if (scene.treeClusterCulling && scene.treeSubmission != TreeSubmission::GpuBatched)
        throw std::runtime_error("--tree-clusters on requires --tree-submit batched.");
    if (scene.treeBatchOcclusion && scene.treeSubmission != TreeSubmission::GpuBatched)
        throw std::runtime_error("--tree-occlusion on requires --tree-submit batched.");
    if (options.forceBough && (scene.sprayTraversal != SprayTraversal::BoughProxy || scene.branchLod != automaticBranchLod))
        throw std::runtime_error("--force-bough requires automatic LOD and bough traversal.");
    scene.captureForceBough = options.forceBough;
    scene.sprayPixelLimit = options.sprayPixelLimit.value_or(scene.sprayPixelLimit);
    scene.sprayTemplatePixelLimit = options.sprayTemplatePixelLimit.value_or(scene.sprayTemplatePixelLimit);
    scene.cullSprayWoodBackfaces = options.cullSprayWoodBackfaces;
    scene.boughVoxelBake = options.boughVoxelBake;
    scene.softwareBoughRaster = options.softwareBoughRaster;
    scene.liveCut = options.liveCut;
    scene.liveCutGpu = options.liveCutGpu;
    if (options.liveCutGpu && (options.liveCut.empty() || options.liveCutUnbatched))
        throw std::runtime_error("--live-cut-gpu requires a live cut and generates its own draw batches.");
    scene.liveCutUnbatched = options.liveCutUnbatched;
    scene.liveCutPackAssembly = options.liveCutPackAssembly;
    scene.cameraPipelineStats = options.cameraPipelineStats;
    scene.profileOakRaster = options.profileOakRaster;
    scene.liveCutPixelError = options.liveCutPixelError;
    if (!scene.liveCut.empty() && (scene.kind != SceneKind::ForkedOak || scene.treeSubmission != TreeSubmission::Direct ||
        scene.sprayTraversal != SprayTraversal::Hierarchy || !scene.boughVoxelBake.empty() ||
        scene.branchLod != automaticBranchLod))
        throw std::runtime_error("Live oak cuts require the direct automatic forked-oak hierarchy.");
    if (!scene.boughVoxelBake.empty() && (scene.treeSubmission != TreeSubmission::Direct ||
        scene.sprayTraversal != SprayTraversal::Hierarchy))
        throw std::runtime_error("Bough voxel captures require direct hierarchy submission.");
    scene.captureBoughMaterials = options.boughMaterials;
    scene.captureBoughDepth = options.boughDepth;
    if ((options.boughDepth || options.boughMaterials) && !options.geometryMask)
        throw std::runtime_error("Bough depth and material captures require --geometry-mask.");
    scene.captureGeometryMask = options.geometryMask;
    scene.captureLighting = options.lighting;
    scene.probeLighting = options.probeLighting.value_or(scene.probeLighting);
    scene.probeWindRefreshVoxels = options.probeWindRefreshVoxels.value_or(scene.probeWindRefreshVoxels);
    if (options.geometryMask) scene.branchAppearance = BranchAppearance::Representation;
    scene.sunAzimuth = options.sunAzimuth.value_or(scene.sunAzimuth);
    scene.sunElevation = options.sunElevation.value_or(scene.sunElevation);
    if (options.treeCount) scene.setForkedOakCount(*options.treeCount);
    scene.exposureEv = options.exposureEv.value_or(scene.exposureEv);
    scene.windStrength = options.windStrength.value_or(scene.windStrength);
    if (options.backlighting)
    {
        for (auto& material : scene.asset->materials)
        {
            if (material.hasBacklighting) material.transmission = *options.backlighting;
        }
    }
    if (options.noLeafShadowReception)
    {
        for (std::size_t i = 0; i < scene.asset->meshes.size(); ++i)
        {
            if (scene.asset->materials[scene.asset->meshes[i].material].hasBacklighting)
                scene.meshShadows[i].receive = false;
        }
    }
    if (options.animateWind && (!scene.foliage || scene.windStrength == 0 || scene.branchVoxelView != BranchVoxelView::Off))
        throw std::runtime_error("Animated wind needs a foliage scene with nonzero wind strength and triangle rendering.");
}

void checkCapture(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::format("{} failed (0x{:08X}).", operation, static_cast<unsigned long>(result)));
}

// Publish completed encodes without truncating an existing file, including during concurrent runs.
void writeNewFile(const fs::path& path, std::span<const std::byte> bytes, std::span<const std::byte> payload = {})
{
    if (bytes.size() > MAXDWORD || payload.size() > MAXDWORD)
        throw std::runtime_error("Capture output is too large to write.");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw fs::filesystem_error("Could not create capture output", path,
                                   {static_cast<int>(GetLastError()), std::system_category()});
    const auto write = [&](std::span<const std::byte> data) -> DWORD {
        if (data.empty()) return ERROR_SUCCESS;
        const auto size = static_cast<DWORD>(data.size());
        DWORD written = 0;
        if (!WriteFile(file, data.data(), size, &written, nullptr)) return GetLastError();
        return written == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    };
    auto failure = write(bytes);
    if (!failure) failure = write(payload);
    CloseHandle(file);
    if (failure)
    {
        DeleteFileW(path.c_str());
        throw fs::filesystem_error("Could not write capture output", path,
                                   {static_cast<int>(failure), std::system_category()});
    }
}

void writeLinearDds(const fs::path& path, UINT width, UINT height, std::span<const std::uint64_t> pixels)
{
    // DDS magic, DDS_HEADER, then DDS_HEADER_DXT10. Each pixel is four little-endian half floats.
    std::array<std::uint32_t, 37> header{};
    header[0] = 0x20534444; // "DDS "
    header[1] = 124;
    header[2] = 0x100f; // Caps, height, width, pitch, and pixel format.
    header[3] = height;
    header[4] = width;
    header[5] = width * sizeof(std::uint64_t);
    header[19] = 32; // DDS_PIXELFORMAT size.
    header[20] = 4; // FourCC.
    header[21] = 0x30315844; // "DX10"
    header[27] = 0x1000; // DDSCAPS_TEXTURE.
    header[32] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    header[33] = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    header[35] = 1; // Array size.
    header[36] = 3; // Opaque alpha.
    writeNewFile(path, std::as_bytes(std::span(header)), std::as_bytes(pixels));
}

class PngEncoder
{
public:
    PngEncoder()
    {
        if (apartment_ != RPC_E_CHANGED_MODE) checkCapture(apartment_, "Initialize COM");
        try
        {
            checkCapture(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&factory_)), "Create WIC imaging factory");
        }
        catch (...)
        {
            if (SUCCEEDED(apartment_)) CoUninitialize();
            throw;
        }
    }

    ~PngEncoder()
    {
        factory_.Reset();
        if (SUCCEEDED(apartment_)) CoUninitialize();
    }
    PngEncoder(const PngEncoder&) = delete;
    PngEncoder& operator=(const PngEncoder&) = delete;

    void write(const fs::path& path, UINT width, UINT height, std::span<const std::uint32_t> pixels) const
    {
        const UINT rowBytes = width * sizeof(std::uint32_t);
        ComPtr<IWICBitmap> bitmap;
        checkCapture(factory_->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA, rowBytes,
                     static_cast<UINT>(pixels.size_bytes()),
                     reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(pixels.data())), &bitmap), "Create WIC bitmap");
        ComPtr<IWICBitmapSource> rgb;
        checkCapture(WICConvertBitmapSource(GUID_WICPixelFormat24bppBGR, bitmap.Get(), &rgb), "Convert frame to opaque RGB");

        ComPtr<IStream> stream;
        checkCapture(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "Create PNG stream");
        ComPtr<IWICBitmapEncoder> encoder;
        checkCapture(factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Create PNG encoder");
        checkCapture(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize PNG encoder");
        ComPtr<IWICBitmapFrameEncode> frame;
        checkCapture(encoder->CreateNewFrame(&frame, nullptr), "Create PNG frame");
        checkCapture(frame->Initialize(nullptr), "Initialize PNG frame");
        checkCapture(frame->SetSize(width, height), "Set PNG dimensions");
        auto format = GUID_WICPixelFormat24bppBGR;
        checkCapture(frame->SetPixelFormat(&format), "Set PNG pixel format");
        if (format != GUID_WICPixelFormat24bppBGR) throw std::runtime_error("The PNG encoder did not accept RGB.");

        // The display pass writes sRGB into the UNORM target. Diagnostics write encoded RGB directly.
        ComPtr<IWICMetadataQueryWriter> metadata;
        checkCapture(frame->GetMetadataQueryWriter(&metadata), "Open PNG metadata");
        PROPVARIANT intent{};
        intent.vt = VT_UI1;
        intent.bVal = 0;
        checkCapture(metadata->SetMetadataByName(L"/sRGB/RenderingIntent", &intent), "Set PNG sRGB intent");
        checkCapture(frame->WriteSource(rgb.Get(), nullptr), "Write PNG pixels");
        checkCapture(frame->Commit(), "Commit PNG frame");
        checkCapture(encoder->Commit(), "Commit PNG");

        STATSTG stats{};
        checkCapture(stream->Stat(&stats, STATFLAG_NONAME), "Read PNG size");
        if (stats.cbSize.HighPart) throw std::runtime_error("Encoded PNG exceeds the supported size.");
        std::vector<std::byte> bytes(stats.cbSize.LowPart);
        checkCapture(stream->Seek({}, STREAM_SEEK_SET, nullptr), "Rewind PNG stream");
        ULONG read = 0;
        checkCapture(stream->Read(bytes.data(), stats.cbSize.LowPart, &read), "Read encoded PNG");
        if (read != stats.cbSize.LowPart) throw std::runtime_error("Incomplete encoded PNG.");
        writeNewFile(path, bytes);
    }

private:
    HRESULT apartment_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory_;
};

std::string sprayLodColumns()
{
    std::string columns;
    for (auto pass : {"camera", "shadow"})
        for (auto part : {"wood", "leaf"})
            for (std::size_t level = 0; level < fixedLodLevelCount; ++level)
                columns += std::format(",{}_{}_lod{}_sprays", pass, part, level);
    for (auto pass : {"camera", "shadow"})
        for (auto part : {"leaf", "wood"})
            columns += std::format(",{0}_{1}_local_sprays,{0}_{1}_local_groups,{0}_{1}_intermediate_groups", pass, part);
    for (auto pass : {"camera", "shadow"})
        columns += std::format(",{0}_tree_node_tests,{0}_tree_proxy_nodes,{0}_tree_leaf_sprays,{0}_tree_wood_sprays,"
                               "{0}_tree_leaf_triangles,{0}_tree_wood_triangles,{0}_tree_trunk_triangles,"
                               "{0}_tree_draws,{0}_tree_submission_calls,{0}_tree_submit_ms,{0}_tree_cut_hash,"
                               "{0}_gpu_tree_dispatches,{0}_tree_depth_rejects,{0}_tree_batch_jobs,{0}_tree_batch_instances,"
                               "{0}_tree_batch_coarse_ms,{0}_tree_batch_select_ms,{0}_tree_batch_compact_ms,{0}_tree_batch_draw_ms,"
                               "{0}_tree_cluster_tests,{0}_tree_cluster_rejects,{0}_tree_cluster_splits,"
                               "{0}_tree_cluster_fallbacks,{0}_tree_cluster_gpu_ms,"
                               "{0}_tree_occlusion_active,{0}_tree_occlusion_history,{0}_tree_occlusion_tests,"
                               "{0}_tree_occlusion_deferred,{0}_tree_occlusion_rejected,{0}_tree_occlusion_recovered,"
                               "{0}_tree_occlusion_gpu_ms,{0}_tree_occlusion_retest_ms,{0}_tree_pyramid_gpu_ms",
                               pass);
    return columns + '\n';
}

void appendSprayLodCounts(std::string& csv, const D3D12Renderer::FrameTiming& timing)
{
    for (const auto* pass : {&timing.camera, &timing.shadow})
        for (const auto* counts : {&pass->sprayWoodByLod, &pass->sprayLeavesByLod})
            for (const auto count : *counts)
                csv += std::format(",{}", count);
    for (const auto* pass : {&timing.camera, &timing.shadow})
        for (std::size_t part = 0; part < pass->sprayLocalPlacements.size(); ++part)
            csv += std::format(",{},{},{}", pass->sprayLocalPlacements[part], pass->sprayLocalGroups[part],
                               pass->sprayIntermediateGroups[part]);
    for (const auto* pass : {&timing.camera, &timing.shadow})
    {
        csv += std::format(",{},{},{},{},{},{},{},{},{},{:.6f},{},{},{},{},{}", pass->treeNodeTests, pass->treeNodesDrawn,
            pass->treeProxySprays[0], pass->treeProxySprays[1], pass->treeCanopyTriangles[0],
            pass->treeCanopyTriangles[1], pass->treeTrunkTriangles, pass->treeDraws, pass->treeSubmissionCalls,
            pass->treeSubmissionMilliseconds, pass->treeCutHash, pass->gpuTreeDispatches, pass->treeDepthRejects,
            pass->treeBatchJobs, pass->treeBatchInstances);
        for (const auto ms : pass->treeBatchGpuMilliseconds) csv += std::format(",{:.6f}", ms);
        csv += std::format(",{},{},{},{},{:.6f}", pass->treeClusterTests, pass->treeClusterRejects,
                           pass->treeClusterSplits, pass->treeClusterFallbacks, pass->treeClusterGpuMilliseconds);
        csv += std::format(",{},{},{},{},{},{},{:.6f},{:.6f},{:.6f}",
            static_cast<int>(pass->treeOcclusionActive), static_cast<int>(pass->treeOcclusionHistory),
            pass->treeOcclusionTests, pass->treeOcclusionDeferred, pass->treeOcclusionRejected, pass->treeOcclusionRecovered,
            pass->treeOcclusionGpuMilliseconds, pass->treeOcclusionRetestMilliseconds, pass->treePyramidGpuMilliseconds);
    }
    csv += '\n';
}

CameraView captureCamera(const ViewerScene& scene, const CaptureOptions& options)
{
    auto view = scene.cameraView(options.camera);
    if (options.yaw) view.yaw = DirectX::XMConvertToRadians(*options.yaw);
    if (options.pitch) view.pitch = DirectX::XMConvertToRadians(*options.pitch);
    view.distance = options.distance.value_or(view.distance);
    return view;
}

void profileOakRaster(D3D12Renderer& renderer, ViewerScene& scene, const DirectX::XMFLOAT4X4& matrix,
                      float wind, const fs::path& output)
{
    using Arm = D3D12Renderer::OakRaster;
    constexpr std::array arms{Arm::Full, Arm::Depth, Arm::Visibility, Arm::Flat, Arm::EarlyDepthFull};
    constexpr std::array names{"full", "depth", "visibility", "flat", "early-full"};
    auto path = output;
    path.replace_extension(L".raster.csv");
    if (fs::exists(path)) throw std::runtime_error("The oak raster CSV already exists.");
    auto maskPath = output;
    maskPath.replace_extension(L".mask");
    if (fs::exists(maskPath)) throw std::runtime_error("The oak raster coverage file already exists.");
    const auto draw = [&] { return renderer.drawFrame(scene, matrix, wind, {}); };
    const auto prime = draw().camera;
    if (!prime.gpuTreeDispatches || !prime.treeDraws)
        throw std::runtime_error("Oak raster capture did not generate a live cut.");
    const auto triangles = prime.treeCanopyTriangles[0] + prime.treeCanopyTriangles[1] + prime.treeTrunkTriangles;
    std::vector<uint32_t> reference, depth;
    std::vector<uint64_t> ids;
    std::array<D3D12Renderer::PassStats, arms.size()> counters;
    std::vector<uint64_t> referenceColor, earlyColor;
    std::size_t colorChanges = 0;
    std::size_t occupied = 0;
    scene.cameraPipelineStats = true;
    for (unsigned i = 0; i < arms.size(); ++i)
    {
        renderer.setOakRasterReplay(arms[i], true);
        auto* color = arms[i] == Arm::Full ? &referenceColor : arms[i] == Arm::EarlyDepthFull ? &earlyColor : nullptr;
        counters[i] = renderer.drawFrame(scene, matrix, wind, {}, {}, nullptr, nullptr, color).camera;
        const auto& c = counters[i];
        if (c.inputPrimitives != triangles || c.treeCanopyTriangles != prime.treeCanopyTriangles ||
            c.treeTrunkTriangles != prime.treeTrunkTriangles || c.treeDraws != prime.treeDraws ||
            c.treeSubmissionCalls != prime.treeSubmissionCalls || c.gpuTreeDispatches)
            throw std::runtime_error("Frozen oak draw counts differ from the primed cut.");
        renderer.readOakRasterValidation(depth, ids);
        if (i == 0)
        {
            reference = depth;
            occupied = depth.size() - std::count(depth.begin(), depth.end(), std::bit_cast<uint32_t>(1.0f));
            if (!occupied) throw std::runtime_error("Empty oak depth buffer.");
        }
        else if (depth != reference) throw std::runtime_error("Frozen oak depth differs between shader modes.");
        if (arms[i] == Arm::Visibility)
            for (std::size_t p = 0; p < depth.size(); ++p)
                if ((uint32_t(ids.at(p)) != 0) != (depth[p] != std::bit_cast<uint32_t>(1.0f)))
                    throw std::runtime_error("Oak visibility ID and depth coverage differ.");
    }
    for (size_t p = 0; p < referenceColor.size(); ++p)
        colorChanges += referenceColor[p] != earlyColor.at(p);
    std::vector<uint8_t> mask((reference.size() + 7) / 8);
    for (size_t p = 0; p < reference.size(); ++p)
        if (reference[p] != std::bit_cast<uint32_t>(1.0f)) mask[p / 8] |= uint8_t(1u << (p % 8));
    writeNewFile(maskPath, std::as_bytes(std::span(mask)));
    scene.cameraPipelineStats = false;
    std::string csv = "block,arm,sample,triangles,draws,submissions,vs_invocations,ps_invocations,covered_pixels,early_full_hdr_changed_pixels,tree_gpu_ms,frame_gpu_ms,fenced_ms\n";
    // An odd-sized Williams design needs both the forward and reversed orders.
    constexpr std::array offsets{0u, 1u, 4u, 2u, 3u};
    for (unsigned block = 0; block < 2 * arms.size(); ++block)
        for (unsigned position = 0; position < arms.size(); ++position)
        {
            const auto offset = offsets[block < arms.size() ? position : arms.size() - 1 - position];
            const auto i = (block + offset) % arms.size();
            renderer.setOakRasterReplay(arms[i]);
            for (unsigned warm = 0; warm < 8; ++warm) draw();
            for (unsigned sample = 0; sample < 24; ++sample)
            {
                const auto start = std::chrono::steady_clock::now();
                const auto frame = draw();
                const auto wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (!frame.gpuAssetMilliseconds || !frame.gpuMilliseconds)
                    throw std::runtime_error("Oak raster capture needs GPU timestamps.");
                const auto& c = counters[i];
                csv += std::format("{},{},{},{},{},{},{},{},{},{},{:.6f},{:.6f},{:.6f}\n",
                    block, names[i], sample, triangles, c.treeDraws, c.treeSubmissionCalls, c.vertexInvocations,
                    c.pixelInvocations, occupied, colorChanges, *frame.gpuAssetMilliseconds, *frame.gpuMilliseconds, wall);
            }
        }
    writeNewFile(path, std::as_bytes(std::span(csv)));
    renderer.setOakRasterReplay(Arm::Full); // Save the shaded result using the same frozen buffers.
}
}

SceneCaptureReview prepareSceneCaptureReview(std::span<wchar_t* const> arguments, ViewerScenes& scenes)
{
    const auto options = parseOptions(arguments);
    if (options.frames != 1 || options.timingSamples || !options.linearHdr.empty())
        throw std::runtime_error("Interactive review accepts still-scene settings.");
    if (options.width != D3D12Renderer::defaultRenderWidth || options.height != D3D12Renderer::defaultRenderHeight)
        throw std::runtime_error("Interactive review uses the default 3840 x 2160 viewport.");
    auto& scene = scenes.open(options.scene, options.imported);
    applyOverrides(scene, options);
    scene.windPaused = true;
    return {&scene, captureCamera(scene, options), options.msaa, options.windTime, options.cullMeshBackfaces};
}

void captureScene(std::span<wchar_t* const> arguments, const fs::path& assetDirectory)
{
    if (arguments.empty() || !arguments.front()[0] || std::wstring_view(arguments.front()).starts_with(L"--"))
        throw std::runtime_error("Supply an output PNG path or a new directory for a frame sequence.");
    fs::path output = arguments.front();
    while (output.has_relative_path() && !output.has_filename())
        output = output.parent_path();
    const auto options = parseOptions(arguments.subspan(1));
    if (options.profileOakRaster && (options.frames != 1 || options.msaa != 1 || !options.liveCutGpu ||
        options.liveCutPackAssembly || options.geometryMask || options.timingSamples || options.cameraPipelineStats))
        throw std::runtime_error("Fixed-cut oak profiling requires a single shaded 1x GPU-instanced capture.");
    const bool png = _wcsicmp(output.extension().c_str(), L".png") == 0;
    if (options.frames == 1 && !png) throw std::runtime_error("A still capture requires a .png output path.");
    if (options.frames > 1 && png) throw std::runtime_error("A frame sequence requires a new output directory.");
    if (fs::exists(output)) throw std::runtime_error("Capture output already exists. Choose a new path.");
    if (!options.linearHdr.empty() && fs::exists(options.linearHdr))
        throw std::runtime_error("Linear HDR output already exists. Choose a new path.");
    auto timingPath = output;
    timingPath.replace_extension(L".timings.csv");
    if (options.timingSamples && fs::exists(timingPath))
        throw std::runtime_error("Timing CSV already exists. Choose a new capture path.");
    ViewerScenes scenes(assetDirectory);
    auto& scene = scenes.open(options.scene, options.imported);
    applyOverrides(scene, options);

    D3D12Renderer renderer(options.width, options.height, assetDirectory / L"MeadowDaySky.hdr", options.cullMeshBackfaces);
    if (options.msaa == 4 && !renderer.supportsMsaa())
        throw std::runtime_error("The selected D3D12 device does not support 4x MSAA for this renderer.");
    if (renderer.msaaEnabled() != (options.msaa == 4)) renderer.toggleMsaa();
    renderer.setScene(scene);

    const auto start = captureCamera(scene, options);
    const auto endDistance = options.endDistance.value_or(start.distance);
    const auto endPitch = options.endPitch ? DirectX::XMConvertToRadians(*options.endPitch) : start.pitch;
    const auto orbitRadians = DirectX::XMConvertToRadians(options.orbitDegrees);

    const bool sequence = options.frames > 1;
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
    if (!options.linearHdr.parent_path().empty()) fs::create_directories(options.linearHdr.parent_path());
    if (sequence && !fs::create_directory(output))
        throw std::runtime_error("Could not create a new capture sequence directory.");

    const PngEncoder encoder;
    std::vector<std::uint32_t> pixels;
    std::vector<std::uint64_t> linearPixels;
    const auto lodColumns = sprayLodColumns();
    std::string manifest = "frame,fps,seconds,wind_seconds,yaw_degrees,pitch_degrees,distance,msaa,"
        "camera_spray_wood_triangles,shadow_spray_wood_triangles,camera_coarse_wood_sprays,shadow_coarse_wood_sprays,"
        "camera_triangles,shadow_triangles,camera_leaf_triangles,camera_voxel_bricks,gpu_asset_ms,gpu_bough_brick_ms,gpu_camera_ms,gpu_frame_ms" +
        lodColumns;
    for (UINT index = 0; index < options.frames; ++index)
    {
        const double seconds = index / options.fps;
        const double windTime = options.windTime + (options.animateWind ? seconds : 0);
        const auto fraction = sequence ? static_cast<float>(index) / static_cast<float>(options.frames - 1) : 0;
        auto view = start;
        view.yaw += orbitRadians * fraction;
        view.pitch = std::lerp(start.pitch, endPitch, fraction);
        view.distance = std::lerp(start.distance, endDistance, fraction);
        if (options.liveCutErrorDistanceProduct)
            scene.liveCutPixelError = std::clamp(std::round(10000.0f * *options.liveCutErrorDistanceProduct / view.distance)
                                                 / 10000.0f, .001f, options.liveCutPixelError);
        renderer.updateOrbitCameraProjection(scene, view.distance);
        const auto matrix = view.matrix();
        renderer.updateBranchLod(scene, matrix);
        if (options.profileOakRaster)
            profileOakRaster(renderer, scene, matrix, windPhaseAt(windTime), output);
        if (options.timingSamples)
        {
            for (int warmup = 0; warmup < 2; ++warmup)
                renderer.drawFrame(scene, matrix, windPhaseAt(windTime), {});
            std::string csv =
                "sample,cpu_ms,gpu_frame_ms,gpu_camera_ms,gpu_shadow_ms,camera_triangles,shadow_triangles,"
                "camera_leaf_triangles,shadow_leaf_triangles,camera_spray_wood_triangles,shadow_spray_wood_triangles,"
                "camera_sprays,shadow_sprays,camera_sprays_considered,shadow_sprays_considered,camera_draws,shadow_draws,"
                "camera_spray_group_tests,shadow_spray_group_tests,camera_spray_tests,shadow_spray_tests,"
                "camera_bough_triangles,shadow_bough_triangles,camera_bough_sprays,shadow_bough_sprays,"
                "camera_coarse_wood_sprays,shadow_coarse_wood_sprays,camera_voxel_bricks,shadow_voxel_bricks,scene_far_m,camera_selection_cpu_ms,camera_mesh_vs_invocations,camera_mesh_ps_invocations,fenced_frame_ms,gpu_asset_ms,gpu_live_cut_ms" + lodColumns;
            for (UINT sample = 0; sample < options.timingSamples; ++sample)
            {
                const auto frameStart = std::chrono::steady_clock::now();
                const auto t = renderer.drawFrame(scene, matrix, windPhaseAt(windTime), {});
                const auto fencedMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frameStart).count();
                if (!t.gpuMilliseconds || !t.gpuCameraMilliseconds || !t.gpuShadowMilliseconds)
                    throw std::runtime_error("GPU timestamp queries are unavailable on this device.");
                csv += std::format("{},{:.6f},{:.6f},{:.6f},{:.6f},"
                    "{},{},{},{},{},{},"
                    "{},{},{},{},{},{},"
                    "{},{},{},{},"
                    "{},{},{},{},{},{}",
                    sample + 1, t.cpuMilliseconds, *t.gpuMilliseconds, *t.gpuCameraMilliseconds, *t.gpuShadowMilliseconds,
                    t.camera.triangles, t.shadow.triangles, t.camera.foliageTriangles, t.shadow.foliageTriangles,
                    t.camera.sprayWoodTriangles, t.shadow.sprayWoodTriangles,
                    t.camera.visibleSprayPlacements, t.shadow.visibleSprayPlacements,
                    t.camera.sprayPlacements, t.shadow.sprayPlacements, t.camera.draws, t.shadow.draws,
                    t.camera.sprayGroupTests, t.shadow.sprayGroupTests, t.camera.sprayPlacementTests, t.shadow.sprayPlacementTests,
                    t.camera.boughProxyTriangles, t.shadow.boughProxyTriangles,
                    t.camera.boughProxyPlacements, t.shadow.boughProxyPlacements,
                    t.camera.sprayWoodByLod[fixedLodSharedCoarseLevel], t.shadow.sprayWoodByLod[fixedLodSharedCoarseLevel]);
                csv += std::format(",{},{},{},{:.6f},{},{}", t.camera.voxelBricks,t.shadow.voxelBricks,scene.farPlane(),t.camera.treeSelectionMilliseconds,
                                   t.camera.vertexInvocations,t.camera.pixelInvocations);
                csv += std::format(",{:.6f},{:.6f},{:.6f}", fencedMs, *t.gpuAssetMilliseconds, *t.gpuLiveCutMilliseconds);
                appendSprayLodCounts(csv, t);
            }
            writeNewFile(timingPath, std::as_bytes(std::span(csv)));
        }
        if (!sequence && scene.treeBatchOcclusion && !options.timingSamples)
            renderer.drawFrame(scene, matrix, windPhaseAt(windTime), {});
        if (options.sequenceTimingsOnly && index == 0)
            for (unsigned warm = 0; warm < 32; ++warm)
                renderer.drawFrame(scene, matrix, windPhaseAt(windTime), {});
        const auto timing = renderer.drawFrame(scene, matrix, windPhaseAt(windTime), {}, {}, options.sequenceTimingsOnly ? nullptr : &pixels, nullptr,
                                               options.linearHdr.empty() ? nullptr : &linearPixels);

        const auto path = sequence ? output / std::format(L"frame-{:06}.png", index) : output;
        if (!options.sequenceTimingsOnly) encoder.write(path, options.width, options.height, pixels);
        if (!options.linearHdr.empty()) writeLinearDds(options.linearHdr, options.width, options.height, linearPixels);
        if (sequence)
        {
            manifest += std::format("{},{},{:.9f},{:.9f},{},{},{},{},{},{},{},{},{},{}", index, options.fps, seconds, windTime,
                                    DirectX::XMConvertToDegrees(view.yaw), DirectX::XMConvertToDegrees(view.pitch),
                                    view.distance, options.msaa, timing.camera.sprayWoodTriangles,
                                    timing.shadow.sprayWoodTriangles, timing.camera.sprayWoodByLod[fixedLodSharedCoarseLevel],
                                    timing.shadow.sprayWoodByLod[fixedLodSharedCoarseLevel], timing.camera.triangles,
                                    timing.shadow.triangles);
            manifest += std::format(",{},{},{:.6f},{:.6f},{:.6f},{:.6f}", timing.camera.foliageTriangles,
                timing.camera.voxelBricks, timing.gpuAssetMilliseconds.value_or(0),
                timing.gpuBoughBrickMilliseconds, timing.gpuCameraMilliseconds.value_or(0), timing.gpuMilliseconds.value_or(0));
            appendSprayLodCounts(manifest, timing);
        }
    }
    if (sequence) writeNewFile(output / L"frames.csv", std::as_bytes(std::span(manifest)));
}

void captureProbeWalk(const fs::path& output, const fs::path& assetDirectory)
{
    if (output.empty() || !fs::create_directories(output))
        throw std::runtime_error("Choose a new probe walk output directory.");
    constexpr UINT width = D3D12Renderer::defaultRenderWidth, height = D3D12Renderer::defaultRenderHeight;
    constexpr unsigned samplesPerSecond = 60;
    constexpr auto route = OakWalkRoute::Approach;
    constexpr unsigned walkFrames = static_cast<unsigned>(oakWalkDuration(route) * samplesPerSecond) + 1;
    constexpr float firstSun = -65.0f, secondSun = -25.0f;
    const PngEncoder encoder;
    D3D12Renderer renderer(width, height, assetDirectory / L"MeadowDaySky.hdr");
    if (!renderer.msaaEnabled()) throw std::runtime_error("The probe walk requires 4x MSAA.");
    ViewerScenes scenes(assetDirectory);
    auto& grove = scenes.open(SceneKind::Grove);
    auto& branches = scenes.open(SceneKind::OakBranchGrid);
    if (!branches.hasAutomaticBranchVoxels())
        throw std::runtime_error("Install the eighth-meter coverage and normal bakes to verify the voxel transition.");
    const auto hardware = std::format(L"\uFEFFAdapter: {}\n{}x{}, 4x MSAA, 0 EV\n",
                                      renderer.adapterName(), width, height);
    writeNewFile(output / "hardware.txt", std::as_bytes(std::span(hardware)));
    std::string report = "test,frame,seconds,sun_azimuth,cpu_ms,gpu_frame_ms,gpu_shadow_ms,gpu_camera_ms,gpu_probe_ms,"
                         "probe_count,probe_bytes,probes_updated,probes_pending,camera_triangles,camera_voxel_bricks,"
                         "shadow_triangles,camera_local_leaf_groups,camera_local_wood_groups,camera_bough_sprays,"
                         "camera_hierarchy_leaf_sprays,camera_hierarchy_wood_sprays,camera_hierarchy_trunk_triangles\n";
    const auto record = [&](std::string_view mode, unsigned frame, double time, const ViewerScene& scene,
                            const D3D12Renderer::FrameTiming& timing) {
        if (!timing.gpuMilliseconds || !timing.gpuShadowMilliseconds ||
            !timing.gpuCameraMilliseconds || !timing.gpuProbeMilliseconds)
            throw std::runtime_error("GPU timestamp queries are unavailable.");
        const bool probesEnabled = scene.probeLighting != ProbeLightingMode::Off;
        report += std::format("{},{},{:.6f},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},"
                              "{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
                              mode, frame, time, scene.sunAzimuth, timing.cpuMilliseconds, *timing.gpuMilliseconds,
                              *timing.gpuShadowMilliseconds, *timing.gpuCameraMilliseconds, *timing.gpuProbeMilliseconds,
                              probesEnabled ? renderer.probeCount() : 0, probesEnabled ? renderer.probeBytes() : 0,
                              timing.probesUpdated, timing.probesPending, timing.camera.triangles, timing.camera.voxelBricks,
                              timing.shadow.triangles, timing.camera.sprayLocalGroups[0], timing.camera.sprayLocalGroups[1],
                              timing.camera.boughProxyPlacements, timing.camera.treeProxySprays[0],
                              timing.camera.treeProxySprays[1], timing.camera.treeTrunkTriangles);
    };
    std::vector<std::uint32_t> pixels;
    const auto save = [&](const fs::path& path, const std::vector<std::uint32_t>& frame) {
        encoder.write(output / path, width, height, frame);
    };
    grove.treeCount = 25;
    grove.exposureEv = 0;
    grove.sunElevation = 49;
    grove.windStrength = 1.0f;
    for (const bool enabled : {false, true})
    {
        const std::string mode = enabled ? "walk-probes" : "walk-control";
        grove.probeLighting = enabled ? ProbeLightingMode::GroundBounce : ProbeLightingMode::Off;
        grove.sunAzimuth = firstSun;
        renderer.setScene(grove);
        for (unsigned frame = 0; frame < walkFrames; ++frame)
        {
            if (frame == walkFrames / 2)
            {
                grove.sunAzimuth = secondSun;
                renderer.updateSun(grove);
            }
            const float seconds = static_cast<float>(frame) / samplesPerSecond;
            const auto view = oakWalkView(route, seconds).matrix();
            const bool snapshot = frame == 0 || frame == walkFrames / 2 || frame + 1 == walkFrames;
            const auto timing = renderer.drawFrame(grove, view, windPhaseAt(seconds), {}, {}, snapshot ? &pixels : nullptr);
            record(mode, frame, seconds, grove, timing);
            if (snapshot) save(std::format("{}-{:04}.png", mode, frame), pixels);
        }
    }

    grove.windStrength = 0;
    grove.sunAzimuth = firstSun;
    renderer.setScene(grove);
    const auto settledPose = oakWalkView(route, 31).matrix();
    renderer.drawFrame(grove, settledPose, 0, {}, {}, &pixels);
    save("sun-before.png", pixels);
    grove.sunAzimuth = secondSun;
    renderer.updateSun(grove);
    std::vector<std::uint32_t> settled;
    unsigned settleFrames = 0;
    for (unsigned frame = 0; frame < 240; ++frame)
    {
        const auto timing = renderer.drawFrame(grove, settledPose, 0, {}, {}, frame == 0 ? &pixels : nullptr);
        record("sun-step", frame, static_cast<double>(frame) / samplesPerSecond, grove, timing);
        if (frame == 0) save("sun-first-frame.png", pixels);
        if (!timing.probesPending)
        {
            settleFrames = frame + 1;
            renderer.drawFrame(grove, settledPose, 0, {}, {}, &settled);
            save("sun-settled.png", settled);
            break;
        }
    }
    renderer.setScene(grove);
    renderer.drawFrame(grove, settledPose, 0, {}, {}, &pixels);
    if (!settleFrames || settled != pixels)
        throw std::runtime_error("The scheduled probe updates did not reproduce a fresh static field.");
    const auto convergence = std::format("Settled after {} frames. Display matched a fresh field pixel for pixel.\n", settleFrames);
    writeNewFile(output / "sun-step.txt", std::as_bytes(std::span(convergence)));

    grove.windStrength = 1;
    renderer.setScene(grove);
    for (unsigned frame = 0; frame < 48; ++frame)
        renderer.drawFrame(grove, settledPose, windPhaseAt(static_cast<double>(frame) / samplesPerSecond), {});
    const float heldWindPhase = windPhaseAt(47.0 / samplesPerSecond);
    settleFrames = 0;
    for (unsigned frame = 0; frame < 240; ++frame)
    {
        const auto timing = renderer.drawFrame(grove, settledPose, heldWindPhase, {}, {});
        record("wind-stop", frame, static_cast<double>(frame) / samplesPerSecond, grove, timing);
        if (!timing.probesPending)
        {
            settleFrames = frame + 1;
            renderer.drawFrame(grove, settledPose, heldWindPhase, {}, {}, &settled);
            break;
        }
    }
    renderer.setScene(grove);
    renderer.drawFrame(grove, settledPose, heldWindPhase, {}, {}, &pixels);
    if (!settleFrames || settled != pixels)
        throw std::runtime_error("The scheduled probe updates did not reproduce a fresh field at the held wind pose.");
    const auto windConvergence = std::format(
        "Settled after {} frames. Display matched a fresh field at the held wind pose pixel for pixel.\n", settleFrames);
    writeNewFile(output / "wind-stop.txt", std::as_bytes(std::span(windConvergence)));
    save("wind-settled.png", settled);

    branches.setBranchGrid(3);
    branches.probeLighting = ProbeLightingMode::GroundBounce;
    branches.windStrength = 0;
    branches.branchVoxelUseNormals = true;
    branches.branchVoxelView = BranchVoxelView::Automatic;
    branches.branchVoxelEnterPixels = 1.0f;
    branches.branchAppearance = BranchAppearance::Shaded;
    branches.branchLod = automaticBranchLod;
    renderer.setScene(branches);
    renderer.updateCameraProjection(branchVoxelFarNearPlane, branchVoxelFarClipPlane);
    auto pose = branches.cameraView(CameraPreset::Overview);
    bool sawMesh = false, sawVoxels = false, sawMixed = false;
    for (unsigned frame = 0; frame <= 120; ++frame)
    {
        pose.distance = 45.0f * std::pow(3200.0f / 45.0f, frame / 120.0f);
        const auto view = pose.matrix();
        renderer.updateBranchLod(branches, view);
        const auto timing = renderer.drawFrame(branches, view, 0, {});
        record("branch-lod", frame, static_cast<double>(frame) / samplesPerSecond, branches, timing);
        const bool voxels = timing.camera.voxelBricks != 0;
        const bool mesh = timing.camera.foliageTriangles > timing.camera.voxelBricks * 12;
        sawMesh |= mesh;
        sawVoxels |= voxels;
        if (mesh && voxels && !sawMixed)
        {
            sawMixed = true;
            renderer.drawFrame(branches, view, 0, {}, {}, &pixels);
            save("branch-mixed-lod.png", pixels);
        }
    }

    // Same camera and field, with the eighth-meter foliage representation forced for inspection.
    pose.distance = 45;
    renderer.updateCameraProjection(branches.nearPlane, branches.farPlane());
    for (const bool voxels : {false, true})
    {
        branches.branchVoxelView = voxels ? BranchVoxelView::CellOneEighth : BranchVoxelView::Off;
        branches.branchLod = sourceBranchLod;
        const auto view = pose.matrix();
        renderer.updateBranchLod(branches, view);
        renderer.drawFrame(branches, view, 0, {}, {}, &pixels);
        save(voxels ? "branch-voxel.png" : "branch-mesh.png", pixels);
    }
    if (!sawMesh || !sawVoxels || !sawMixed)
        throw std::runtime_error("The branch sweep did not exercise mesh, voxel, and mixed foliage.");

    auto& oak = scenes.open(SceneKind::ForkedOak);
    if (!oak.sprayBough || !oak.treeHierarchy)
        throw std::runtime_error("Install the Forked White Oak spray, bough, and hierarchy bakes.");
    oak.exposureEv = 0;
    oak.windStrength = 0;
    oak.sprayLocalLod = true;
    renderer.setScene(oak);

    struct OakCase
    {
        std::string_view label;
        SprayTraversal traversal;
        float distance;
    };
    constexpr OakCase oakCases[] {
        {"forked-grouped-41m", SprayTraversal::Grouped, 41},
        {"forked-hierarchy-41m", SprayTraversal::Hierarchy, 41},
        {"forked-grouped-100m", SprayTraversal::Grouped, 100},
        {"forked-bough-100m", SprayTraversal::BoughProxy, 100},
        {"forked-hierarchy-100m", SprayTraversal::Hierarchy, 100}
    };
    bool oakFieldInitialized = false;
    for (const auto& test : oakCases)
    {
        oak.sprayTraversal = test.traversal;
        auto oakPose = oak.cameraView(test.distance == 100 ? CameraPreset::Distant : CameraPreset::Overview);
        oakPose.distance = test.distance;
        renderer.updateOrbitCameraProjection(oak, oakPose.distance);
        const auto view = oakPose.matrix();
        renderer.updateBranchLod(oak, view);

        D3D12Renderer::FrameTiming control{};
        for (const bool enabled : {false, true})
        {
            oak.probeLighting = enabled ? ProbeLightingMode::GroundBounce : ProbeLightingMode::Off;
            const auto mode = std::format("{}-{}", test.label, enabled ? "probes" : "control");
            const auto first = renderer.drawFrame(oak, view, 0, {});
            // Reuse the field while changing the camera and its geometry cuts.
            const unsigned expectedUpdates = enabled && !oakFieldInitialized ? renderer.probeCount() : 0;
            if (first.probesUpdated != expectedUpdates || (enabled && !renderer.probeCount()))
                throw std::runtime_error("The static Forked White Oak did not reuse its probe field across geometry cuts.");
            oakFieldInitialized |= enabled;

            for (unsigned warmup = 0; warmup < 4; ++warmup)
                renderer.drawFrame(oak, view, 0, {});
            for (unsigned sample = 0; sample < 61; ++sample)
            {
                const auto timing = renderer.drawFrame(oak, view, 0, {});
                record(mode, sample, 0, oak, timing);
            }
            const auto timing = renderer.drawFrame(oak, view, 0, {}, {}, &pixels);
            if (!enabled) control = timing;
            else if (timing.camera.triangles != control.camera.triangles ||
                     timing.shadow.triangles != control.shadow.triangles ||
                     timing.camera.treeCutHash != control.camera.treeCutHash ||
                     timing.shadow.treeCutHash != control.shadow.treeCutHash)
                throw std::runtime_error("The paired Forked White Oak captures selected different cuts.");
            save(mode + ".png", pixels);
        }
    }
    writeNewFile(output / "timings.csv", std::as_bytes(std::span(report)));
}

void captureProbeWind(const fs::path& output, const fs::path& assetDirectory, bool captureQuality)
{
    if (output.empty() || !fs::create_directories(output))
        throw std::runtime_error("Choose a new probe wind benchmark directory.");
    constexpr UINT width = D3D12Renderer::defaultRenderWidth, height = D3D12Renderer::defaultRenderHeight;
    constexpr unsigned fps = 60, breezeStart = 40, gustStart = 124, pauseStart = 660, frameCount = 756;
    constexpr std::array keyframes{39u, 40u, 56u, 84u, 123u, 124u, 128u, 132u, 140u, 156u, 212u,
                                   243u, 359u, 479u, 599u, 659u, 660u, 676u, 708u, 724u, 755u};
    struct WindPose { std::string_view section; float strength; float phase; };
    const auto windAt = [&](unsigned frame) -> WindPose {
        if (frame < breezeStart) return {"calm", 0, 0};
        const auto phase = windPhaseAt(1.45 + static_cast<double>(std::min(frame, pauseStart - 1) - breezeStart) / fps);
        if (frame < gustStart) return {"breeze", 0.18f, phase};
        const float ramp = std::clamp(static_cast<float>(frame - gustStart) / 12.0f, 0.0f, 1.0f);
        const float strength = std::lerp(0.18f, 1.20f, ramp * ramp * (3.0f - 2.0f * ramp));
        return {frame < pauseStart ? "gust" : "pause", strength, phase};
    };

    D3D12Renderer renderer(width, height, assetDirectory / L"MeadowDaySky.hdr");
    if (!renderer.msaaEnabled()) throw std::runtime_error("The probe wind benchmark requires 4x MSAA.");
    ViewerScenes scenes(assetDirectory);
    auto& oak = scenes.open(SceneKind::ForkedOak);
    if (!oak.treeHierarchy)
        throw std::runtime_error("Install the reviewed Forked White Oak hierarchy bake. " + oak.treeHierarchyStatus);
    oak.sprayTraversal = SprayTraversal::Hierarchy;
    oak.exposureEv = 0;
    oak.probeLighting = ProbeLightingMode::GroundBounce;
    const auto settings = std::format(L"\uFEFFAdapter: {}\n{}x{}, 4x MSAA, 0 EV, {} Hz wind timeline\n"
        L"Forked White Oak hierarchy, fixed sun. Wind refresh limit: {} rendered frames.\n",
        renderer.adapterName(), width, height, fps, ViewerScene::probeWindMaxInterval);
    writeNewFile(output / "hardware.txt", std::as_bytes(std::span(settings)));

    struct Candidate { std::string_view label; float voxels; };
    constexpr std::array candidates{Candidate{"every-frame", 0}, Candidate{"eighth", 0.125f},
                                    Candidate{"quarter", 0.25f}, Candidate{"half", 0.5f}};
    std::string report = "view,mode,threshold_voxels,frame,seconds,wind_section,wind_strength,wind_phase,"
        "cpu_ms,gpu_frame_ms,gpu_probe_ms,gpu_camera_ms,gpu_shadow_ms,probes_updated,probes_pending,advection,ground_update,readback,"
        "camera_triangles,shadow_triangles\n";
    const PngEncoder encoder;
    std::vector<std::uint64_t> linear;
    std::vector<std::uint32_t> mask;
    for (bool orbit : {false, true})
    {
        const auto viewName = orbit ? "orbit" : "under";
        auto start = oak.cameraView(orbit ? CameraPreset::Overview : CameraPreset::UnderCanopy);
        start.yaw = DirectX::XMConvertToRadians(orbit ? 132.0f : -37.24f);
        start.pitch = DirectX::XMConvertToRadians(orbit ? -5.0f : -21.0f);
        start.distance = orbit ? 29.0f : 7.0f;
        const auto cameraAt = [&](unsigned frame) {
            auto pose = start;
            if (orbit) pose.yaw += DirectX::XMConvertToRadians(16.0f) * frame / (frameCount - 1);
            return pose.matrix();
        };
        const auto viewDir = output / viewName;
        for (std::size_t run = 0; run < candidates.size(); ++run)
        {
            const auto& candidate = candidates[orbit ? candidates.size() - 1 - run : run];
            const auto destination = viewDir / candidate.label;
            fs::create_directories(destination);
            oak.probeWindRefreshVoxels = candidate.voxels;
            renderer.setScene(oak);
            renderer.updateOrbitCameraProjection(oak, start.distance);
            for (unsigned frame = 0; frame < frameCount; ++frame)
            {
                const auto wind = windAt(frame);
                oak.windStrength = wind.strength;
                const auto view = cameraAt(frame);
                const bool sampled = captureQuality && std::binary_search(keyframes.begin(), keyframes.end(), frame);
                const auto timing = renderer.drawFrame(oak, view, wind.phase, {}, {}, nullptr, nullptr,
                                                       sampled ? &linear : nullptr);
                if (!timing.gpuMilliseconds || !timing.gpuProbeMilliseconds ||
                    !timing.gpuCameraMilliseconds || !timing.gpuShadowMilliseconds)
                    throw std::runtime_error("GPU timestamp queries are unavailable.");
                report += std::format("{},{},{},{},{:.6f},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{},{},{},{},{},{},{}\n",
                    viewName, candidate.label, candidate.voxels, frame, static_cast<double>(frame) / fps,
                    wind.section, wind.strength, wind.phase, timing.cpuMilliseconds, *timing.gpuMilliseconds,
                    *timing.gpuProbeMilliseconds, *timing.gpuCameraMilliseconds, *timing.gpuShadowMilliseconds,
                    timing.probesUpdated, timing.probesPending,
                    unsigned(timing.probeWindAdvection), unsigned(timing.probeGroundUpdate), unsigned(sampled),
                    timing.camera.triangles, timing.shadow.triangles);
                if (sampled)
                    writeLinearDds(destination / std::format("frame-{:03}.dds", frame), width, height, linear);
                if (frame + 1 == frameCount && timing.probesPending)
                    throw std::runtime_error("Probes did not finish settling at the held wind pose.");
            }
        }

        if (!captureQuality) continue;
        fs::create_directories(viewDir / "fresh");
        fs::create_directories(viewDir / "mask");
        for (const auto frame : keyframes)
        {
            const auto wind = windAt(frame);
            const auto view = cameraAt(frame);
            oak.windStrength = wind.strength;
            renderer.setScene(oak);
            renderer.updateOrbitCameraProjection(oak, start.distance);
            renderer.drawFrame(oak, view, wind.phase, {}, {}, nullptr, nullptr, &linear);
            writeLinearDds(viewDir / "fresh" / std::format("frame-{:03}.dds", frame), width, height, linear);
            oak.captureGeometryMask = true;
            renderer.drawFrame(oak, view, wind.phase, {}, {}, &mask);
            encoder.write(viewDir / "mask" / std::format("frame-{:03}.png", frame), width, height, mask);
            oak.captureGeometryMask = false;
        }
    }
    writeNewFile(output / "timings.csv", std::as_bytes(std::span(report)));
}

void reportCaptureError(const char* message)
{
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (!error || error == INVALID_HANDLE_VALUE)
    {
        AttachConsole(ATTACH_PARENT_PROCESS);
        error = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (error && error != INVALID_HANDLE_VALUE)
    {
        constexpr char prefix[] = "Scene capture: ";
        DWORD written = 0;
        WriteFile(error, prefix, sizeof(prefix) - 1, &written, nullptr);
        WriteFile(error, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
        WriteFile(error, "\r\n", 2, &written, nullptr);
    }
    else OutputDebugStringA(message);
}
