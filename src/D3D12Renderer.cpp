#include "D3D12Renderer.h"
#include "MeshPS.h"
#include "MeshVS.h"
#include "ShadowPS.h"
#include "ShadowVS.h"
#include "HudVS.h"
#include "HudPS.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace DirectX;

namespace
{
constexpr DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT colorTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr UINT shadowMapSize = 2048;
constexpr DXGI_SAMPLE_DESC msaaSamples{4, 0};
constexpr float clearColor[] = {0.30f, 0.20f, 0.17f, 1.0f};

D3D12_SHADER_BYTECODE bytecode(const std::vector<std::byte>& shader)
{
    return {shader.data(), shader.size()};
}

struct DrawConstants
{
    XMFLOAT4X4 model;
    XMFLOAT4X4 normalToWorld;
    XMFLOAT4 surfaceColor;
    UINT textureCoordinateSet;
    UINT hasColorTexture;
    float alphaCutoff;
    float transmission;
    UINT movesWithWind;
    UINT receivesShadows;
};

struct FrameConstants
{
    XMFLOAT4X4 viewProjection;
    XMFLOAT4X4 lightViewProjection;
    XMFLOAT3 toSun;
    float shadowBias;
    XMFLOAT2 windOffset;
    float windStart;
    float inverseWindHeight;
    float normalBiasWorld;
};

constexpr UINT drawConstantCount = sizeof(DrawConstants) / sizeof(float);
static_assert(drawConstantCount == 42);
static_assert(sizeof(FrameConstants) == 164);
static_assert(offsetof(FrameConstants, normalBiasWorld) == 160);
static_assert(sizeof(FrameConstants) <= D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

struct HudRect
{
    XMFLOAT4 rectangle;
    XMFLOAT4 color;
};

constexpr std::size_t maxHudCharacters = 64;
constexpr std::size_t maxHudRects = 1 + maxHudCharacters * 5 * 7;

std::array<unsigned, 7> glyphRows(char character)
{
    switch (character)
    {
    case '0': return {14, 17, 19, 21, 25, 17, 14};
    case '1': return {4, 12, 4, 4, 4, 4, 14};
    case '2': return {14, 17, 1, 2, 4, 8, 31};
    case '3': return {30, 1, 1, 14, 1, 1, 30};
    case '4': return {2, 6, 10, 18, 31, 2, 2};
    case '5': return {31, 16, 16, 30, 1, 1, 30};
    case '6': return {14, 16, 16, 30, 17, 17, 14};
    case '7': return {31, 1, 2, 4, 8, 8, 8};
    case '8': return {14, 17, 17, 14, 17, 17, 14};
    case '9': return {14, 17, 17, 15, 1, 1, 14};
    case 'c': return {0, 0, 14, 16, 16, 16, 14};
    case 'f': return {6, 8, 8, 28, 8, 8, 8};
    case 'g': return {0, 0, 15, 17, 15, 1, 14};
    case 'm': return {0, 0, 26, 21, 21, 21, 21};
    case 'p': return {0, 0, 30, 17, 30, 16, 16};
    case 's': return {0, 0, 15, 16, 14, 1, 30};
    case 'u': return {0, 0, 17, 17, 17, 17, 15};
    case '.': return {0, 0, 0, 0, 0, 4, 4};
    case '-': return {0, 0, 0, 14, 0, 0, 0};
    default: return {};
    }
}

void check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        throw std::runtime_error(std::format("{} failed (0x{:08X}).",
                                             operation, static_cast<unsigned long>(result)));
    }
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> createPipeline(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& description, const char* operation)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12InfoQueue> queue;
    device->QueryInterface(IID_PPV_ARGS(&queue));
    const auto first = queue ? queue->GetNumStoredMessagesAllowedByRetrievalFilter() : 0;
    ComPtr<ID3D12PipelineState> pipeline;
    const auto result = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result) && queue)
    {
        std::string detail = operation;
        const auto end = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (auto index = first; index < end; ++index)
        {
            SIZE_T size = 0;
            if (FAILED(queue->GetMessage(index, nullptr, &size)) || size == 0) continue;
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue->GetMessage(index, message, &size)))
            {
                detail += "\n";
                detail += message->pDescription;
            }
        }
        check(result, detail.c_str());
    }
    check(result, operation);
    return pipeline;
}

bool supportsSampleCount(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{};
    levels.Format = format;
    levels.SampleCount = msaaSamples.Count;
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                 &levels, sizeof(levels))) &&
           levels.NumQualityLevels > msaaSamples.Quality;
}

void transitionResource(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &barrier);
}
}

D3D12Renderer::D3D12Renderer(HWND window)
{
    UINT factoryFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "Create DXGI factory");
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
          "Create D3D12 device");

    D3D12_FEATURE_DATA_FORMAT_SUPPORT colorSupport{colorFormat};
    const bool canResolve =
        SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                               &colorSupport, sizeof(colorSupport))) &&
        (colorSupport.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) != 0;
    const bool canUseMsaa = canResolve && supportsSampleCount(device_.Get(), colorFormat) &&
                            supportsSampleCount(device_.Get(), depthFormat);

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device_->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue_)),
          "Create graphics queue");
    createFrameTimer();

    DXGI_SWAP_CHAIN_DESC1 swapDescription{};
    swapDescription.Format = colorFormat;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = bufferCount;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain;
    check(factory->CreateSwapChainForHwnd(queue_.Get(), window, &swapDescription,
                                         nullptr, nullptr, &swapChain),
          "Create swap chain");
    check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER),
          "Configure window association");
    check(swapChain.As(&swapChain_), "Get swap chain interface");

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.NumDescriptors = bufferCount + (canUseMsaa ? 1 : 0);
    check(device_->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&renderTargetHeap_)),
          "Create render target descriptor heap");

    auto target = renderTargetHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT descriptorSize = device_->GetDescriptorHandleIncrementSize(heapDescription.Type);
    for (UINT index = 0; index < bufferCount; ++index)
    {
        check(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffers_[index])),
              "Get swap chain buffer");
        device_->CreateRenderTargetView(backBuffers_[index].Get(), nullptr, target);
        renderTargets_[index] = target;
        target.ptr += descriptorSize;
    }

    const auto backBuffer = backBuffers_[0]->GetDesc();
    viewport_.Width = static_cast<float>(backBuffer.Width);
    viewport_.Height = static_cast<float>(backBuffer.Height);
    viewport_.MaxDepth = 1.0f;
    scissor_.right = static_cast<LONG>(backBuffer.Width);
    scissor_.bottom = static_cast<LONG>(backBuffer.Height);
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDescription.NumDescriptors = canUseMsaa ? 3 : 2;
    check(device_->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&depthHeap_)),
          "Create depth descriptor heap");
    singleSample_.depthTarget = depthHeap_->GetCPUDescriptorHandleForHeapStart();
    createDepthBuffer(singleSample_, backBuffer);
    const UINT depthDescriptorSize = device_->GetDescriptorHandleIncrementSize(heapDescription.Type);
    shadowDepthTarget_ = singleSample_.depthTarget;
    shadowDepthTarget_.ptr += (heapDescription.NumDescriptors - 1) * depthDescriptorSize;

    if (canUseMsaa)
    {
        // The swap chain is single-sampled. Draw to this separate color target before resolving.
        auto multisampleDescription = backBuffer;
        multisampleDescription.SampleDesc = msaaSamples;
        multisampleDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_CLEAR_VALUE clear{colorFormat, {clearColor[0], clearColor[1],
                                                   clearColor[2], clearColor[3]}};
        multisampleColor_ = createRenderTexture(multisampleDescription,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, clear);
        multisampleTarget_ = target;
        device_->CreateRenderTargetView(multisampleColor_.Get(), nullptr, multisampleTarget_);

        multisample_.depthTarget = singleSample_.depthTarget;
        multisample_.depthTarget.ptr += depthDescriptorSize;
        createDepthBuffer(multisample_, multisampleDescription);
    }
    createShadowMap();
    createMeshPipelineLayout();
    installMeshPipelines({meshVS, sizeof(meshVS)}, {meshPS, sizeof(meshPS)},
                         {shadowVS, sizeof(shadowVS)}, {shadowPS, sizeof(shadowPS)}, {}, canUseMsaa);
    createHud();
    frameConstants_ = createUploadBuffer(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    msaaEnabled_ = canUseMsaa;

    check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)),
          "Create command allocator");
    check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(),
                                     nullptr, IID_PPV_ARGS(&commands_)),
          "Create command list");
    check(commands_->Close(), "Close initial command list");
    check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Create fence");

    fenceEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!fenceEvent_)
    {
        check(HRESULT_FROM_WIN32(GetLastError()), "Create fence event");
    }
}

bool D3D12Renderer::supportsMsaa() const
{
    return multisample_.pipeline.Get() != nullptr;
}

bool D3D12Renderer::msaaEnabled() const
{
    return msaaEnabled_;
}

void D3D12Renderer::toggleMsaa()
{
    if (supportsMsaa())
    {
        msaaEnabled_ = !msaaEnabled_;
    }
}

DXGI_FORMAT D3D12Renderer::backBufferFormat() const
{
    return colorFormat;
}

void D3D12Renderer::setScene(const ViewerScene& scene)
{
    // Each frame and texture upload has completed before the viewer can switch scenes.
    if (asset_ != scene.asset)
    {
        std::vector<GpuMesh> meshes;
        meshes.reserve(scene.asset->meshes.size());
        for (const auto& mesh : scene.asset->meshes)
        {
            meshes.push_back(uploadMesh(mesh));
        }
        createTextures(scene.asset->textures);
        meshes_ = std::move(meshes);
        asset_ = scene.asset;
    }
    ground_ = uploadMesh(scene.ground);
    std::vector<XMFLOAT4> placements{{0.0f, 0.0f, 0.0f, 1.0f}};
    for (const auto& object : scene.objects)
    {
        placements.push_back(object.placement);
    }
    const UINT bytes = static_cast<UINT>(placements.size() * sizeof(XMFLOAT4));
    instances_ = uploadBuffer(placements.data(), bytes);
    instanceView_ = {instances_->GetGPUVirtualAddress(), bytes, sizeof(XMFLOAT4)};
    const float aspect = viewport_.Width / viewport_.Height;
    XMStoreFloat4x4(&projection_, XMMatrixPerspectiveFovRH(XM_PI / 4, aspect, 0.05f, scene.farPlane()));

    const auto& bounds = scene.assetBounds;
    const float height = 2.0f * bounds.halfExtent.y;
    windStart_ = bounds.center.y - 0.38f * height;
    inverseWindHeight_ = scene.foliage && height > 0.0f ? 1.0f / (0.88f * height) : 0.0f;
    windAmplitude_ = scene.foliage ? 0.05f * height : 0.0f;
    updateSun(scene);
}

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::createRenderTexture(
    const D3D12_RESOURCE_DESC& description, D3D12_RESOURCE_STATES initialState,
    const D3D12_CLEAR_VALUE& clear)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> texture;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                            initialState, &clear, IID_PPV_ARGS(&texture)),
          "Create render texture");
    return texture;
}

void D3D12Renderer::createDepthBuffer(RenderMode& mode, D3D12_RESOURCE_DESC description)
{
    description.Format = depthFormat;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = depthFormat;
    clear.DepthStencil.Depth = 1.0f;
    mode.depthBuffer = createRenderTexture(description, D3D12_RESOURCE_STATE_DEPTH_WRITE, clear);
    device_->CreateDepthStencilView(mode.depthBuffer.Get(), nullptr, mode.depthTarget);
}

void D3D12Renderer::updateSun(const ViewerScene& scene)
{
    toSun_ = scene.sunDirection();
    // Fit the light to conservative placement bounds. A sun edit needs no GPU resource rebuild.
    const auto rotation = XMMatrixLookToRH(XMVectorZero(), -XMLoadFloat3(&toSun_),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    auto minimum = XMVectorReplicate(std::numeric_limits<float>::max());
    auto maximum = -minimum;
    const auto includeLightPoint = [&](FXMVECTOR world) {
        const auto point = XMVector3TransformCoord(world, rotation);
        minimum = XMVectorMin(minimum, point);
        maximum = XMVectorMax(maximum, point);
    };
    for (const auto& vertex : scene.ground.vertices)
    {
        includeLightPoint(XMLoadFloat3(&vertex.position));
    }
    float maxInstanceScale = 1.0f;
    const auto& [center, half] = scene.assetBounds;
    for (const auto& object : scene.objects)
    {
        const auto& instance = object.placement;
        maxInstanceScale = std::max(maxInstanceScale, instance.w);
        const auto placement = XMMatrixScaling(instance.w, instance.w, instance.w) *
                               XMMatrixRotationY(instance.z) *
                               XMMatrixTranslation(instance.x, 0.0f, instance.y);
        for (float x : {center.x - half.x, center.x + half.x})
        {
            for (float y : {center.y - half.y, center.y + half.y})
            {
                for (float z : {center.z - half.z, center.z + half.z})
                {
                    includeLightPoint(XMVector3TransformCoord(XMVectorSet(x, y, z, 1), placement));
                }
            }
        }
    }
    XMFLOAT3 lower;
    XMFLOAT3 upper;
    XMStoreFloat3(&lower, minimum);
    XMStoreFloat3(&upper, maximum);
    const float span = std::max(upper.x - lower.x, upper.y - lower.y);
    const float padding = std::max(0.25f, span * 0.025f) +
                          windAmplitude_ * maxWindStrength * maxInstanceScale;
    constexpr float nearPlane = 1.0f;
    const float depthSpan = upper.z - lower.z + 2.0f * padding;
    const auto view = rotation * XMMatrixTranslation(0.0f, 0.0f, -upper.z - padding - nearPlane);
    const auto projection = XMMatrixOrthographicOffCenterRH(
        lower.x - padding, upper.x + padding, lower.y - padding, upper.y + padding,
        nearPlane, nearPlane + depthSpan);
    XMStoreFloat4x4(&lightViewProjection_, view * projection);
    shadowWorldUnitsPerTexel_ = (span + 2.0f * padding) / shadowMapSize;
    shadowDepthPerTexel_ = shadowWorldUnitsPerTexel_ / depthSpan;
}

void D3D12Renderer::createShadowMap()
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = shadowMapSize;
    description.Height = shadowMapSize;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = depthFormat;
    clear.DepthStencil.Depth = 1.0f;
    shadowMap_ = createRenderTexture(description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, clear);

    D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
    depthView.Format = depthFormat;
    depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowMap_.Get(), &depthView, shadowDepthTarget_);
}

void D3D12Renderer::createMeshPipelineLayout()
{
    D3D12_DESCRIPTOR_RANGE colorTexture{};
    colorTexture.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorTexture.NumDescriptors = 1;
    auto shadowTexture = colorTexture;
    shadowTexture.BaseShaderRegister = 1;

    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = drawConstantCount;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &colorTexture};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {1, &shadowTexture};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    auto shadowSampler = sampler;
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.ShaderRegister = 1;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {sampler, shadowSampler};

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = static_cast<UINT>(std::size(parameters));
    rootDescription.pParameters = parameters;
    rootDescription.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rootDescription.pStaticSamplers = samplers;
    rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serializedRoot;
    check(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &serializedRoot, nullptr), "Serialize mesh root signature");
    check(device_->CreateRootSignature(0, serializedRoot->GetBufferPointer(),
                                       serializedRoot->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)),
          "Create mesh root signature");

    static constexpr D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texcoords),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texcoords) + sizeof(XMFLOAT2),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    auto& description = meshPipelineDescription_;
    description.pRootSignature = rootSignature_.Get();
    description.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DSVFormat = depthFormat;
    description.SampleMask = UINT_MAX;
    description.SampleDesc.Count = 1;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = colorFormat;
}

void D3D12Renderer::installMeshPipelines(D3D12_SHADER_BYTECODE cameraVertex, D3D12_SHADER_BYTECODE cameraPixel,
                                         D3D12_SHADER_BYTECODE shadowVertex, D3D12_SHADER_BYTECODE shadowPixel,
                                         const ShadowBiasSettings& bias, bool enableMultisampling)
{
    auto description = meshPipelineDescription_;
    description.VS = cameraVertex;
    description.PS = cameraPixel;
    auto singleSample = createPipeline(device_.Get(), description, "Create single-sample mesh pipeline");
    ComPtr<ID3D12PipelineState> multisample;
    auto shadow = createShadowPipeline(shadowVertex, shadowPixel, bias);
    if (enableMultisampling)
    {
        description.SampleDesc = msaaSamples;
        description.RasterizerState.MultisampleEnable = TRUE;
        multisample = createPipeline(device_.Get(), description, "Create 4x MSAA mesh pipeline");
    }

    // The frame loop fences each submission. Commit the whole set between frames.
    singleSample_.pipeline = std::move(singleSample);
    multisample_.pipeline = std::move(multisample);
    shadowPipeline_ = std::move(shadow);
    appliedConstantBias_ = bias.constant;
    appliedSlopeBias_ = bias.slopeScale;
}

void D3D12Renderer::reloadMeshShaders(MeshShaders shaders, const ShadowBiasSettings& bias)
{
    installMeshPipelines(bytecode(shaders.cameraVertex), bytecode(shaders.cameraPixel),
                         bytecode(shaders.shadowVertex), bytecode(shaders.shadowPixel), bias, supportsMsaa());
    reloadedShaders_ = std::move(shaders);
}

D3D12Renderer::ComPtr<ID3D12PipelineState> D3D12Renderer::createShadowPipeline(
    D3D12_SHADER_BYTECODE vertex, D3D12_SHADER_BYTECODE pixel, const ShadowBiasSettings& bias)
{
    auto description = meshPipelineDescription_;
    description.VS = vertex;
    description.PS = pixel;
    description.NumRenderTargets = 0;
    description.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    description.RasterizerState.DepthBias = bias.constant;
    description.RasterizerState.SlopeScaledDepthBias = bias.slopeScale;
    description.RasterizerState.DepthBiasClamp = ShadowBiasSettings::drawClamp;
    return createPipeline(device_.Get(), description, "Create shadow pipeline");
}

void D3D12Renderer::updateShadowPipeline(const ShadowBiasSettings& bias)
{
    if (appliedConstantBias_ == bias.constant && appliedSlopeBias_ == bias.slopeScale)
    {
        return;
    }

    const auto vertex = reloadedShaders_.shadowVertex.empty() ? D3D12_SHADER_BYTECODE{shadowVS, sizeof(shadowVS)}
                                                              : bytecode(reloadedShaders_.shadowVertex);
    const auto pixel = reloadedShaders_.shadowPixel.empty() ? D3D12_SHADER_BYTECODE{shadowPS, sizeof(shadowPS)}
                                                            : bytecode(reloadedShaders_.shadowPixel);
    shadowPipeline_ = createShadowPipeline(vertex, pixel, bias);
    appliedConstantBias_ = bias.constant;
    appliedSlopeBias_ = bias.slopeScale;
}

void D3D12Renderer::createTextures(std::span<const TextureAsset> textures)
{
    check(allocator_->Reset(), "Reset texture upload allocator");
    check(commands_->Reset(allocator_.Get(), nullptr), "Reset texture upload list");
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = static_cast<UINT>(textures.size() + 2);
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&textureHeap_)), "Create texture descriptor heap");
    textureDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(heap.Type);

    auto handle = textureHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = colorTextureFormat;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(nullptr, &view, handle);

    std::vector<ComPtr<ID3D12Resource>> staging;
    textures_.clear();
    textures_.reserve(textures.size());
    staging.reserve(textures.size());
    for (const auto& texture : textures)
    {
        const auto& mips = texture.mips;
        textures_.push_back(uploadTexture(mips, staging));
        handle.ptr += textureDescriptorSize_;
        view.Texture2D.MipLevels = static_cast<UINT>(mips.size());
        device_->CreateShaderResourceView(textures_.back().Get(), &view, handle);
    }
    handle.ptr += textureDescriptorSize_;
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(shadowMap_.Get(), &view, handle);
    shadowTexture_ = textureHeap_->GetGPUDescriptorHandleForHeapStart();
    shadowTexture_.ptr += static_cast<UINT64>(textures.size() + 1) * textureDescriptorSize_;
    check(commands_->Close(), "Close texture upload command list");
    if (!staging.empty())
    {
        ID3D12CommandList* lists[] = {commands_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        waitForGpu();
    }
}

void D3D12Renderer::writeTextureView(std::size_t texture, D3D12_CPU_DESCRIPTOR_HANDLE destination) const
{
    const auto& resource = textures_.at(texture);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    // The inspector composites display RGB, so display the encoded texture values.
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = resource->GetDesc().MipLevels;
    device_->CreateShaderResourceView(resource.Get(), &view, destination);
}

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::uploadTexture(
    const TextureData& mips, std::vector<ComPtr<ID3D12Resource>>& staging)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = mips.front().width;
    description.Height = mips.front().height;
    description.DepthOrArraySize = 1;
    description.MipLevels = static_cast<UINT16>(mips.size());
    description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&texture)), "Create color texture");

    // D3D12 pads each row and subresource to device copy alignment.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(mips.size());
    UINT64 uploadSize = 0;
    device_->GetCopyableFootprints(&description, 0, description.MipLevels, 0,
                                   layouts.data(), nullptr, nullptr, &uploadSize);
    auto upload = createUploadBuffer(static_cast<std::size_t>(uploadSize));
    const D3D12_RANGE noCpuReads{0, 0};
    void* mapped = nullptr;
    check(upload->Map(0, &noCpuReads, &mapped), "Map color texture upload");
    for (UINT level = 0; level < description.MipLevels; ++level)
    {
        const auto& mip = mips[level];
        const auto& layout = layouts[level];
        const auto rowBytes = static_cast<std::size_t>(mip.width) * 4;
        auto* destination = static_cast<std::uint8_t*>(mapped) + layout.Offset;
        for (std::size_t row = 0; row < mip.height; ++row)
        {
            std::memcpy(destination + row * layout.Footprint.RowPitch,
                        mip.rgba.data() + row * rowBytes, rowBytes);
        }

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = layout;
        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = texture.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = level;
        commands_->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    }
    upload->Unmap(0, nullptr);
    transitionResource(commands_.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    staging.push_back(std::move(upload));
    return texture;
}

D3D12Renderer::GpuMesh D3D12Renderer::uploadMesh(const MeshData& mesh)
{
    GpuMesh gpu{};
    const auto vertexBytes = mesh.vertices.size() * sizeof(Vertex);
    const auto indexBytes = mesh.indices.size() * sizeof(std::uint32_t);
    gpu.vertexBuffer = uploadBuffer(mesh.vertices.data(), vertexBytes);
    gpu.indexBuffer = uploadBuffer(mesh.indices.data(), indexBytes);
    gpu.vertices.BufferLocation = gpu.vertexBuffer->GetGPUVirtualAddress();
    gpu.vertices.StrideInBytes = sizeof(Vertex);
    gpu.vertices.SizeInBytes = static_cast<UINT>(vertexBytes);
    gpu.indices.BufferLocation = gpu.indexBuffer->GetGPUVirtualAddress();
    gpu.indices.Format = DXGI_FORMAT_R32_UINT;
    gpu.indices.SizeInBytes = static_cast<UINT>(indexBytes);
    gpu.indexCount = static_cast<UINT>(mesh.indices.size());
    gpu.localToWorld = mesh.localToWorld;
    const auto world = XMLoadFloat4x4(&mesh.localToWorld);
    XMStoreFloat4x4(&gpu.normalToWorld, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
    gpu.material = mesh.material;
    return gpu;
}

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::createUploadBuffer(std::size_t size)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&resource)),
          "Create upload buffer");
    return resource;
}

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::uploadBuffer(const void* data, std::size_t size)
{
    auto resource = createUploadBuffer(size);
    const D3D12_RANGE noCpuReads{0, 0};
    void* mapped = nullptr;
    check(resource->Map(0, &noCpuReads, &mapped), "Map mesh buffer");
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
    return resource;
}

void D3D12Renderer::updateFrameConstants(const ViewerScene& scene, const XMFLOAT4X4& view, float windPhase)
{
    FrameConstants constants{};
    XMStoreFloat4x4(&constants.viewProjection, XMLoadFloat4x4(&view) * XMLoadFloat4x4(&projection_));
    constants.lightViewProjection = lightViewProjection_;
    constants.toSun = toSun_;
    constants.shadowBias = scene.shadowBias.samplingTexels * shadowDepthPerTexel_;
    constants.normalBiasWorld = scene.shadowBias.normalTexels * shadowWorldUnitsPerTexel_;
    // Harmonics give the whole tree a shared gust with a small crosswind component.
    const float gust = 0.70f * std::sin(windPhase) + 0.16f * std::sin(2.0f * windPhase + 0.55f);
    const float crosswind = 0.14f * std::sin(3.0f * windPhase + 0.7f);
    const float amplitude = windAmplitude_ * scene.windStrength;
    constants.windOffset = {amplitude * (0.94f * gust - 0.34f * crosswind),
                            amplitude * (0.34f * gust + 0.94f * crosswind)};
    constants.windStart = windStart_;
    constants.inverseWindHeight = inverseWindHeight_;

    const D3D12_RANGE noCpuReads{0, 0};
    const D3D12_RANGE written{0, sizeof(constants)};
    void* mapped = nullptr;
    check(frameConstants_->Map(0, &noCpuReads, &mapped), "Map frame constants");
    std::memcpy(mapped, &constants, sizeof(constants));
    frameConstants_->Unmap(0, &written);
}

D3D12Renderer::FrameTiming D3D12Renderer::drawFrame(
    const ViewerScene& scene, const XMFLOAT4X4& view, float windPhase, std::string_view hud,
    const std::function<void(ID3D12GraphicsCommandList*)>& drawOverlay)
{
    using Clock = std::chrono::steady_clock;
    const auto cpuStart = Clock::now();
    updateShadowPipeline(scene.shadowBias);
    updateFrameConstants(scene, view, windPhase);
    const UINT bufferIndex = swapChain_->GetCurrentBackBufferIndex();
    auto* backBuffer = backBuffers_[bufferIndex].Get();
    const auto& mode = msaaEnabled_ ? multisample_ : singleSample_;
    const auto colorTarget = msaaEnabled_ ? multisampleTarget_ : renderTargets_[bufferIndex];
    check(allocator_->Reset(), "Reset command allocator");
    check(commands_->Reset(allocator_.Get(), shadowPipeline_.Get()), "Reset command list");
    if (timestampQueries_)
    {
        commands_->EndQuery(timestampQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    }

    commands_->SetGraphicsRootSignature(rootSignature_.Get());
    commands_->SetGraphicsRootConstantBufferView(3, frameConstants_->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = {textureHeap_.Get()};
    commands_->SetDescriptorHeaps(1, heaps);
    commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const unsigned shadowDraws = drawShadowMap(scene);
    commands_->SetPipelineState(mode.pipeline.Get());
    commands_->SetGraphicsRootDescriptorTable(2, shadowTexture_);

    if (!msaaEnabled_)
    {
        transitionResource(commands_.Get(), backBuffer, D3D12_RESOURCE_STATE_PRESENT,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    commands_->ClearRenderTargetView(colorTarget, clearColor, 0, nullptr);
    commands_->ClearDepthStencilView(mode.depthTarget, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commands_->RSSetViewports(1, &viewport_);
    commands_->RSSetScissorRects(1, &scissor_);
    commands_->OMSetRenderTargets(1, &colorTarget, FALSE, &mode.depthTarget);
    const unsigned cameraDraws = drawMeshes(scene, false);

    auto backBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (msaaEnabled_)
    {
        auto* multisampleColor = multisampleColor_.Get();
        transitionResource(commands_.Get(), multisampleColor, D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        backBufferState = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        transitionResource(commands_.Get(), backBuffer, D3D12_RESOURCE_STATE_PRESENT, backBufferState);
        commands_->ResolveSubresource(backBuffer, 0, multisampleColor, 0, colorFormat);
        transitionResource(commands_.Get(), multisampleColor, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (msaaEnabled_)
    {
        transitionResource(commands_.Get(), backBuffer, backBufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    drawHud(renderTargets_[bufferIndex], hud);
    if (drawOverlay)
    {
        drawOverlay(commands_.Get());
    }
    transitionResource(commands_.Get(), backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PRESENT);
    if (timestampQueries_)
    {
        commands_->EndQuery(timestampQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        commands_->ResolveQueryData(timestampQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                    timestampReadback_.Get(), 0);
    }
    check(commands_->Close(), "Close command list");

    ID3D12CommandList* lists[] = {commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const double cpuMs = std::chrono::duration<double, std::milli>(Clock::now() - cpuStart).count();
    const HRESULT presentResult = swapChain_->Present(1, 0);

    // Keep one frame in flight so we can safely reuse this allocator next frame.
    waitForGpu();
    check(presentResult, "Present frame");
    return {cpuMs, readGpuTime(), shadowDraws + cameraDraws + 1};
}

unsigned D3D12Renderer::drawShadowMap(const ViewerScene& scene)
{
    transitionResource(commands_.Get(), shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commands_->ClearDepthStencilView(shadowDepthTarget_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, shadowMapSize, shadowMapSize, 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, shadowMapSize, shadowMapSize};
    commands_->RSSetViewports(1, &viewport);
    commands_->RSSetScissorRects(1, &scissor);
    commands_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDepthTarget_);
    const unsigned draws = drawMeshes(scene, true);
    transitionResource(commands_.Get(), shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return draws;
}

unsigned D3D12Renderer::drawMeshes(const ViewerScene& scene, bool shadowPass)
{
    commands_->IASetVertexBuffers(1, 1, &instanceView_);
    const UINT count = scene.kind == SceneKind::Grove ? scene.treeCount : 1u;
    unsigned draws = 0;
    for (std::size_t i = 0; i < meshes_.size(); ++i)
    {
        const auto& flags = scene.meshShadows[i];
        if (!shadowPass || flags.cast)
        {
            const auto& mesh = meshes_[i];
            drawMesh(mesh, scene.asset->materials[mesh.material], scene.foliage, flags.receive, count, 1);
            ++draws;
        }
    }
    if (!shadowPass || scene.groundShadows.cast)
    {
        drawMesh(ground_, scene.groundMaterial, false, scene.groundShadows.receive, 1, 0);
        ++draws;
    }
    return draws;
}

void D3D12Renderer::drawMesh(const GpuMesh& mesh, const MaterialData& material, bool wind,
                             bool receiveShadows, UINT instanceCount, UINT firstInstance)
{
    DrawConstants constants{};
    constants.model = mesh.localToWorld;
    constants.normalToWorld = mesh.normalToWorld;
    constants.surfaceColor = material.color;
    constants.textureCoordinateSet = material.textureCoordinateSet;
    constants.hasColorTexture = material.colorTexture.has_value();
    constants.alphaCutoff = material.alphaCutoff;
    constants.transmission = material.transmission;
    constants.movesWithWind = wind;
    constants.receivesShadows = receiveShadows;
    commands_->SetGraphicsRoot32BitConstants(0, drawConstantCount, &constants, 0);
    auto texture = textureHeap_->GetGPUDescriptorHandleForHeapStart();
    const auto index = material.colorTexture ? *material.colorTexture + 1 : 0;
    texture.ptr += index * textureDescriptorSize_;
    commands_->SetGraphicsRootDescriptorTable(1, texture);
    commands_->IASetVertexBuffers(0, 1, &mesh.vertices);
    commands_->IASetIndexBuffer(&mesh.indices);
    commands_->DrawIndexedInstanced(mesh.indexCount, instanceCount, 0, 0, firstInstance);
}

void D3D12Renderer::waitForGpu()
{
    const UINT64 submittedFence = ++fenceValue_;
    check(queue_->Signal(fence_.Get(), submittedFence), "Signal frame fence");
    check(fence_->SetEventOnCompletion(submittedFence, fenceEvent_.get()), "Set frame completion event");
    if (WaitForSingleObject(fenceEvent_.get(), INFINITE) != WAIT_OBJECT_0)
    {
        check(HRESULT_FROM_WIN32(GetLastError()), "Wait for frame completion");
    }
}

void D3D12Renderer::createFrameTimer()
{
    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = 2;
    if (FAILED(queue_->GetTimestampFrequency(&timestampFrequency_)) || timestampFrequency_ == 0 ||
        FAILED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&timestampQueries_))))
    {
        return;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 2 * sizeof(UINT64);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&timestampReadback_))))
    {
        timestampQueries_.Reset();
    }
}

std::optional<double> D3D12Renderer::readGpuTime() const
{
    if (!timestampReadback_)
    {
        return std::nullopt;
    }
    const D3D12_RANGE readRange{0, 2 * sizeof(UINT64)};
    void* mapped = nullptr;
    if (FAILED(timestampReadback_->Map(0, &readRange, &mapped)))
    {
        return std::nullopt;
    }
    const auto* ticks = static_cast<const UINT64*>(mapped);
    const UINT64 elapsed = ticks[1] - ticks[0];
    const D3D12_RANGE noWrites{0, 0};
    timestampReadback_->Unmap(0, &noWrites);
    return 1000.0 * static_cast<double>(elapsed) / static_cast<double>(timestampFrequency_);
}

void D3D12Renderer::createHud()
{
    D3D12_ROOT_PARAMETER size{};
    size.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    size.Constants.Num32BitValues = 2;
    size.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 1;
    root.pParameters = &size;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
          "Serialize HUD root signature");
    check(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&hudRoot_)), "Create HUD root signature");

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(HudRect, rectangle),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(HudRect, color),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = hudRoot_.Get();
    pipeline.VS = {hudVS, sizeof(hudVS)};
    pipeline.PS = {hudPS, sizeof(hudPS)};
    pipeline.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    auto& blend = pipeline.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.SampleDesc.Count = 1;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = colorFormat;
    check(device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&hudPipeline_)), "Create HUD pipeline");
    hudGlyphs_ = createUploadBuffer(maxHudRects * sizeof(HudRect));
}

void D3D12Renderer::drawHud(D3D12_CPU_DESCRIPTOR_HANDLE target, std::string_view text)
{
    text = text.substr(0, maxHudCharacters);
    const float scale = std::clamp(std::floor(viewport_.Height / 600.0f), 1.0f, 3.0f);
    const float margin = 8.0f * scale;
    const float padding = 4.0f * scale;
    const float width = static_cast<float>(text.size() * 6) * scale + padding * 2.0f;
    const float left = std::max(0.0f, viewport_.Width - width - margin);
    const D3D12_RANGE noReads{0, 0};
    void* mapped = nullptr;
    check(hudGlyphs_->Map(0, &noReads, &mapped), "Map HUD buffer");
    auto* rects = static_cast<HudRect*>(mapped);
    UINT count = 0;
    rects[count++] = {{left, margin, width, 7.0f * scale + padding * 2.0f}, {0.04f, 0.07f, 0.10f, 0.82f}};
    float pen = left + padding;
    for (char character : text)
    {
        const auto rows = glyphRows(character);
        for (std::size_t row = 0; row < rows.size(); ++row)
        {
            for (unsigned column = 0; column < 5; ++column)
            {
                if (rows[row] & (16u >> column))
                {
                    rects[count++] = {{pen + static_cast<float>(column) * scale,
                                       margin + padding + static_cast<float>(row) * scale, scale, scale},
                                      {0.91f, 0.95f, 0.98f, 1.0f}};
                }
            }
        }
        pen += 6.0f * scale;
    }
    const UINT bytes = count * sizeof(HudRect);
    const D3D12_RANGE writes{0, bytes};
    hudGlyphs_->Unmap(0, &writes);

    commands_->SetPipelineState(hudPipeline_.Get());
    commands_->SetGraphicsRootSignature(hudRoot_.Get());
    const float inverseSize[] = {1.0f / viewport_.Width, 1.0f / viewport_.Height};
    commands_->SetGraphicsRoot32BitConstants(0, 2, inverseSize, 0);
    commands_->OMSetRenderTargets(1, &target, FALSE, nullptr);
    commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    const D3D12_VERTEX_BUFFER_VIEW view{hudGlyphs_->GetGPUVirtualAddress(), bytes, sizeof(HudRect)};
    commands_->IASetVertexBuffers(0, 1, &view);
    commands_->DrawInstanced(4, count, 0, 0);
}
