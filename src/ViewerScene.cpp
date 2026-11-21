#include "ViewerScene.h"

#include "GltfLoader.h"
#include "SourceFingerprint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

using namespace DirectX;

namespace
{
void populateGrove(ViewerScene& scene)
{
    struct Cell { int x; int z; };
    std::vector<Cell> cells;
    for (int z = -10; z <= 10; ++z)
    {
        for (int x = -10; x <= 10; ++x)
        {
            cells.push_back({x, z});
        }
    }
    std::stable_sort(cells.begin(), cells.end(), [](const Cell& a, const Cell& b) {
        return a.x * a.x + a.z * a.z < b.x * b.x + b.z * b.z;
    });
    scene.objects.reserve(groveCounts.back());
    std::minstd_rand random(2026);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (unsigned i = 1; i < groveCounts.back(); ++i)
    {
        const auto cell = cells[i];
        scene.objects.push_back({i + 1, {11.0f * cell.x + 4.0f * (unit(random) - 0.5f),
                                        11.0f * cell.z + 4.0f * (unit(random) - 0.5f),
                                        XM_2PI * unit(random), 0.8f + 0.35f * unit(random)}});
    }
}

MeshData makeGround(const ViewerScene& scene)
{
    const auto& [center, halfExtent] = scene.assetBounds;
    const float height = std::max(0.0f, center.y + halfExtent.y);
    const float x = std::abs(center.x) + halfExtent.x;
    const float z = std::abs(center.z) + halfExtent.z;
    const auto sun = scene.sunDirection();
    float extent = std::max(x + height * std::abs(sun.x / sun.y),
                            z + height * std::abs(sun.z / sun.y));
    // Allow space for the longer shadows from the sun elevation control.
    const float radius = std::hypot(x, z);
    const float shadowLength = height / std::tan(XMConvertToRadians(25.0f));
    for (const auto& object : scene.objects)
    {
        const auto& p = object.placement;
        extent = std::max(extent, std::max(std::abs(p.x), std::abs(p.y)) + (radius + shadowLength) * p.w);
    }
    extent = std::max(4.0f, 1.3f * extent);

    MeshData ground{};
    ground.name = "Ground";
    ground.vertices = {
        {{-extent, 0.0f, -extent}, {0.0f, 1.0f, 0.0f}, {}},
        {{-extent, 0.0f,  extent}, {0.0f, 1.0f, 0.0f}, {}},
        {{ extent, 0.0f,  extent}, {0.0f, 1.0f, 0.0f}, {}},
        {{ extent, 0.0f, -extent}, {0.0f, 1.0f, 0.0f}, {}},
    };
    ground.indices = {0, 1, 2, 0, 2, 3};
    ground.localBounds = {{0.0f, 0.0f, 0.0f}, {extent, 0.0f, extent}};
    XMStoreFloat4x4(&ground.localToWorld, XMMatrixIdentity());
    return ground;
}

Bounds assetBounds(std::span<const MeshData> meshes)
{
    auto lower = XMVectorReplicate(std::numeric_limits<float>::max());
    auto upper = -lower;
    const auto include = [&](FXMVECTOR point) {
        lower = XMVectorMin(lower, point);
        upper = XMVectorMax(upper, point);
    };
    for (const auto& mesh : meshes)
    {
        const auto world = XMLoadFloat4x4(&mesh.localToWorld);
        if (mesh.instances)
        {
            const auto center = XMLoadFloat3(&mesh.localBounds.center);
            const auto half = XMLoadFloat3(&mesh.localBounds.halfExtent);
            for (const auto& instance : *mesh.instances)
            {
                const auto transform = instance.matrix() * world;
                for (unsigned corner = 0; corner < 8; ++corner)
                {
                    const auto signs = XMVectorSet(corner & 1 ? 1.0f : -1.0f,
                                                   corner & 2 ? 1.0f : -1.0f,
                                                   corner & 4 ? 1.0f : -1.0f, 0.0f);
                    include(XMVector3TransformCoord(center + signs * half, transform));
                }
            }
            continue;
        }
        for (const auto& vertex : mesh.vertices)
        {
            include(XMVector3TransformCoord(XMLoadFloat3(&vertex.position), world));
        }
    }
    Bounds bounds;
    XMStoreFloat3(&bounds.center, (lower + upper) * 0.5f);
    XMStoreFloat3(&bounds.halfExtent, (upper - lower) * 0.5f);
    return bounds;
}

auto loadBranchVoxelRegions(const ViewerScene& scene, const std::filesystem::path& file, const std::filesystem::path& mask)
{
    const auto& voxels = *scene.branchVoxels[automaticBranchVoxelIndex];
    const auto& normals = *scene.branchVoxelNormals[automaticBranchVoxelIndex];
    const auto owners = readVoxelRegionOwners(file, sourceFingerprint(mask), sourceFingerprint(fixedLodPath(scene.source)),
                                              occupiedVoxelCount(voxels));
    auto regions = std::make_shared<std::array<BranchVoxelRegion, voxelBranchRegionCount>>();
    for (auto& region : *regions)
    {
        region.foliage = {voxels.sourceFingerprint, voxels.meshIndex, voxels.origin, voxels.cellSize, {}};
        region.normals.hasDirectionalWeights = normals.hasDirectionalWeights;
        region.normals.hasShadowReceivers = normals.hasShadowReceivers;
    }
    std::size_t cell = 0;
    for (const auto& brick : voxels.bricks)
    {
        std::array<std::uint64_t, voxelBranchRegionCount> masks{};
        for (auto bits = brick.occupancy; bits; bits &= bits - 1)
        {
            const auto owner = owners[cell];
            masks[owner] |= std::uint64_t{1} << std::countr_zero(bits);
            (*regions)[owner].normals.cells.push_back(normals.cells[cell++]);
        }
        for (std::size_t i = 0; i < regions->size(); ++i)
            if (masks[i]) (*regions)[i].foliage.bricks.push_back({brick.coordinate, masks[i]});
    }
    for (const auto& region : *regions)
        if (region.foliage.bricks.empty()) throw std::runtime_error("The region bake contains an empty voxel region.");
    return regions;
}
}

const char* sceneName(SceneKind kind)
{
    constexpr std::array names{"Cube", "Card Oak", "Modeled Oak", "Modeled Oak Walk", "Grove", "Oak Branch", "Oak Branch Grid", "Oak Branch Orbit",
                               "Oak Branch Transition", "Forked White Oak", "Imported GLB"};
    static_assert(names.size() == sceneKinds.size());
    return names[static_cast<std::size_t>(kind)];
}

std::optional<SceneKind> findSceneKind(std::wstring_view name)
{
    constexpr std::array<std::wstring_view, sceneKinds.size() - 1> names{
        L"cube", L"oak", L"modeled-oak", L"oak-walk", L"grove", L"branch", L"branch-grid",
        L"branch-orbit", L"branch-transition", L"forked-oak"
    };
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) return std::nullopt;
    return sceneKinds[static_cast<std::size_t>(found - names.begin())];
}

float windPhaseAt(double seconds)
{
    constexpr double cycleSeconds = 6.0;
    return static_cast<float>(std::fmod(seconds, cycleSeconds) * (XM_2PI / cycleSeconds));
}

const char* ViewerScene::name() const
{
    return sceneName(kind);
}

unsigned ViewerScene::instanceCount() const
{
    return kind == SceneKind::Grove ? treeCount : static_cast<unsigned>(objects.size());
}

bool ViewerScene::hasAutomaticBranchVoxels() const
{
    return fixedLod && fixedLod->levels.size() == fixedLodLevelCount &&
           branchVoxelCoverageOptimized[automaticBranchVoxelIndex] && branchVoxels[automaticBranchVoxelIndex] &&
           branchVoxelNormals[automaticBranchVoxelIndex];
}

void ViewerScene::setBranchGrid(int side)
{
    objects.clear();
    branchLodSelection.groupLevels.clear();
    branchLodSelection.voxelCut.clear();
    const int radius = side / 2;
    for (int z = -radius; z <= radius; ++z)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            const auto id = static_cast<unsigned>(objects.size()) + 1;
            objects.push_back({id, {11.0f * x, 11.0f * z, 0.51f * (3 * x + 2 * z), 1.0f}});
        }
    }
    ground = makeGround(*this);
}

void ViewerScene::setForkedOakCount(unsigned count)
{
    const auto side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const float spacing = 2.0f * std::max(assetBounds.halfExtent.x, assetBounds.halfExtent.z) + 2.0f;
    objects.clear();
    objects.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
        const int column = static_cast<int>(i) % side - side / 2;
        const int row = static_cast<int>(i) / side - side / 2;
        const float scale = 1.0f + 0.04f * static_cast<float>((i * 7) % 5) - 0.08f;
        const bool center = column == 0 && row == 0;
        objects.push_back({i + 1, {spacing * column, spacing * row, center ? 0.0f : 1.25f * (i % 5),
                                    center ? 1.0f : scale}});
    }
    ground = makeGround(*this);
}

XMMATRIX SceneObject::placementMatrix() const
{
    const auto& p = placement;
    return XMMatrixScaling(p.w, p.w, p.w) * XMMatrixRotationY(p.z) * XMMatrixTranslation(p.x, 0.0f, p.y);
}

XMFLOAT3 CameraView::eye() const
{
    const float horizontal = distance * std::cos(pitch);
    return {focus.x + horizontal * std::sin(yaw),
            focus.y + distance * std::sin(pitch),
            focus.z + horizontal * std::cos(yaw)};
}

XMFLOAT4X4 CameraView::matrix() const
{
    const auto position = eye();
    XMFLOAT4X4 result;
    XMStoreFloat4x4(&result, XMMatrixLookAtRH(XMLoadFloat3(&position), XMLoadFloat3(&focus),
                                             XMVectorSet(0, 1, 0, 0)));
    return result;
}

CameraView oakWalkView(OakWalkRoute route, float seconds)
{
    const float elapsed = std::clamp(seconds, 0.0f, oakWalkDuration(route));
    if (route == OakWalkRoute::Circle)
    {
        const float yaw = elapsed == oakWalkDuration(route) ? 0.0f : elapsed * oakWalkSpeed / oakWalkRadius;
        return {{0.0f, oakWalkEyeHeight, 0.0f}, yaw, 0.0f, oakWalkRadius};
    }

    const float distance = oakWalkStart - elapsed * oakWalkSpeed;
    const auto glance = [elapsed](float start, float duration) {
        const float phase = std::clamp((elapsed - start) / duration, 0.0f, 1.0f);
        const float s = std::sin(XM_PI * phase);
        return s * s;
    };
    const float yaw = XMConvertToRadians(31.0f) * (glance(18.0f, 7.0f) - glance(26.0f, 7.0f));
    // CameraView uses the backward axis, so place its look point one unit ahead of the eye.
    return {{-std::sin(yaw), oakWalkEyeHeight, distance - std::cos(yaw)}, yaw, 0.0f, 1.0f};
}

XMFLOAT3 ViewerScene::sunDirection() const
{
    const float azimuth = XMConvertToRadians(sunAzimuth);
    const float elevation = XMConvertToRadians(sunElevation);
    const float horizontal = std::cos(elevation);
    return {horizontal * std::sin(azimuth), std::sin(elevation), horizontal * std::cos(azimuth)};
}

CameraView ViewerScene::cameraView(CameraPreset preset) const
{
    const auto& bounds = cameraBounds ? *cameraBounds : assetBounds;
    const float radius = XMVectorGetX(XMVector3Length(XMLoadFloat3(&bounds.halfExtent)));
    const bool imported = kind == SceneKind::Imported;
    const float distance = std::max(imported ? minimumOrbitDistance / 0.65f : 5.4f, radius * 2.9f);
    const auto focus = imported || distance > 5.4f ? bounds.center : XMFLOAT3{0.0f, 0.75f, 0.0f};
    if (kind == SceneKind::ForkedOak && preset == CameraPreset::Overview)
    {
        return {focus, XMConvertToRadians(132.0f), XMConvertToRadians(-5.0f), radius * 2.6f};
    }
    if (kind == SceneKind::ForkedOak && preset == CameraPreset::Distant)
    {
        return {focus, XMConvertToRadians(132.0f), XMConvertToRadians(-2.235f), 100.0f};
    }
    if (kind == SceneKind::OakBranchGrid && (preset == CameraPreset::Overview || preset == CameraPreset::Distant))
    {
        const float gridDistance = preset == CameraPreset::Overview ? 42.0f : 120.0f;
        return {focus, XMConvertToRadians(-125.0f), XMConvertToRadians(68.0f), gridDistance};
    }
    switch (preset)
    {
    case CameraPreset::CloseUp:
        return {focus, -0.28f, 0.20f, distance * 0.65f};
    case CameraPreset::UnderCanopy:
        return {{focus.x, focus.y * 0.55f, focus.z}, -0.65f, -0.15f, distance * 0.55f};
    case CameraPreset::Distant:
        return {focus, -0.28f, 0.4f, 300.0f};
    default:
        return {focus, -0.28f, 0.4f, kind == SceneKind::Grove ? 112.0f : distance};
    }
}

CameraView ViewerScene::branchOrbitView(float elapsedSeconds, float distance) const
{
    struct Angle { float yaw, pitch; };
    constexpr Angle path[]{{-125, 68}, {-60, 34}, {32, 8}, {71, 61}, {145, 24}, {235, 68}};
    const float fraction = std::clamp(elapsedSeconds / branchOrbitDuration, 0.0f, 1.0f);
    const float yaw = std::lerp(path[0].yaw, path[std::size(path) - 1].yaw, fraction);
    const auto* end = std::end(path);
    const auto* next = std::upper_bound(std::begin(path) + 1, end - 1, yaw,
                                       [](float angle, const Angle& key) { return angle < key.yaw; });
    const auto& from = next[-1];
    const auto& to = *next;
    const float t = (yaw - from.yaw) / (to.yaw - from.yaw);
    const float pitch = std::lerp(from.pitch, to.pitch, t * t * (3.0f - 2.0f * t));
    const float viewYaw = fraction == 1.0f ? path[0].yaw : yaw;
    return {assetBounds.center, XMConvertToRadians(viewYaw), XMConvertToRadians(pitch), distance};
}

float ViewerScene::farPlane() const
{
    const float overviewFar = std::max({100.0f, minimumFarPlane, cameraView(CameraPreset::Overview).distance * 10.0f});
    if (isOakBranchScene(kind)) return std::max(overviewFar, cameraView(CameraPreset::Distant).distance * 1.1f);
    return overviewFar;
}

ViewerScenes::ViewerScenes(std::filesystem::path assetDirectory)
    : assetDirectory_(std::move(assetDirectory))
{
}

std::filesystem::path ViewerScenes::sourcePath(SceneKind kind, const std::filesystem::path& importedPath) const
{
    if (kind == SceneKind::Imported) return importedPath;
    if (kind == SceneKind::Cube) return assetDirectory_ / L"cube.glb";
    if (kind == SceneKind::ForkedOak) return assetDirectory_ / L"WhiteOak-ForkedCanopy.glb";
    if (isOakBranchScene(kind)) return assetDirectory_ / L"LowBranchedWhiteOak-SingleBranch.glb";
    if (kind == SceneKind::ModeledOak || kind == SceneKind::ModeledOakWalk)
        return assetDirectory_ / L"LowBranchedWhiteOak-Modeled.glb";
    return assetDirectory_ / L"LowBranchedWhiteOakLOD0.glb";
}

ViewerScene& ViewerScenes::open(SceneKind kind, const std::filesystem::path& importedPath)
{
    auto& entry = scenes_[static_cast<std::size_t>(kind)];
    if (entry)
    {
        return *entry;
    }

    ViewerScene scene{};
    scene.kind = kind;
    const bool usesOak = kind == SceneKind::Oak || kind == SceneKind::Grove;
    const bool usesModeledOak = kind == SceneKind::ModeledOak || kind == SceneKind::ModeledOakWalk;
    scene.source = sourcePath(kind, importedPath);
    if (isOakBranchScene(kind) && kind != SceneKind::OakBranch)
    {
        const auto& branch = open(SceneKind::OakBranch);
        scene.asset = branch.asset;
        scene.fixedLod = branch.fixedLod;
        scene.fixedLodStatus = branch.fixedLodStatus;
        scene.branchVoxels = branch.branchVoxels;
        scene.branchVoxelNormals = branch.branchVoxelNormals;
        scene.branchVoxelCoverageOptimized = branch.branchVoxelCoverageOptimized;
        scene.branchVoxelRegions = branch.branchVoxelRegions;
        scene.branchVoxelStatus = branch.branchVoxelStatus;
        scene.assetBounds = branch.assetBounds;
        if (kind == SceneKind::OakBranchGrid && scene.fixedLod && scene.fixedLod->levels.size() == fixedLodLevelCount)
        {
            scene.branchLod = automaticBranchLod;
            scene.lodTint = FixedLodTint::Level;
        }
    }
    else if (usesOak && oak_)
    {
        scene.asset = oak_;
    }
    else if (usesModeledOak && modeledOak_)
    {
        scene.asset = modeledOak_;
    }
    else
    {
        scene.asset = std::make_shared<AssetData>(loadGlb(scene.source));
        if (usesOak)
        {
            oak_ = scene.asset;
        }
        if (usesModeledOak) modeledOak_ = scene.asset;
    }
    scene.meshShadows.resize(scene.asset->meshes.size());
    if (kind == SceneKind::OakBranch || kind == SceneKind::ModeledOakWalk)
    {
        try
        {
            scene.fixedLod = std::make_shared<FixedLod>(readFixedLod(scene.source, *scene.asset));
        }
        catch (const std::runtime_error& error)
        {
            scene.fixedLodStatus = error.what();
        }
        if (kind == SceneKind::OakBranch && scene.fixedLod && scene.fixedLod->levels.size() == fixedLodLevelCount)
        {
            const auto fingerprint = fixedLodSourceFingerprint(scene.source);
            constexpr std::array<const wchar_t*, branchVoxelBakeCount> extensions{
                L".fine.voxels", L".coarse.voxels", L".sixteenth.voxels", L".eighth.voxels", L".quarter.voxels",
                L".half.voxels"};
            for (std::size_t i = 0; i < extensions.size(); ++i)
            {
                auto path = scene.source;
                path.replace_extension(extensions[i]);
                const auto reportBakeError = [&](const std::filesystem::path& candidate, const std::runtime_error& error) {
                    if (!scene.branchVoxelStatus.empty()) scene.branchVoxelStatus += '\n';
                    scene.branchVoxelStatus += candidate.filename().string() + " " + error.what();
                };
                const auto loadBake = [&](const std::filesystem::path& candidate) {
                    try
                    {
                        auto data = readVoxelFoliage(candidate);
                        if (data.sourceFingerprint != fingerprint || data.meshIndex != scene.fixedLod->meshIndex || data.bricks.empty())
                            throw std::runtime_error("Voxel foliage bake does not match the branch mesh.");
                        scene.branchVoxels[i] = std::make_shared<const VoxelFoliage>(std::move(data));
                        return true;
                    }
                    catch (const std::runtime_error& error)
                    {
                        reportBakeError(candidate, error);
                        return false;
                    }
                };
                auto fitted = path;
                fitted.replace_extension(L".coverage.voxels");
                if (std::filesystem::exists(fitted) && loadBake(fitted))
                    scene.branchVoxelCoverageOptimized[i] = true;
                else
                    loadBake(path);
                auto normalPath = scene.branchVoxelCoverageOptimized[i] ? fitted : path;
                normalPath.replace_extension(L".normals");
                if (!scene.branchVoxels[i] || !std::filesystem::exists(normalPath)) continue;
                try
                {
                    const auto& mask = scene.branchVoxelCoverageOptimized[i] ? fitted : path;
                    auto normals = readVoxelNormals(normalPath, sourceFingerprint(mask), occupiedVoxelCount(*scene.branchVoxels[i]));
                    scene.branchVoxelNormals[i] = std::make_shared<const VoxelNormalData>(std::move(normals));
                }
                catch (const std::runtime_error& error)
                {
                    reportBakeError(normalPath, error);
                }
            }
            auto regionPath = scene.source;
            regionPath.replace_extension(L".eighth.coverage.regions");
            if (scene.hasAutomaticBranchVoxels() && scene.fixedLod->groups.size() >= voxelBranchRegionCount &&
                std::filesystem::exists(regionPath))
            {
                auto mask = scene.source;
                mask.replace_extension(L".eighth.coverage.voxels");
                try { scene.branchVoxelRegions = loadBranchVoxelRegions(scene, regionPath, mask); }
                catch (const std::runtime_error& error)
                {
                    if (!scene.branchVoxelStatus.empty()) scene.branchVoxelStatus += '\n';
                    scene.branchVoxelStatus += regionPath.filename().string() + " " + error.what();
                }
            }
        }
    }
    if (!isOakBranchScene(kind) || kind == SceneKind::OakBranch) scene.assetBounds = assetBounds(scene.asset->meshes);
    if (usesOak)
    {
        cardOakBounds_ = scene.assetBounds;
    }
    if (kind == SceneKind::ModeledOak)
    {
        if (!cardOakBounds_)
        {
            const auto cardOak = loadGlb(sourcePath(SceneKind::Oak, {}));
            cardOakBounds_ = assetBounds(cardOak.meshes);
        }
        scene.cameraBounds = cardOakBounds_;
    }
    scene.foliage = std::any_of(scene.asset->materials.begin(), scene.asset->materials.end(),
                                [](const MaterialData& material) { return material.hasBacklighting; });
    if (kind == SceneKind::ForkedOak)
    {
        try
        {
            const auto sources = sprayBoughSources(scene.source);
            for (std::size_t part = 0; part < scene.sprayLods.size(); ++part)
                scene.sprayLods[part] = std::make_shared<FixedLod>(readFixedLod(scene.source, sources[part + 1], *scene.asset));
            const auto leaves = scene.sprayLods[0]->meshIndex;
            const auto wood = scene.sprayLods[1]->meshIndex;
            if (leaves == wood || !scene.asset->meshes[leaves].instances ||
                scene.asset->meshes[leaves].instances != scene.asset->meshes[wood].instances)
                throw std::runtime_error("Spray bakes must reference the two primitives of the same instanced node.");
            scene.branchLod = automaticBranchLod;
        }
        catch (const std::runtime_error& error)
        {
            scene.sprayLods = {};
            scene.sprayLodStatus = error.what();
        }
        if (scene.sprayLods[0])
        {
            try
            {
                scene.sprayBough = std::make_shared<SprayBough>(
                    readSprayBough(sprayBoughSources(scene.source), *scene.asset, scene.sprayLods));
                scene.sprayTraversal = SprayTraversal::BoughProxy;
            }
            catch (const std::runtime_error& error)
            {
                scene.sprayBoughStatus = error.what();
            }
            try
            {
                scene.treeHierarchy = std::make_shared<TreeHierarchy>(
                    readTreeHierarchy(sprayBoughSources(scene.source), *scene.asset));
                scene.sprayTraversal = SprayTraversal::Hierarchy;
            }
            catch (const std::runtime_error& error)
            {
                scene.treeHierarchyStatus = error.what();
            }
        }
        scene.sunAzimuth = 102.0f;
        scene.sunElevation = 49.0f;
        scene.probeLighting = ProbeLightingMode::GroundBounce;
        scene.windStrength = 0.0f;
        scene.windPaused = true;
    }
    if (kind == SceneKind::OakBranchGrid)
    {
        scene.sunAzimuth = -100.0f;
        scene.windPaused = true;
        scene.setBranchGrid(branchGridSide);
        scene.inspectedLodInstance = static_cast<int>(scene.objects.size() / 2);
    }
    else
    {
        scene.objects.push_back({1, {0.0f, 0.0f, 0.0f, 1.0f}});
        if (kind == SceneKind::Grove) populateGrove(scene);
        scene.ground = makeGround(scene);
    }
    if (isBranchOrbitScene(kind))
    {
        scene.sunAzimuth = -100.0f;
        scene.windPaused = true;
        scene.branchVoxelView = scene.fixedLod && scene.fixedLod->levels.size() == fixedLodLevelCount
            ? BranchVoxelView::Lod6 : BranchVoxelView::Off;
        scene.branchAppearance = BranchAppearance::Coverage;
    }
    if (kind == SceneKind::OakBranchTransition)
    {
        if (scene.branchVoxelRegions) scene.branchVoxelView = BranchVoxelView::Regional;
        else if (scene.hasAutomaticBranchVoxels()) scene.branchVoxelView = BranchVoxelView::Automatic;
        scene.branchAppearance = BranchAppearance::Shaded;
        scene.branchLod = automaticBranchLod;
        scene.branchVoxelUseNormals = true;
        scene.branchFrustumCulling = true;
    }
    if (kind == SceneKind::ModeledOakWalk)
    {
        scene.sunAzimuth = -100.0f;
        scene.windStrength = 0.0f;
        scene.windPaused = true;
        scene.branchFrustumCulling = true;
        scene.branchGroupFrustumCulling = true;
        if (scene.fixedLod && scene.fixedLod->levels.size() == fixedLodLevelCount)
        {
            scene.branchLod = automaticBranchLod;
            scene.branchShadowLod = static_cast<int>(fixedLodFarLevel);
        }
    }
    scene.groundMaterial.name = "Ground";
    scene.groundMaterial.color = {0.30f, 0.36f, 0.30f, 1.0f};
    return entry.emplace(std::move(scene));
}
