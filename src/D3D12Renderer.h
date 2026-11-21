#pragma once

#include "ShaderCompiler.h"
#include "ProbeLighting.h"
#include "ViewerScene.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

class D3D12Renderer
{
public:
    struct PassStats
    {
        unsigned draws = 0;
        unsigned foliageDraws = 0;
        std::uint64_t triangles = 0;
        std::uint64_t foliageTriangles = 0;
        unsigned foliageCalls = 0;
        unsigned visibleInstances = 0;
        std::size_t foliageGroupsConsidered = 0;
        std::size_t foliageGroupsVisible = 0;
        unsigned shadowLodOverrides = 0;
        std::uint64_t voxelBricks = 0;
        std::uint64_t proxyFoliageTriangles = 0;
        std::uint64_t sprayWoodTriangles = 0;
        std::array<unsigned, fixedLodLevelCount> sprayWoodByLod{};
        std::array<unsigned, fixedLodLevelCount> sprayLeavesByLod{};
        unsigned sprayPlacements = 0;
        unsigned visibleSprayPlacements = 0;
        unsigned sprayGroupTests = 0;
        unsigned sprayPlacementTests = 0;
        std::array<unsigned, 2> sprayLocalPlacements{};
        std::array<unsigned, 2> sprayLocalGroups{};
        std::array<unsigned, 2> sprayIntermediateGroups{};
        std::uint64_t boughProxyTriangles = 0;
        unsigned boughProxyPlacements = 0;
        unsigned treeNodeTests = 0;
        unsigned treeNodesDrawn = 0;
        std::array<unsigned, 2> treeProxySprays{};
        std::array<std::uint64_t, 2> treeCanopyTriangles{};
        std::uint64_t treeTrunkTriangles = 0;
        unsigned treeDraws = 0;
        unsigned treeSubmissionCalls = 0;
        double treeSubmissionMilliseconds = 0;
        double treeSelectionMilliseconds = 0;
        std::uint64_t vertexInvocations = 0, pixelInvocations = 0, inputPrimitives = 0;
        std::uint64_t treeCutHash = 0;
        unsigned gpuTreeDispatches = 0;
        unsigned treeDepthRejects = 0;
        unsigned treeBatchJobs = 0;
        unsigned treeBatchInstances = 0;
        unsigned treeClusterTests = 0;
        unsigned treeClusterRejects = 0;
        unsigned treeClusterSplits = 0;
        unsigned treeClusterFallbacks = 0;
        double treeClusterGpuMilliseconds = 0;
        unsigned treeOcclusionTests = 0;
        unsigned treeOcclusionDeferred = 0;
        unsigned treeOcclusionRejected = 0;
        unsigned treeOcclusionRecovered = 0;
        bool treeOcclusionActive = false;
        bool treeOcclusionHistory = false;
        double treeOcclusionGpuMilliseconds = 0;
        double treeOcclusionRetestMilliseconds = 0;
        double treePyramidGpuMilliseconds = 0;
        std::array<double, 4> treeBatchGpuMilliseconds{};
    };

    struct FrameTiming
    {
        double cpuMilliseconds;
        std::optional<double> gpuMilliseconds;
        unsigned drawCalls = 0;
        std::optional<double> gpuCameraMilliseconds, gpuAssetMilliseconds, gpuLiveCutMilliseconds;
        double gpuBoughBrickMilliseconds = 0;
        std::optional<double> gpuShadowMilliseconds;
        PassStats camera;
        PassStats shadow;
        std::array<PassStats, sunCascadeSplits.size()> shadowCascades;
        double cpuBranchDrawListMilliseconds = 0;
        std::optional<double> gpuProbeMilliseconds;
        unsigned probesUpdated = 0;
        unsigned probesPending = 0;
        bool probeWindAdvection = false;
        bool probeGroundUpdate = false;
    };

    explicit D3D12Renderer(HWND window, const std::filesystem::path& skyPanorama = {}, bool cullMeshBackfaces = false);
    D3D12Renderer(UINT width, UINT height, const std::filesystem::path& skyPanorama = {}, bool cullMeshBackfaces = false);

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    bool supportsMsaa() const;
    bool msaaEnabled() const;
    void toggleMsaa();
    void setScene(const ViewerScene& scene);
    void updateCameraProjection(float nearPlane, float farPlane);
    void updateOrbitCameraProjection(const ViewerScene& scene, float distance);
    void updateSun(const ViewerScene& scene);
    void reloadMeshShaders(MeshShaders shaders, const ShadowBiasSettings& bias);
    float shadowDepthPerTexel() const { return shadowDepthPerTexel_; }
    float shadowWorldUnitsPerTexel() const { return shadowWorldUnitsPerTexel_; }
    const auto& sunCascadeTexelWidths() const { return cascadeWorldUnitsPerTexel_; }
    std::uint64_t probeBytes() const;
    unsigned probeCount() const;
    static constexpr float cameraVerticalFov = DirectX::XM_PIDIV4;
    static constexpr UINT defaultRenderWidth = 3840;
    static constexpr UINT defaultRenderHeight = 2160;
    DirectX::XMFLOAT2 viewportSize() const { return {viewport_.Width, viewport_.Height}; }
    float projectLodError(float localError, const Bounds& bounds,
                          const DirectX::XMFLOAT4X4& localToWorld, const DirectX::XMFLOAT4X4& view,
                          DirectX::XMFLOAT2 worldWind = {}) const;
    float projectShadowError(float localError, const DirectX::XMFLOAT4X4& localToWorld) const;
    struct BranchLodErrors { float farPixels; float proxyPixels; };
    BranchLodErrors projectBranchLodErrors(const FixedLodGroup& group, const DirectX::XMFLOAT4X4& localToWorld,
                                          const DirectX::XMFLOAT4X4& view) const;
    void updateBranchLod(ViewerScene& scene, const DirectX::XMFLOAT4X4& view) const;
    FrameTiming drawFrame(const ViewerScene& scene, const DirectX::XMFLOAT4X4& view, float windPhase,
                          std::string_view hud,
                          const std::function<void(ID3D12GraphicsCommandList*)>& drawOverlay = {},
                          std::vector<std::uint32_t>* capture = nullptr,
                          const DirectX::XMFLOAT4X4* cascadeFitView = nullptr,
                          std::vector<std::uint64_t>* linearCapture = nullptr);

    enum class OakRaster { Live, Full, Depth, Visibility, Flat, EarlyDepthFull };
    void setOakRasterReplay(OakRaster mode, bool validate = false);
    void readOakRasterValidation(std::vector<uint32_t>& depth, std::vector<uint64_t>& ids);
    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    DXGI_FORMAT backBufferFormat() const;
    const std::wstring& adapterName() const { return adapterName_; }
    void writeTextureView(std::size_t texture, D3D12_CPU_DESCRIPTOR_HANDLE destination) const;

private:
    D3D12Renderer(HWND window, UINT width, UINT height, const std::filesystem::path& skyPanorama, bool cullMeshBackfaces);
    static constexpr UINT bufferCount = 2;
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;
    using LodTag = std::array<std::uint32_t, 3>;

    struct GpuFixedLodLevel
    {
        ComPtr<ID3D12Resource> indexBuffer;
        ComPtr<ID3D12Resource> triangleTags;
        D3D12_VERTEX_BUFFER_VIEW proxyVertices{};
        D3D12_INDEX_BUFFER_VIEW indices{};
        UINT indexCount = 0;
    };

    struct GpuSprayLod
    {
        struct CutPlan
        {
            float error;
            std::size_t wholeLevel;
            std::vector<std::size_t> localCuts;
        };

        struct Group
        {
            Bounds bounds;
            float maxScale;
            float minScale;
            UINT first;
            UINT count;
            std::size_t rightChild = 0;
        };

        Bounds bounds;
        std::vector<GpuFixedLodLevel> levels;
        std::array<float, fixedLodLevelCount> errors{};
        ComPtr<ID3D12Resource> proxyVertices;
        std::vector<MeshInstance> groupedInstances;
        std::vector<LodTag> groupedTags;
        std::vector<Group> groups;
        std::size_t localGroupCount = 0;
        struct LeafGroup
        {
            Bounds bounds;
            std::size_t firstCut;
            std::size_t cutCount;
        };
        std::vector<LeafGroup> leafGroups;
        std::vector<SprayGroupCut> localCuts;
        std::array<GpuFixedLodLevel, 2> localLevels;
        std::vector<CutPlan> cutPlans;
        std::size_t selectCut(float error) const;
    };

    struct GpuMesh
    {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vertices;
        D3D12_INDEX_BUFFER_VIEW indices;
        ComPtr<ID3D12Resource> meshInstances;
        D3D12_VERTEX_BUFFER_VIEW meshInstanceView{};
        UINT meshInstanceCount = 0;
        std::unique_ptr<GpuSprayLod> sprayLod;
        UINT indexCount;
        DirectX::XMFLOAT4X4 localToWorld;
        DirectX::XMFLOAT4X4 normalToWorld;
        bool mirrored;
        std::size_t material;
    };

    struct GpuTreePart
    {
        const TreeLodPart* bake = nullptr;
        GpuMesh geometry{};
        std::vector<MeshInstance> instances;
        ComPtr<ID3D12Resource> orderedInstances;
        std::vector<LodTag> tags;
        std::vector<float> maxScale;
        ComPtr<ID3D12Resource> gpuNodes, gpuPlacements, gpuPlans, gpuBuckets, gpuDraws;
        UINT placementCount = 0;
        UINT bucketCount = 0;
        UINT localPlanCount = 0;
        UINT wholePlanCount = 0;
    };
    struct LiveCut
    {
        struct Node
        {
            DirectX::XMFLOAT4 sphere;
            Bounds bounds;
            float minimum, maximum;
            UINT end, kind, geometry, instance;
        };
        struct Tier { float limit; UINT geometry; };
        using Ranges = std::array<FixedLodIndexRange, 3>;
        std::vector<Node> nodes;
        std::vector<Ranges> geometry;
        std::vector<Tier> tiers;
        std::vector<MeshInstance> placements;
        std::vector<std::array<UINT,2>> regions;
        struct Batches {
            std::vector<std::vector<UINT>> bins;
            std::vector<UINT> used;
        };
        std::vector<Batches> batches;
        UINT treeCount = 0;
        std::vector<UINT> sourceIndices;
        UINT replace = 0;
        bool packAssembly = false;
        GpuMesh mesh{};
        ComPtr<ID3D12Resource> instances, packed, assemblyIndices;
        UINT* mappedIndices = nullptr;
        size_t indexOffset = 0;
        MeshInstance* mapped = nullptr;
        size_t offset = 0;
    };
    LiveCut liveCut_;
    std::filesystem::path livePath_;
    void loadLiveCut(const ViewerScene& scene);
    struct LiveGpu
    {
        struct View {
            DirectX::XMFLOAT4X4 world, view;
            DirectX::XMFLOAT4 planes[6], windFocal, viewWindNear;
            float scale; UINT visible, frustum, pad;
        };
        std::array<ComPtr<ID3D12Resource>,4> inputs;
        ComPtr<ID3D12Resource> views, active, bins, instances, arguments, counts, readback;
        std::vector<std::array<UINT,2>> levels;
        UINT capacity = 0, packStride = 0;
        std::array<UINT,3> packSizes{};
        ComPtr<ID3D12Resource> packedIndices, packedDraws;
        bool used = false, readable = false;
    } liveGpu_;
    ComPtr<ID3D12RootSignature> liveGpuRoot_;
    std::array<ComPtr<ID3D12PipelineState>,4> liveGpuPipelines_;
    ComPtr<ID3D12CommandSignature> liveGpuSignature_;
    void prepareLiveGpu();
    void dispatchLiveGpu(const ViewerScene& scene, PassStats& stats);
    void drawLiveGpu(const ViewerScene& scene, size_t meshIndex, UINT slot, UINT object, PassStats& stats);
    void readLiveGpuStats(PassStats& stats);
    void selectLiveCut(const ViewerScene& scene, UINT objectIndex, PassStats& stats);
    void drawLiveCut(const ViewerScene& scene, size_t meshIndex, UINT slot, UINT objectIndex, PassStats& stats);

    struct TreeGpuReadback
    {
        bool shadow;
        std::optional<std::size_t> slice;
        std::size_t part;
    };

    struct TreeSelection
    {
        std::uint32_t node;
        bool proxy;
        bool fullyVisible;
    };

    struct TreeDraw
    {
        D3D12_VERTEX_BUFFER_VIEW vertices;
        D3D12_VERTEX_BUFFER_VIEW instances;
        D3D12_INDEX_BUFFER_VIEW indices;
        UINT firstTag;
        UINT lodLevel;
        D3D12_GPU_VIRTUAL_ADDRESS tags;
        D3D12_DRAW_INDEXED_ARGUMENTS indexed;
        UINT padding = 0;
    };

    struct TreeGpuNode
    {
        Bounds bounds;
        UINT firstChild, childCount, indexCount, placements;
        float error;
        UINT firstPlacement;
    };

    struct TreeGpuPlacement
    {
        MeshInstance instance;
        DirectX::XMFLOAT4X4 toAsset;
        LodTag tag;
        UINT leaf;
    };

    struct TreeGpuPlan
    {
        float error;
        UINT first, count;
    };

    struct TreeGpuTable
    {
        std::size_t meshIndex;
        std::vector<TreeGpuNode> nodes;
        std::vector<TreeGpuPlacement> placements;
        std::vector<TreeGpuPlan> plans;
        std::vector<UINT> planBins;
        std::vector<TreeDraw> templates;
    };

    struct TreeBatchDraw
    {
        D3D12_VERTEX_BUFFER_VIEW vertices, sprays;
        D3D12_INDEX_BUFFER_VIEW indices;
        UINT firstTag, lodLevel;
        D3D12_GPU_VIRTUAL_ADDRESS tags;
        D3D12_VERTEX_BUFFER_VIEW trees;
        D3D12_DRAW_INDEXED_ARGUMENTS indexed;
        UINT padding = 0;
    };

    struct TreeDrawBatch
    {
        std::size_t meshIndex;
        UINT firstBin, binCount, firstCommand, commandCapacity, part;
        bool proxy;
    };

    struct TreeBatchGpu
    {
        struct Bin { UINT triangles, sprays, batch; };
        struct Pass
        {
            bool shadow;
            std::optional<std::size_t> slice;
            UINT binCount;
            bool sparse, clusters, history, recovery;
        };
        std::vector<Bin> bins;
        std::vector<TreeDrawBatch> batches;
        std::vector<std::size_t> meshIndices;
        std::vector<Pass> passes;
        UINT partCount = 0, rootCapacity = 0, recordCapacity = 0;
        UINT stateBytes = 0, poseStride = 0, baseBins = 0;
        Bounds bounds{};
        std::array<ComPtr<ID3D12Resource>, 7> tables;
        ComPtr<ID3D12Resource> constants, poses, reset, state, dispatch, jobs, records, arguments, instances, readback;
        std::byte* mappedConstants = nullptr;
        std::byte* mappedPoses = nullptr;
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> timesReadback;
        ComPtr<ID3D12Resource> pyramid, deferred;
        DirectX::XMFLOAT4X4 previousClip{};
        DirectX::XMFLOAT2 previousWind{};
        bool depthUsed = false, historyValid = false, pyramidReadable = false;
    };

    struct CameraPipeline
    {
        ComPtr<ID3D12PipelineState> linear;
        ComPtr<ID3D12PipelineState> encoded;

        ID3D12PipelineState* get(bool encodedOutput) const
        {
            return encodedOutput ? encoded.Get() : linear.Get();
        }
    };

    struct RenderMode
    {
        ComPtr<ID3D12Resource> depthBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE depthTarget{};
        CameraPipeline mesh;
        CameraPipeline oneSidedMesh;
        CameraPipeline sky;
        CameraPipeline voxel;
    };

    struct FrameReadback
    {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT64 bytes = 0;
    };

    struct PassVisibility;
    struct GpuVoxelLevel
    {
        ComPtr<ID3D12Resource> axes;
        UINT meshIndex = 0;
        ComPtr<ID3D12Resource> bricks;
        ComPtr<ID3D12Resource> normals;
        bool hasDirectionalWeights = false;
        bool hasShadowReceivers = false;
        DirectX::XMFLOAT3 origin{};
        float cellSize = 0;
        UINT brickCount = 0;
    };

    struct BranchDraw
    {
        UINT firstTriangle;
        D3D12_DRAW_INDEXED_ARGUMENTS indexed;
    };

    struct InstanceRange
    {
        UINT first;
        UINT count;

        bool operator==(const InstanceRange&) const = default;
    };

    struct PassVisibility
    {
        std::vector<InstanceRange> ranges;
        UINT count = 0;

        void clear();
        void add(UINT instance);
        void add(UINT first, UINT length);
        bool contains(UINT instance) const;
    };

    struct BranchPassDraws
    {
        std::array<std::vector<BranchDraw>, fixedLodLevelCount> byLevel;
        std::array<std::uint64_t, fixedLodLevelCount> triangles{};
        std::optional<UINT64> argumentOffset;
    };

    struct CascadeCasterDraws
    {
        PassVisibility instances;
        PassVisibility sourceFoliage;
        BranchPassDraws bakedFoliage;
        std::size_t groupsConsidered = 0;
        std::size_t groupsVisible = 0;
    };

    struct BranchLodProjection
    {
        Bounds bounds;
        float farError;
        float proxyError;
        float lockedRimError;
        float rimError;
    };

    void createDepthBuffer(RenderMode& mode, D3D12_RESOURCE_DESC description);
    void createShadowMap();
    void createCascadeShadowMaps();
    ComPtr<ID3D12Resource> createShadowDepth(UINT16 slices);
    void writeCascadeShadowView();
    void updateShadowCascades(const DirectX::XMFLOAT4X4& view, const ViewerScene& scene);
    ComPtr<ID3D12Resource> createRenderTexture(const D3D12_RESOURCE_DESC& description,
                                               D3D12_RESOURCE_STATES initialState,
                                               const D3D12_CLEAR_VALUE& clear);
    ComPtr<ID3D12Resource> createCameraColor(DXGI_FORMAT format, DXGI_SAMPLE_DESC samples,
                                             D3D12_CPU_DESCRIPTOR_HANDLE target,
                                             D3D12_RESOURCE_STATES initialState);
    CameraPipeline createCameraPipeline(D3D12_GRAPHICS_PIPELINE_STATE_DESC description, const char* operation);
    void createMeshPipelineLayout();
    void createSky(const std::filesystem::path& panorama);
    void drawSky(ID3D12PipelineState* pipeline, const DirectX::XMFLOAT4X4& view, bool skyOnly);
    void createDisplayPipeline();
    void drawDisplay(float exposureEv);
    void createProbePipeline();
    void initializeProbeField(const ViewerScene& scene);
    ComPtr<ID3D12Resource> createProbeBuffer(UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
                                           D3D12_RESOURCE_STATES initial);
    void orderProbeUpdates();
    struct ProbeUpdate
    {
        unsigned count = 0;
        bool advected = false;
        bool groundUpdated = false;
    };
    ProbeUpdate updateProbeField(const ViewerScene& scene);
    void writeProbeView();
    void createBranchCommandSignature();
    void createTreeCommandSignature();
    void uploadTreeHierarchy(const ViewerScene& scene, const std::vector<GpuMesh>& meshes);
    void createTreeGpuPipelines();
    void prepareTreeGpu(const ViewerScene& scene);
    void prepareTreeBatch(const ViewerScene& scene, std::span<const TreeGpuTable> tables);
    bool drawBatchedTrees(const ViewerScene& scene, const PassVisibility& visibility, bool shadowPass,
                          std::optional<std::size_t> shadowSlice, PassStats& stats);
    void resolveTreeBatchTimes();
    void buildTreeBatchDepth();
    void finishTreeBatchDepth(DirectX::FXMMATRIX viewProjection);
    void readTreeBatchStats(const ViewerScene& scene, PassStats& camera, PassStats& shadow,
                            std::array<PassStats, sunCascadeSplits.size()>& cascades);
    void buildTreeDepth();
    void writeTreeDepthViews(D3D12_CPU_DESCRIPTOR_HANDLE first);
    void drawGpuTreePart(const ViewerScene& scene, std::size_t meshIndex, const SceneObject& object,
                         DirectX::FXMMATRIX viewProjection, bool shadowPass, std::optional<std::size_t> shadowSlice,
                         FixedLodTint tint, PassStats& stats);
    void readTreeGpuStats(const ViewerScene& scene, PassStats& camera, PassStats& shadow,
                          std::array<PassStats, sunCascadeSplits.size()>& cascades);
    void createVoxelPipeline(bool enableMultisampling);
    GpuVoxelLevel uploadVoxelLevel(const VoxelFoliage& foliage, const VoxelNormalData* normals);
    void uploadBranchVoxels(const ViewerScene& scene);
    void uploadBoughVoxels(const ViewerScene& scene);
    void drawBranchVoxels(const ViewerScene& scene, const GpuVoxelLevel& level, DirectX::FXMMATRIX view,
                          const PassVisibility& visibility, PassStats& stats, bool packPlacements, bool shadowPass = false,
                          const SceneObject* tree = nullptr);
    static BranchLodProjection branchGroupProjection(const FixedLodGroup& group, bool fullCrown = false);
    static BranchLodProjection branchInstanceProjection(std::span<const BranchLodProjection> groups);
    BranchLodErrors projectBranchLodErrors(const BranchLodProjection& projection,
                                          const DirectX::XMFLOAT4X4& localToWorld,
                                          const DirectX::XMFLOAT4X4& view) const;
    void installMeshPipelines(D3D12_SHADER_BYTECODE cameraVertex, D3D12_SHADER_BYTECODE cameraPixel,
                              D3D12_SHADER_BYTECODE shadowVertex, D3D12_SHADER_BYTECODE shadowPixel,
                              const ShadowBiasSettings& bias, bool enableMultisampling);
    ComPtr<ID3D12PipelineState> createShadowPipeline(D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel,
                                                     const ShadowBiasSettings& bias);
    void updateShadowPipeline(const ShadowBiasSettings& bias);
    void createTextures(std::span<const TextureAsset> textures);
    ComPtr<ID3D12Resource> uploadTexture(const TextureData& mips,
                                         std::vector<ComPtr<ID3D12Resource>>& staging,
                                         DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                         D3D12_RESOURCE_STATES ready = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    GpuMesh uploadMesh(const MeshData& mesh);
    std::unique_ptr<GpuSprayLod> uploadSprayLod(const MeshData& mesh, const FixedLod& lod,
                                               std::span<const std::uint32_t> boughMembers);
    void buildSprayGroups(GpuSprayLod& lod, const MeshData& mesh, std::span<const std::uint32_t> boughMembers);
    GpuFixedLodLevel uploadFixedLodLevel(const FixedLod& lod, std::size_t levelIndex, bool tagTriangles = true);
    ComPtr<ID3D12Resource> createUploadBuffer(std::size_t size);
    ComPtr<ID3D12Resource> uploadBuffer(const void* data, std::size_t size);
    DirectX::XMFLOAT2 updateFrameConstants(const ViewerScene& scene, DirectX::FXMMATRIX viewProjection, float windPhase);
    void prepareVisibility(const ViewerScene& scene, DirectX::FXMMATRIX viewProjection, DirectX::XMFLOAT2 windOffset);
    void prepareFoliageGroupVisibility(const ViewerScene& scene, DirectX::FXMMATRIX viewProjection, DirectX::XMFLOAT2 windOffset);
    void prepareCascadeCasterDraws(const ViewerScene& scene, DirectX::XMFLOAT2 windOffset, std::size_t& argumentBytes);
    void prepareBranchDraws(const ViewerScene& scene, const PassVisibility& visibility, BranchPassDraws& pass,
                            std::size_t& argumentBytes, bool omitVoxelRegions = false,
                            std::span<const std::uint8_t> visibleGroups = {}, int forcedLevel = automaticBranchLod,
                            std::span<const int> instanceLevels = {});
    void copyFrameColor(ID3D12Resource* color, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after, FrameReadback& readback);
    template <typename Pixel>
    void readFrameColor(const FrameReadback& readback, std::vector<Pixel>& pixels);
    PassStats drawShadowMap(const ViewerScene& scene);
    PassStats drawCascadeShadowMaps(const ViewerScene& scene, std::array<PassStats, sunCascadeSplits.size()>& slices);
    PassStats drawMeshes(const ViewerScene& scene, bool shadowPass, bool omitFoliage = false,
                        const CascadeCasterDraws* cascade = nullptr, std::optional<std::size_t> shadowSlice = {});
    void drawInstancedMesh(const ViewerScene& scene, std::size_t meshIndex, const PassVisibility& visibility,
                           bool shadowPass, std::optional<std::size_t> shadowSlice, PassStats& stats);
    void selectTreeCut(const GpuTreePart& part, const ViewerScene& scene, const SceneObject& object,
                       DirectX::FXMMATRIX viewProjection, bool shadowPass, PassStats& stats);
    void submitTreeDraws(const ViewerScene& scene, const GpuMesh& mesh, bool receiveShadows,
                         FixedLodTint tint, std::vector<TreeDraw>& draws, PassStats& stats);
    TreeDraw treeDraw(const GpuMesh& mesh, const GpuFixedLodLevel* lod, FixedLodIndexRange range,
                      UINT count, UINT firstInstance, D3D12_VERTEX_BUFFER_VIEW instances,
                      UINT level = 0, UINT firstTag = 0, D3D12_GPU_VIRTUAL_ADDRESS tags = 0) const;
    void bindMesh(const GpuMesh& mesh, const MaterialData& material, bool wind, bool receiveShadows,
                  const GpuFixedLodLevel* lod, FixedLodTint tint, UINT highlightedGroup, UINT lodLevel,
                  UINT firstTag, D3D12_GPU_VIRTUAL_ADDRESS tags = 0);
    std::uint64_t drawMesh(const GpuMesh& mesh, const MaterialData& material, bool wind,
                  bool receiveShadows, UINT instanceCount, UINT firstInstance,
                  const GpuFixedLodLevel* lod = nullptr, FixedLodTint tint = FixedLodTint::Shaded,
                  UINT highlightedGroup = 0, UINT lodLevel = 0, FixedLodIndexRange range = {},
                  D3D12_GPU_VIRTUAL_ADDRESS tags = 0);
    void createFrameTimer();
    void readGpuTimes(FrameTiming& timing) const;
    void createHud();
    void drawHud(D3D12_CPU_DESCRIPTOR_HANDLE target, std::string_view text);
    void waitForGpu();

    ComPtr<ID3D12Device> device_;
    std::wstring adapterName_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> renderTargetHeap_;
    std::array<ComPtr<ID3D12Resource>, bufferCount> backBuffers_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, bufferCount> renderTargets_{};
    ComPtr<ID3D12DescriptorHeap> depthHeap_;
    ComPtr<ID3D12Resource> shadowMap_;
    ComPtr<ID3D12Resource> cascadeShadowMap_;
    ComPtr<ID3D12PipelineState> shadowPipeline_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC meshPipelineDescription_{};
    bool cullMeshBackfaces_ = false;
    MeshShaders reloadedShaders_;
    int appliedConstantBias_ = 0;
    float appliedSlopeBias_ = 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDepthTarget_{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, sunCascadeSplits.size()> cascadeDepthTargets_{};
    D3D12_CPU_DESCRIPTOR_HANDLE cascadeShadowView_{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowTexture_{};
    DirectX::XMFLOAT4X4 lightViewProjection_;
    DirectX::XMFLOAT4X4 sunRotation_;
    DirectX::XMFLOAT2 cascadeCasterDepth_;
    std::array<DirectX::XMFLOAT4X4, sunCascadeSplits.size()> cascadeViewProjections_;
    std::array<float, sunCascadeSplits.size()> cascadeWorldUnitsPerTexel_{};
    std::array<float, sunCascadeSplits.size()> cascadeDepthPerTexel_{};
    DirectX::XMFLOAT3 toSun_;
    float shadowDepthPerTexel_ = 0.0f;
    float shadowWorldUnitsPerTexel_ = 0.0f;
    float windStart_ = 0.0f;
    float inverseWindHeight_ = 0.0f;
    float windAmplitude_ = 0.0f;
    RenderMode singleSample_;
    RenderMode multisample_;
    ComPtr<ID3D12Resource> sceneColor_;
    ComPtr<ID3D12Resource> multisampleColor_;
    ComPtr<ID3D12Resource> encodedMultisampleColor_;
    D3D12_CPU_DESCRIPTOR_HANDLE sceneTarget_{};
    D3D12_CPU_DESCRIPTOR_HANDLE multisampleTarget_{};
    D3D12_CPU_DESCRIPTOR_HANDLE encodedMultisampleTarget_{};
    bool msaaEnabled_ = false;
    bool encodedCamera_ = false;
    bool useLeafResponse_ = false;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12Resource> frameConstants_;
    ComPtr<ID3D12Resource> instances_;
    D3D12_VERTEX_BUFFER_VIEW instanceView_{};
    ComPtr<ID3D12Resource> identityMeshInstance_;
    D3D12_VERTEX_BUFFER_VIEW identityMeshInstanceView_{};
    std::shared_ptr<const AssetData> asset_;
    std::shared_ptr<const FixedLod> fixedLodAsset_;
    std::array<std::shared_ptr<const FixedLod>, 2> sprayLodAssets_;
    std::shared_ptr<const SprayBough> sprayBoughAsset_;
    std::array<GpuMesh, 2> sprayBoughMeshes_{};
    Bounds sprayBoughBounds_{};
    std::shared_ptr<const TreeHierarchy> treeHierarchyAsset_;
    std::vector<GpuTreePart> treeParts_;
    ComPtr<ID3D12CommandSignature> treeCommandSignature_;
    ComPtr<ID3D12Resource> treeArguments_;
    TreeDraw* mappedTreeArguments_ = nullptr;
    std::size_t treeArgumentOffset_ = 0;
    std::vector<TreeSelection> treeSelection_;
    std::vector<TreeDraw> treeProxyDraws_;
    std::vector<TreeDraw> treeInstancedProxyDraws_;
    std::vector<TreeDraw> treeSourceDraws_;
    ComPtr<ID3D12RootSignature> treeGpuRoot_, treeDepthRoot_;
    ComPtr<ID3D12PipelineState> treeGpuPipeline_, treeDepthPipeline_, treeBatchDepthPipeline_;
    ComPtr<ID3D12Resource> treeGpuArguments_, treeGpuResults_, treeGpuInstances_, treeGpuTags_, treeGpuDepth_;
    ComPtr<ID3D12Resource> treeGpuConstants_, treeGpuReadbacks_;
    std::vector<TreeGpuReadback> treeGpuReadbackParts_;
    std::byte* mappedTreeGpuConstants_ = nullptr;
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> treeDepthViews_{};
    bool treeDepthReady_ = false;
    TreeBatchGpu treeBatch_;
    ComPtr<ID3D12CommandSignature> treeBatchDrawSignature_, treeBatchDispatchSignature_;
    ComPtr<ID3D12RootSignature> treeBatchRoot_;
    std::array<ComPtr<ID3D12PipelineState>, 8> treeBatchPipelines_;
    std::vector<GpuMesh> meshes_;
    ComPtr<ID3D12Resource> sprayInstances_;
    MeshInstance* mappedSprayInstances_ = nullptr;
    ComPtr<ID3D12Resource> sprayInstanceTags_;
    LodTag* mappedSprayInstanceTags_ = nullptr;
    ComPtr<ID3D12Resource> spraySourceGroupTags_;
    std::vector<LodTag> sprayGroupTags_;
    std::size_t sprayInstanceOffset_ = 0;
    std::array<std::vector<MeshInstance>, fixedLodLevelCount> sprayBatches_;
    std::array<std::vector<LodTag>, fixedLodLevelCount> sprayTagBatches_;
    std::array<PassVisibility, fixedLodLevelCount> sprayGroupBatches_;
    std::vector<PassVisibility> sprayLocalBatches_;
    DirectX::XMFLOAT4X4 frameView_;
    DirectX::XMFLOAT2 frameWind_{};
    std::array<GpuFixedLodLevel, fixedLodLevelCount> fixedLodLevels_;
    ComPtr<ID3D12Resource> fixedLodProxyVertices_;
    std::vector<BranchLodProjection> branchGroupProjections_;
    BranchLodProjection branchInstanceProjection_{};
    std::vector<Bounds> cascadeFoliageBlocks_;
    float cascadeProxyError_ = 0;
    Bounds branchCullBounds_{};
    Bounds voxelCullBounds_{};
    PassVisibility cameraVisibility_;
    PassVisibility cameraTriangleBranches_;
    PassVisibility cameraVoxelBranches_;
    std::array<PassVisibility, voxelBranchRegionCount> cameraVoxelRegions_;
    PassVisibility shadowVisibility_;
    PassVisibility shadowMatchVisibility_;
    PassVisibility shadowOverrideVisibility_;
    BranchPassDraws cameraBranchDraws_;
    std::vector<std::uint8_t> cameraGroupVisibility_;
    std::size_t cameraGroupsConsidered_ = 0;
    std::size_t cameraGroupsVisible_ = 0;
    BranchPassDraws shadowBranchDraws_;
    std::array<CascadeCasterDraws, sunCascadeSplits.size()> cascadeCasterDraws_;
    PassVisibility cascadeBakedVisibility_;
    std::vector<int> cascadeInstanceLevels_;
    std::vector<std::uint8_t> cascadeGroupVisibility_;
    std::vector<std::uint8_t> cascadeInstanceFlags_;
    bool shareBranchDraws_ = true;
    bool showShadowFactor_ = false;
    bool captureGeometryMask_ = false;
    bool boughDepth_ = false, boughMaterials_ = false;
    const GpuMesh* diagnosticFoliage_ = nullptr;
    BranchAppearance branchAppearance_ = BranchAppearance::Shaded;
    ComPtr<ID3D12CommandSignature> branchCommandSignature_;
    ComPtr<ID3D12Resource> branchArgumentBuffer_;
    std::array<GpuVoxelLevel, branchVoxelBakeCount> voxelLevels_;
    std::array<GpuVoxelLevel, 2> boughVoxelLevels_;
    std::array<PassVisibility, 2> selectedBoughTrees_;
    std::array<Bounds, 2> boughBrickBounds_{};
    CameraPipeline brickResolve_;
    ComPtr<ID3D12PipelineState> brickClear_, brickVisibility_;
    std::array<ComPtr<ID3D12Resource>,2> brickHits_, brickWork_;
    struct BrickWork
    {
        DirectX::XMFLOAT4 placement;
        UINT left, top, width, height, firstPixel;
        UINT padding[3]{};
    };
    std::vector<BrickWork> brickWorkItems_;
    size_t boughTreeCapacity_ = 0;
    std::filesystem::path boughVoxelPath_;
    UINT boughFirst_ = 0, boughCount_ = 0, boughReplace_ = 0;
    bool mixedBoughVoxels_ = false;
    std::array<GpuVoxelLevel, voxelBranchRegionCount> voxelRegionLevels_;
    std::array<BranchLodProjection, voxelBranchRegionCount> voxelRegionProjections_;
    std::vector<std::uint8_t> voxelGroupRegionBits_;
    ComPtr<ID3D12Resource> voxelPlacements_;
    std::size_t voxelDrawSlot_ = 0;
    UINT voxelPlacementOffset_ = 0;
    ComPtr<ID3D12RootSignature> voxelRoot_;
    ComPtr<ID3D12PipelineState> voxelShadowPipeline_;
    ComPtr<ID3D12Resource> voxelConstants_;
    ComPtr<ID3D12Resource> voxelBoxIndices_;
    GpuMesh ground_;
    ComPtr<ID3D12DescriptorHeap> textureHeap_;
    std::vector<ComPtr<ID3D12Resource>> textures_;
    UINT textureDescriptorSize_ = 0;
    ComPtr<ID3D12RootSignature> skyRoot_;
    ComPtr<ID3D12Resource> skyPanorama_;
    ComPtr<ID3D12Resource> skyIrradiance_;
    float skyRadianceScale_ = 1;
    DirectX::XMFLOAT3 openSkyGround_{};
    D3D12_GPU_DESCRIPTOR_HANDLE skyTexture_{};
    DiffuseProbeGrid probeGrid_{};
    DirectX::XMFLOAT4 probeDensityOrigin_{};
    DirectX::XMUINT4 probeDensitySize_{};
    bool probesActive_ = false;
    bool probesDirty_ = true;
    ProbeLightingMode cachedProbeMode_ = ProbeLightingMode::Off;
    CaptureLighting cachedProbeLighting_ = CaptureLighting::Combined;
    DirectX::XMFLOAT3 cachedProbeSun_{};
    DirectX::XMFLOAT3 cachedGroundColor_{};
    DirectX::XMFLOAT2 cachedProbeWind_{};
    DirectX::XMFLOAT2 observedProbeWind_{};
    unsigned probeWindAge_ = 0;
    bool probeWindMoving_ = false;
    bool probeCatchup_ = false;
    bool probeLightChangedLastFrame_ = false;
    unsigned probeCursor_ = 0;
    unsigned probesPending_ = 0;
    unsigned groundUpdateRow_ = 0;
    ComPtr<ID3D12RootSignature> probeRoot_;
    ComPtr<ID3D12PipelineState> probePipeline_;
    ComPtr<ID3D12PipelineState> groundProbePipeline_;
    ComPtr<ID3D12PipelineState> advectProbePipeline_;
    ComPtr<ID3D12Resource> probeDensity_;
    ComPtr<ID3D12Resource> advectedProbeDensity_;
    ComPtr<ID3D12Resource> probeUpdateOrder_;
    ComPtr<ID3D12Resource> diffuseProbes_;
    ComPtr<ID3D12Resource> probeGroundLight_;
    D3D12_CPU_DESCRIPTOR_HANDLE probeCpuView_{};
    D3D12_GPU_DESCRIPTOR_HANDLE probeGpuView_{};
    ComPtr<ID3D12RootSignature> displayRoot_;
    ComPtr<ID3D12PipelineState> displayPipeline_;
    D3D12_GPU_DESCRIPTOR_HANDLE sceneColorView_{};
    DirectX::XMFLOAT4X4 projection_;
    float cameraNearPlane_ = 0;
    float cameraFarPlane_ = 0;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    ComPtr<ID3D12QueryHeap> timestampQueries_;
    ComPtr<ID3D12QueryHeap> cameraPipelineQuery_;
    ComPtr<ID3D12Resource> timestampReadback_;
    FrameReadback displayReadback_;
    OakRaster oakRaster_ = OakRaster::Live;
    bool validateOakRaster_ = false;
    std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 4> oakRasterPipelines_;
    ComPtr<ID3D12DescriptorHeap> oakIdHeap_;
    ComPtr<ID3D12Resource> oakIds_, oakPlacementKeys_;
    FrameReadback oakDepthReadback_, oakIdReadback_;
    FrameReadback linearReadback_;
    UINT64 timestampFrequency_ = 0;
    ComPtr<ID3D12RootSignature> hudRoot_;
    ComPtr<ID3D12PipelineState> hudPipeline_;
    ComPtr<ID3D12Resource> hudGlyphs_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_ = 0;
    std::unique_ptr<void, decltype(&CloseHandle)> fenceEvent_{nullptr, CloseHandle};
};
