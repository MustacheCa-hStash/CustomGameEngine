#include "ViewerScene.h"

#include "GltfLoader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
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
    constexpr const char* names[] = {"Cube", "Card Oak", "Modeled Oak", "Grove", "Oak Branch", "Imported GLB"};
    return names[static_cast<std::size_t>(kind)];
}

const char* ViewerScene::name() const
{
    return sceneName(kind);
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
    switch (preset)
    {
    case CameraPreset::CloseUp:
        return {focus, -0.28f, 0.20f, distance * 0.65f};
    case CameraPreset::UnderCanopy:
        return {{focus.x, focus.y * 0.55f, focus.z}, -0.65f, -0.15f, distance * 0.55f};
    default:
        return {focus, -0.28f, 0.4f, kind == SceneKind::Grove ? 112.0f : distance};
    }
}

float ViewerScene::farPlane() const
{
    return std::max(100.0f, cameraView(CameraPreset::Overview).distance * 10.0f);
}

ViewerScenes::ViewerScenes(std::filesystem::path assetDirectory)
    : assetDirectory_(std::move(assetDirectory))
{
}

std::filesystem::path ViewerScenes::sourcePath(SceneKind kind, const std::filesystem::path& importedPath) const
{
    if (kind == SceneKind::Imported) return importedPath;
    if (kind == SceneKind::Cube) return assetDirectory_ / L"cube.glb";
    if (kind == SceneKind::OakBranch) return assetDirectory_ / L"LowBranchedWhiteOak-SingleBranch.glb";
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
    if (usesOak && oak_)
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
    scene.assetBounds = assetBounds(scene.asset->meshes);
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
    scene.objects.push_back({1, {0.0f, 0.0f, 0.0f, 1.0f}});
    if (kind == SceneKind::Grove)
    {
        populateGrove(scene);
    }
    scene.ground = makeGround(scene);
    scene.groundMaterial.name = "Ground";
    scene.groundMaterial.color = {0.30f, 0.36f, 0.30f, 1.0f};
    return entry.emplace(std::move(scene));
}
