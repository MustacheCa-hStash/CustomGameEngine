#pragma once

#include "FixedLod.h"
#include "SprayBough.h"
#include "TreeHierarchy.h"
#include "VoxelFoliage.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

enum class SceneKind { Cube, Oak, ModeledOak, ModeledOakWalk, Grove, OakBranch, OakBranchGrid, OakBranchOrbit, OakBranchTransition, ForkedOak, Imported };
enum class CaptureLighting : std::uint32_t { Combined, SunOnly, SkyOnly, Transmission, ShadowVisibility };
enum class ProbeLightingMode : std::uint32_t { Off, SkyVisibility, GroundBounce };
inline constexpr std::array sceneKinds{SceneKind::Cube, SceneKind::Oak, SceneKind::ModeledOak, SceneKind::ModeledOakWalk, SceneKind::Grove,
                                      SceneKind::OakBranch, SceneKind::OakBranchGrid, SceneKind::OakBranchOrbit,
                                      SceneKind::OakBranchTransition, SceneKind::ForkedOak, SceneKind::Imported};
inline constexpr bool isBranchOrbitScene(SceneKind kind)
{
    return kind == SceneKind::OakBranchOrbit || kind == SceneKind::OakBranchTransition;
}
inline constexpr bool isOakBranchScene(SceneKind kind)
{
    return kind == SceneKind::OakBranch || kind == SceneKind::OakBranchGrid || isBranchOrbitScene(kind);
}
const char* sceneName(SceneKind kind);
std::optional<SceneKind> findSceneKind(std::wstring_view name);
enum class CameraPreset { Overview, CloseUp, UnderCanopy, Distant };
enum class OakWalkRoute { Approach, Circle };
inline constexpr std::array sunCascadeSplits{16.0f, 48.0f, 120.0f};
inline constexpr std::array sunCascadeBlendWidths{2.0f, 4.0f, 8.0f};
inline constexpr float sunCascadeProxyErrorTexels = 0.5f;

inline constexpr std::array<unsigned, 4> groveCounts{1, 25, 100, 400};
inline constexpr int branchGridSide = 5;
inline constexpr float maxWindStrength = 2.0f;
inline constexpr float branchOrbitDuration = 30.0f;
inline constexpr float branchOrbitDistance = 700.0f;
inline constexpr float oakWalkEyeHeight = 1.88f;
inline constexpr float oakWalkSpeed = 1.5f;
inline constexpr float oakWalkStart = 55.5f;
inline constexpr float oakWalkEnd = 3.0f;
inline constexpr float oakWalkRadius = 8.0f;
inline constexpr float oakWalkDuration(OakWalkRoute route)
{
    return route == OakWalkRoute::Approach ? (oakWalkStart - oakWalkEnd) / oakWalkSpeed :
                                          DirectX::XM_2PI * oakWalkRadius / oakWalkSpeed;
}

struct CameraView
{
    DirectX::XMFLOAT3 focus;
    float yaw;
    float pitch;
    float distance;

    DirectX::XMFLOAT3 eye() const;
    DirectX::XMFLOAT4X4 matrix() const;
};
CameraView oakWalkView(OakWalkRoute route, float seconds);
float windPhaseAt(double seconds);

struct SceneObject
{
    unsigned id;
    // X/Z translation, yaw in radians, and uniform scale.
    DirectX::XMFLOAT4 placement;

    DirectX::XMMATRIX placementMatrix() const;
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

inline constexpr int sourceBranchLod = -1;
inline constexpr int automaticBranchLod = -2;
inline constexpr int matchCameraShadowLod = -1;

enum class SprayTraversal { Flat, Grouped, BoughProxy, Hierarchy };
enum class TreeSubmission { Direct, CpuIndirect, GpuPerPart, GpuBatched };

enum class BranchVoxelView
{
    Off, Source, Lod6, Fine, Coarse, CellOneSixteenth, CellOneEighth, CellOneQuarter, CellOneHalf, Automatic, Regional, Count
};
inline constexpr auto branchVoxelBakeCount = static_cast<std::size_t>(BranchVoxelView::Automatic) -
                                             static_cast<std::size_t>(BranchVoxelView::Fine);
enum class BranchAppearance { Shaded, Coverage, BaseColor, Diffuse, ReceivedShadows, CoverageMask, Representation };
enum class VoxelShadowReceiver : std::uint32_t { CellEntry, BakedLeaf, CellMidpoint };
inline constexpr std::array branchVoxelFarDistances{300.0f, 600.0f, 1200.0f, 1800.0f, 2400.0f, 3000.0f};
inline constexpr float branchVoxelFarNearPlane = 20.0f;
inline constexpr float minimumOrbitDistance = 0.02f;
inline constexpr float branchVoxelFarClipPlane = 3600.0f;

inline constexpr std::optional<std::size_t> branchVoxelIndex(BranchVoxelView view)
{
    if (view >= BranchVoxelView::Fine && view < BranchVoxelView::Automatic)
        return static_cast<std::size_t>(view) - static_cast<std::size_t>(BranchVoxelView::Fine);
    return std::nullopt;
}
inline constexpr auto automaticBranchVoxelIndex = *branchVoxelIndex(BranchVoxelView::CellOneEighth);
inline constexpr float branchVoxelDefaultEntryPixels = 0.373f;
inline constexpr float branchVoxelExitFactor = 1.05f;
inline constexpr std::uint8_t allVoxelRegions = (1u << voxelBranchRegionCount) - 1;
inline constexpr bool automaticBranchVoxels(BranchVoxelView view)
{
    return view == BranchVoxelView::Automatic || view == BranchVoxelView::Regional;
}

struct BranchVoxelRegion
{
    VoxelFoliage foliage;
    VoxelNormalData normals;
};

struct BranchLodSelection
{
    struct TestCounts
    {
        unsigned instances = 0;
        unsigned acceptedInstances = 0;
        std::size_t groups = 0;
    };

    static constexpr float defaultFarPixelLimit = 1.7f;
    static constexpr float defaultProxyPixelLimit = 1.2f;
    float farPixelLimit = defaultFarPixelLimit;
    float proxyPixelLimit = defaultProxyPixelLimit;
    float nearPixelLimit = 1.0f;
    bool frozen = false;
    bool indirectDraws = true;
    bool useInstanceBounds = true;
    std::vector<std::size_t> groupLevels;
    std::vector<std::uint8_t> voxelCut;
    BranchVoxelView voxelCutMode = BranchVoxelView::Off;
    TestCounts tests;
};

struct ViewerScene
{
    SceneKind kind;
    std::filesystem::path source;
    std::shared_ptr<AssetData> asset;
    std::vector<SceneObject> objects;
    Bounds assetBounds;
    std::optional<Bounds> cameraBounds;
    float nearPlane = 0.05f;
    float minimumFarPlane = 0;
    MeshData ground;
    MaterialData groundMaterial;
    ShadowBiasSettings shadowBias;
    std::vector<MeshShadows> meshShadows;
    MeshShadows groundShadows{false, true};
    float sunAzimuth = -38.6598f;
    float sunElevation = 54.5715f;
    static constexpr float minExposureEv = -8.0f;
    static constexpr float maxExposureEv = 8.0f;
    float exposureEv = 0.0f;
    ProbeLightingMode probeLighting = ProbeLightingMode::Off;
    static constexpr unsigned probeWindMaxInterval = 8;
    float probeWindRefreshVoxels = 0.125f;
    float windStrength = 1.0f;
    bool windPaused = false;
    bool foliage = false;
    OakWalkRoute oakWalkRoute = OakWalkRoute::Approach;
    unsigned treeCount = 100;
    std::shared_ptr<const FixedLod> fixedLod;
    std::string fixedLodStatus;
    std::array<std::shared_ptr<const FixedLod>, 2> sprayLods;
    std::string sprayLodStatus;
    std::shared_ptr<const SprayBough> sprayBough;
    std::string sprayBoughStatus;
    std::shared_ptr<const TreeHierarchy> treeHierarchy;
    std::string treeHierarchyStatus;
    SprayTraversal sprayTraversal = SprayTraversal::Grouped;
    TreeSubmission treeSubmission = TreeSubmission::Direct;
    std::filesystem::path boughVoxelBake;
    std::filesystem::path liveCut;
    bool liveCutUnbatched = false;
    bool liveCutPackAssembly = false;
    bool cameraPipelineStats = false;
    float liveCutPixelError = 1.0f;
    bool treeOcclusion = false;
    bool treeClusterCulling = false;
    bool treeBatchOcclusion = false;
    bool sprayLocalLod = true;
    bool leafGroupBounds = false;
    bool sprayFrustumCulling = true;
    float sprayPixelLimit = 1.0f;
    float sprayTemplatePixelLimit = 1.0f;
    bool cullSprayWoodBackfaces = false;
    std::array<std::shared_ptr<const VoxelFoliage>, branchVoxelBakeCount> branchVoxels;
    std::array<std::shared_ptr<const VoxelNormalData>, branchVoxelBakeCount> branchVoxelNormals;
    std::array<bool, branchVoxelBakeCount> branchVoxelCoverageOptimized{};
    std::shared_ptr<const std::array<BranchVoxelRegion, voxelBranchRegionCount>> branchVoxelRegions;
    std::string branchVoxelStatus;
    BranchVoxelView branchVoxelView = BranchVoxelView::Off;
    float branchVoxelEnterPixels = branchVoxelDefaultEntryPixels;
    BranchAppearance branchAppearance = BranchAppearance::Shaded;
    bool branchVoxelUseNormals = false;
    bool branchVoxelDirectionalNormals = false;
    VoxelShadowReceiver branchVoxelShadowReceiver = VoxelShadowReceiver::CellEntry;
    float branchVoxelShadowOffsetTexels = 0;
    bool branchVoxelDepthCapture = false;
    bool branchVoxelTightProxies = true;
    bool diagnosticVoxelShadowCasters = false;
    int branchLod = sourceBranchLod;
    BranchLodSelection branchLodSelection;
    bool branchFrustumCulling = false;
    bool branchGroupFrustumCulling = false;
    int branchShadowLod = matchCameraShadowLod;
    bool shadowLodOffCameraOnly = false;
    bool showShadowFactor = false;
    bool captureGeometryMask = false;
    bool captureBoughMaterials = false;
    bool captureBoughDepth = false;
    CaptureLighting captureLighting = CaptureLighting::Combined;
    bool legacyAmbientForDiagnostics = false;
    bool captureForceBough = false;
    bool useSunCascades = false;
    bool stabilizeSunCascades = true;
    bool blendSunCascades = true;
    bool filterSunCascades = true;
    bool cullSunCascadeCasters = true;
    bool coarsenSunCascadeFoliage = false;
    bool showCascadeColors = false;
    FixedLodTint lodTint = FixedLodTint::Shaded;
    int inspectedLodGroup = 0;
    int inspectedLodInstance = 0;
    bool highlightLodGroup = false;

    const char* name() const;
    unsigned instanceCount() const;
    bool hasAutomaticBranchVoxels() const;
    bool usesSunCascades() const { return useSunCascades && branchVoxelView == BranchVoxelView::Off; }
    bool canCoarsenCascadeFoliage() const
    {
        return kind == SceneKind::ModeledOakWalk && fixedLod && fixedLod->levels.size() == fixedLodLevelCount &&
               branchShadowLod == static_cast<int>(fixedLodFarLevel) && !shadowLodOffCameraOnly;
    }
    bool usesCascadeCasterLists() const
    {
        return usesSunCascades() && kind == SceneKind::ModeledOakWalk &&
               (cullSunCascadeCasters || (coarsenSunCascadeFoliage && canCoarsenCascadeFoliage()));
    }
    void setBranchGrid(int side);
    void setForkedOakCount(unsigned count);
    DirectX::XMFLOAT3 sunDirection() const;
    CameraView cameraView(CameraPreset preset) const;
    CameraView branchOrbitView(float elapsedSeconds, float distance) const;
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
    std::shared_ptr<AssetData> modeledOak_;
    std::optional<Bounds> cardOakBounds_;
};

