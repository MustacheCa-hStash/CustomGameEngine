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
    struct FrameTiming
    {
        double cpuMilliseconds;
        std::optional<double> gpuMilliseconds;
        unsigned drawCalls = 0;
    };

    explicit D3D12Renderer(HWND window);

    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    bool supportsMsaa() const;
    bool msaaEnabled() const;
    void toggleMsaa();
    void setScene(const ViewerScene& scene);
    void updateSun(const ViewerScene& scene);
    void reloadMeshShaders(MeshShaders shaders, const ShadowBiasSettings& bias);
    float shadowDepthPerTexel() const { return shadowDepthPerTexel_; }
    float shadowWorldUnitsPerTexel() const { return shadowWorldUnitsPerTexel_; }
    FrameTiming drawFrame(const ViewerScene& scene, const DirectX::XMFLOAT4X4& view, float windPhase,
                          std::string_view hud,
                          const std::function<void(ID3D12GraphicsCommandList*)>& drawOverlay = {});

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    DXGI_FORMAT backBufferFormat() const;
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

    void createDepthBuffer(RenderMode& mode, D3D12_RESOURCE_DESC description);
    void createShadowMap();
    ComPtr<ID3D12Resource> createRenderTexture(const D3D12_RESOURCE_DESC& description,
                                               D3D12_RESOURCE_STATES initialState,
                                               const D3D12_CLEAR_VALUE& clear);
    void createMeshPipelineLayout();
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
    ComPtr<ID3D12Resource> createUploadBuffer(std::size_t size);
    ComPtr<ID3D12Resource> uploadBuffer(const void* data, std::size_t size);
    void updateFrameConstants(const ViewerScene& scene, const DirectX::XMFLOAT4X4& view, float windPhase);
    unsigned drawShadowMap(const ViewerScene& scene);
    unsigned drawMeshes(const ViewerScene& scene, bool shadowPass);
    void drawMesh(const GpuMesh& mesh, const MaterialData& material, bool wind,
                  bool receiveShadows, UINT instanceCount, UINT firstInstance);
    void createFrameTimer();
    std::optional<double> readGpuTime() const;
    void createHud();
    void drawHud(D3D12_CPU_DESCRIPTOR_HANDLE target, std::string_view text);
    void waitForGpu();

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> renderTargetHeap_;
    std::array<ComPtr<ID3D12Resource>, bufferCount> backBuffers_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, bufferCount> renderTargets_{};
    ComPtr<ID3D12DescriptorHeap> depthHeap_;
    ComPtr<ID3D12Resource> shadowMap_;
    ComPtr<ID3D12PipelineState> shadowPipeline_;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC meshPipelineDescription_{};
    MeshShaders reloadedShaders_;
    int appliedConstantBias_ = 0;
    float appliedSlopeBias_ = 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDepthTarget_{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowTexture_{};
    DirectX::XMFLOAT4X4 lightViewProjection_;
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
    std::vector<GpuMesh> meshes_;
    GpuMesh ground_;
    ComPtr<ID3D12DescriptorHeap> textureHeap_;
    std::vector<ComPtr<ID3D12Resource>> textures_;
    UINT textureDescriptorSize_ = 0;
    DirectX::XMFLOAT4X4 projection_;
    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    ComPtr<ID3D12QueryHeap> timestampQueries_;
    ComPtr<ID3D12Resource> timestampReadback_;
    UINT64 timestampFrequency_ = 0;
    ComPtr<ID3D12RootSignature> hudRoot_;
    ComPtr<ID3D12PipelineState> hudPipeline_;
    ComPtr<ID3D12Resource> hudGlyphs_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fenceValue_ = 0;
    std::unique_ptr<void, decltype(&CloseHandle)> fenceEvent_{nullptr, CloseHandle};
};
