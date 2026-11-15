#pragma once

#include "MeshData.h"

#include <array>
#include <filesystem>
#include <memory>
#include <span>

enum class SceneKind { Cube, Oak, ModeledOak, Grove, OakBranch, Imported };
inline constexpr std::array sceneKinds{SceneKind::Cube, SceneKind::Oak, SceneKind::ModeledOak, SceneKind::Grove,
                                      SceneKind::OakBranch, SceneKind::Imported};
const char* sceneName(SceneKind kind);
enum class CameraPreset { Overview, CloseUp, UnderCanopy };

inline constexpr std::array<unsigned, 4> groveCounts{1, 25, 100, 400};
inline constexpr float maxWindStrength = 2.0f;

struct CameraView
{
    DirectX::XMFLOAT3 focus;
    float yaw;
    float pitch;
    float distance;
};

struct SceneObject
{
    unsigned id;
    // X/Z translation, yaw in radians, and uniform scale.
    DirectX::XMFLOAT4 placement;
};

struct ShadowBiasSettings
{
    int constant = 32;
    float slopeScale = 1.0f;
    float samplingTexels = 0.5f;
    float normalTexels = 0.0f;
    static constexpr float drawClamp = 0.002f;
    static constexpr int maxConstant = 65536;
    static constexpr float maxSlopeScale = 6.0f;
    static constexpr float maxReceiverTexels = 8.0f;
};

struct MeshShadows
{
    bool cast = true;
    bool receive = true;
};

struct ViewerScene
{
    SceneKind kind;
    std::filesystem::path source;
    std::shared_ptr<AssetData> asset;
    std::vector<SceneObject> objects;
    Bounds assetBounds;
    std::optional<Bounds> cameraBounds;
    MeshData ground;
    MaterialData groundMaterial;
    ShadowBiasSettings shadowBias;
    std::vector<MeshShadows> meshShadows;
    MeshShadows groundShadows{false, true};
    float sunAzimuth = -38.6598f;
    float sunElevation = 54.5715f;
    float windStrength = 1.0f;
    bool windPaused = false;
    bool foliage = false;
    unsigned treeCount = 100;

    const char* name() const;
    DirectX::XMFLOAT3 sunDirection() const;
    CameraView cameraView(CameraPreset preset) const;
    float farPlane() const;
};

class ViewerScenes
{
public:
    explicit ViewerScenes(std::filesystem::path assetDirectory);
    std::filesystem::path sourcePath(SceneKind kind, const std::filesystem::path& importedPath) const;
    ViewerScene& open(SceneKind kind, const std::filesystem::path& importedPath = {});

private:
    std::filesystem::path assetDirectory_;
    std::array<std::optional<ViewerScene>, sceneKinds.size()> scenes_;
    std::shared_ptr<AssetData> oak_;
    std::optional<Bounds> cardOakBounds_;
};

