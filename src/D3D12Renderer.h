#pragma once

#include "ShaderCompiler.h"
#include "ViewerScene.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
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
    };

    struct FrameTiming
    {
        double cpuMilliseconds;
        std::optional<double> gpuMilliseconds;
        unsigned drawCalls = 0;
        std::optional<double> gpuCameraMilliseconds;
        std::optional<double> gpuShadowMilliseconds;
        PassStats camera;
        PassStats shadow;
        std::array<PassStats, sunCascadeSplits.size()> shadowCascades;
        double cpuBranchDrawListMilliseconds = 0;
    };

    explicit D3D12Renderer(HWND window);

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    bool supportsMsaa() const;
    bool msaaEnabled() const;
    void toggleMsaa();
    void setScene(const ViewerScene& scene);
    void updateCameraProjection(float nearPlane, float farPlane);
    void updateSun(const ViewerScene& scene);
    void reloadMeshShaders(MeshShaders shaders, const ShadowBiasSettings& bias);
    float shadowDepthPerTexel() const { return shadowDepthPerTexel_; }
    float shadowWorldUnitsPerTexel() const { return shadowWorldUnitsPerTexel_; }
    const auto& sunCascadeTexelWidths() const { return cascadeWorldUnitsPerTexel_; }
    static constexpr float cameraVerticalFov = DirectX::XM_PIDIV4;
    DirectX::XMFLOAT2 viewportSize() const { return {viewport_.Width, viewport_.Height}; }
    float projectLodError(float localError, const Bounds& bounds,
                          const DirectX::XMFLOAT4X4& localToWorld, const DirectX::XMFLOAT4X4& view) const;
    float projectShadowError(float localError, const DirectX::XMFLOAT4X4& localToWorld) const;
    struct BranchLodErrors { float farPixels; float proxyPixels; };
    BranchLodErrors projectBranchLodErrors(const FixedLodGroup& group, const DirectX::XMFLOAT4X4& localToWorld,
                                          const DirectX::XMFLOAT4X4& view) const;
    void updateBranchLod(ViewerScene& scene, const DirectX::XMFLOAT4X4& view) const;
    FrameTiming drawFrame(const ViewerScene& scene, const DirectX::XMFLOAT4X4& view, float windPhase,
                          std::string_view hud,
                          const std::function<void(ID3D12GraphicsCommandList*)>& drawOverlay = {},
                          std::vector<std::uint32_t>* capture = nullptr,
                          const DirectX::XMFLOAT4X4* cascadeFitView = nullptr);

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    DXGI_FORMAT backBufferFormat() const;
    const std::wstring& adapterName() const { return adapterName_; }
    void writeTextureView(std::size_t texture, D3D12_CPU_DESCRIPTOR_HANDLE destination) const;

private:
    static constexpr UINT bufferCount = 2;
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct GpuMesh
    {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vertices;
        D3D12_INDEX_BUFFER_VIEW indices;
        UINT indexCount;
        DirectX::XMFLOAT4X4 localToWorld;
        DirectX::XMFLOAT4X4 normalToWorld;
        bool mirrored;
        std::size_t material;
    };

    struct RenderMode
    {
        ComPtr<ID3D12Resource> depthBuffer;
        D3D12_CPU_DESCRIPTOR_HANDLE depthTarget{};
        ComPtr<ID3D12PipelineState> pipeline;
    };

    struct GpuFixedLodLevel
    {
        ComPtr<ID3D12Resource> indexBuffer;
        ComPtr<ID3D12Resource> triangleTags;
        D3D12_VERTEX_BUFFER_VIEW proxyVertices{};
        D3D12_INDEX_BUFFER_VIEW indices{};
        UINT indexCount = 0;
    };

    struct PassVisibility;
    struct GpuVoxelLevel
    {
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
    void createMeshPipelineLayout();
    void createBranchCommandSignature();
    void createVoxelPipeline(bool enableMultisampling);
    GpuVoxelLevel uploadVoxelLevel(const VoxelFoliage& foliage, const VoxelNormalData* normals);
    void uploadBranchVoxels(const ViewerScene& scene);
    void drawBranchVoxels(const ViewerScene& scene, const GpuVoxelLevel& level, DirectX::FXMMATRIX view,
                          const PassVisibility& visibility, PassStats& stats, bool packPlacements, bool shadowPass = false);
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
                                         std::vector<ComPtr<ID3D12Resource>>& staging);
    GpuMesh uploadMesh(const MeshData& mesh);
    GpuFixedLodLevel uploadFixedLodLevel(const FixedLod& lod, std::size_t levelIndex);
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
    void copyFrameColor(ID3D12Resource* backBuffer);
    void readFrameColor(std::vector<std::uint32_t>& pixels);
    PassStats drawShadowMap(const ViewerScene& scene);
    PassStats drawCascadeShadowMaps(const ViewerScene& scene, std::array<PassStats, sunCascadeSplits.size()>& slices);
    PassStats drawMeshes(const ViewerScene& scene, bool shadowPass, bool omitFoliage = false,
                        const CascadeCasterDraws* cascade = nullptr);
    void bindMesh(const GpuMesh& mesh, const MaterialData& material, bool wind, bool receiveShadows,
                  const GpuFixedLodLevel* lod, FixedLodTint tint, UINT highlightedGroup, UINT lodLevel,
                  UINT firstTriangle);
    std::uint64_t drawMesh(const GpuMesh& mesh, const MaterialData& material, bool wind,
                  bool receiveShadows, UINT instanceCount, UINT firstInstance,
                  const GpuFixedLodLevel* lod = nullptr, FixedLodTint tint = FixedLodTint::Shaded,
                  UINT highlightedGroup = 0, UINT lodLevel = 0, FixedLodIndexRange range = {});
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
    ComPtr<ID3D12Resource> multisampleColor_;
    D3D12_CPU_DESCRIPTOR_HANDLE multisampleTarget_{};
    bool msaaEnabled_ = false;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12Resource> frameConstants_;
    ComPtr<ID3D12Resource> instances_;
    D3D12_VERTEX_BUFFER_VIEW instanceView_{};
    std::shared_ptr<const AssetData> asset_;
    std::shared_ptr<const FixedLod> fixedLodAsset_;
    std::vector<GpuMesh> meshes_;
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
    const GpuMesh* diagnosticFoliage_ = nullptr;
    BranchAppearance branchAppearance_ = BranchAppearance::Shaded;
    ComPtr<ID3D12CommandSignature> branchCommandSignature_;
    ComPtr<ID3D12Resource> branchArgumentBuffer_;
    std::array<GpuVoxelLevel, branchVoxelBakeCount> voxelLevels_;
    std::array<GpuVoxelLevel, voxelBranchRegionCount> voxelRegionLevels_;
    std::array<BranchLodProjection, voxelBranchRegionCount> voxelRegionProjections_;
    std::vector<std::uint8_t> voxelGroupRegionBits_;
    ComPtr<ID3D12Resource> voxelPlacements_;
    std::size_t voxelDrawSlot_ = 0;
    UINT voxelPlacementOffset_ = 0;
    ComPtr<ID3D12RootSignature> voxelRoot_;
    ComPtr<ID3D12PipelineState> voxelSingleSample_;
    ComPtr<ID3D12PipelineState> voxelMultisample_;
    ComPtr<ID3D12PipelineState> voxelShadowPipeline_;
    ComPtr<ID3D12Resource> voxelConstants_;
    ComPtr<ID3D12Resource> voxelBoxIndices_;
    GpuMesh ground_;
    ComPtr<ID3D12DescriptorHeap> textureHeap_;
    std::vector<ComPtr<ID3D12Resource>> textures_;
    UINT textureDescriptorSize_ = 0;
    DirectX::XMFLOAT4X4 projection_;
    float cameraNearPlane_ = 0;
    float cameraFarPlane_ = 0;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    ComPtr<ID3D12QueryHeap> timestampQueries_;
    ComPtr<ID3D12Resource> timestampReadback_;
    ComPtr<ID3D12Resource> frameColorReadback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT frameColorLayout_{};
    UINT64 frameColorBytes_ = 0;
    UINT64 timestampFrequency_ = 0;
    ComPtr<ID3D12RootSignature> hudRoot_;
    ComPtr<ID3D12PipelineState> hudPipeline_;
    ComPtr<ID3D12Resource> hudGlyphs_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_ = 0;
    std::unique_ptr<void, decltype(&CloseHandle)> fenceEvent_{nullptr, CloseHandle};
};
