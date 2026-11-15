#include "ViewerScene.h"

#include "GltfLoader.h"

#include <algorithm>
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
    for (const auto& mesh : meshes)
    {
        const auto world = XMLoadFloat4x4(&mesh.localToWorld);
        for (const auto& vertex : mesh.vertices)
        {
            const auto position = XMVector3TransformCoord(XMLoadFloat3(&vertex.position), world);
            lower = XMVectorMin(lower, position);
            upper = XMVectorMax(upper, position);
        }
    }
    Bounds bounds;
    XMStoreFloat3(&bounds.center, (lower + upper) * 0.5f);
    XMStoreFloat3(&bounds.halfExtent, (upper - lower) * 0.5f);
    return bounds;
}
}

const char* sceneName(SceneKind kind)
{
    constexpr std::array names{"Cube", "Card Oak", "Modeled Oak", "Grove", "Oak Branch", "Oak Branch Grid", "Imported GLB"};
    static_assert(names.size() == sceneKinds.size());
    return names[static_cast<std::size_t>(kind)];
}

const char* ViewerScene::name() const
{
    return sceneName(kind);
}

unsigned ViewerScene::instanceCount() const
{
    return kind == SceneKind::Grove ? treeCount : static_cast<unsigned>(objects.size());
}

void ViewerScene::setBranchGrid(int side)
{
    objects.clear();
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
    const float distance = std::max(5.4f, radius * 2.9f);
    const auto focus = distance > 5.4f ? bounds.center : XMFLOAT3{0.0f, 0.75f, 0.0f};
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

float ViewerScene::farPlane() const
{
    const float overviewFar = std::max(100.0f, cameraView(CameraPreset::Overview).distance * 10.0f);
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
    if (isOakBranchScene(kind)) return assetDirectory_ / L"LowBranchedWhiteOak-SingleBranch.glb";
    if (kind == SceneKind::ModeledOak) return assetDirectory_ / L"LowBranchedWhiteOak-Modeled.glb";
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
    scene.source = sourcePath(kind, importedPath);
    if (kind == SceneKind::OakBranchGrid)
    {
        const auto& branch = open(SceneKind::OakBranch);
        scene.asset = branch.asset;
        scene.fixedLod = branch.fixedLod;
        scene.fixedLodStatus = branch.fixedLodStatus;
        scene.assetBounds = branch.assetBounds;
        if (scene.fixedLod && scene.fixedLod->levels.size() == fixedLodLevelCount)
        {
            scene.branchLod = automaticBranchLod;
            scene.lodTint = FixedLodTint::Level;
        }
    }
    else if (usesOak && oak_)
    {
        scene.asset = oak_;
    }
    else
    {
        scene.asset = std::make_shared<AssetData>(loadGlb(scene.source));
        if (usesOak)
        {
            oak_ = scene.asset;
        }
    }
    scene.meshShadows.resize(scene.asset->meshes.size());
    if (kind == SceneKind::OakBranch)
    {
        try
        {
            scene.fixedLod = std::make_shared<FixedLod>(readFixedLod(scene.source, *scene.asset));
        }
        catch (const std::runtime_error& error)
        {
            scene.fixedLodStatus = error.what();
        }
    }
    if (kind != SceneKind::OakBranchGrid) scene.assetBounds = assetBounds(scene.asset->meshes);
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
    scene.groundMaterial.name = "Ground";
    scene.groundMaterial.color = {0.30f, 0.36f, 0.30f, 1.0f};
    return entry.emplace(std::move(scene));
}
