#include "D3D12Renderer.h"
#include "MeshPS.h"
#include "OakFlatPS.h"
#include "OakEarlyPS.h"
#include "OakVisibilityPS.h"
#include "OakVisibilityVS.h"
#include "MeshVS.h"
#include "ShadowPS.h"
#include "ShadowVS.h"
#include "HudVS.h"
#include "HudPS.h"
#include "VoxelVS.h"
#include "BrickClearCS.h"
#include "BrickVisibilityCS.h"
#include "BrickResolvePS.h"
#include "BrickResolveVS.h"
#include "VoxelPS.h"
#include "VoxelShadowPS.h"
#include "SkyVS.h"
#include "SkyPS.h"
#include "SkyLighting.h"
#include "DisplayVS.h"
#include "DisplayPS.h"
#include "ProbeCS.h"
#include "GroundProbeCS.h"
#include "AdvectProbeCS.h"

#include <d3d12sdklayers.h>
#include <DirectXCollision.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

using namespace DirectX;

namespace
{
constexpr DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT sceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT colorTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr UINT shadowMapSize = 2048;
constexpr DXGI_SAMPLE_DESC msaaSamples{4, 0};
constexpr float clearColor[] = {0.30f, 0.20f, 0.17f, 1.0f};

float srgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

const float sceneClearColor[] = {srgbToLinear(clearColor[0]), srgbToLinear(clearColor[1]),
                                 srgbToLinear(clearColor[2]), 1.0f};

struct SkyConstants
{
    XMFLOAT4X4 viewRayTransform;
    XMFLOAT3 toSun;
    UINT encodedCamera;
    UINT skyOnly;
};

struct SkyIrradianceConstants
{
    SkyIrradiance coefficients;
    std::array<std::byte, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - sizeof(SkyIrradiance)> padding{};
};

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
    int twoSidedNormalSign;
    UINT lodTint;
    UINT highlightedLodGroup;
    UINT firstLodTag;
    UINT lodLevel;
    UINT shadowView;
    UINT usesMeshInstances;
    XMFLOAT3 transmissionColor;
    UINT transmissionFlags;
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
    UINT useCascades;
    UINT showCascadeColors;
    UINT filterCascades;
    XMFLOAT4 cascadeSplits;
    std::array<XMFLOAT4X4, sunCascadeSplits.size()> cascadeViewProjections;
    XMFLOAT4 cascadeShadowBias;
    XMFLOAT4 cascadeNormalBias;
    XMFLOAT4 cascadeBlendWidths;
    XMFLOAT4 cascadeDepthPerTexel;
    XMFLOAT4 lightRight;
    UINT encodedCamera;
    UINT directionalSkylight;
    UINT lightingMode;
    UINT probePadding;
    DiffuseProbeGrid probes;
};

struct VoxelFrameConstants
{
    XMFLOAT4X4 viewProjection;
    XMFLOAT4X4 lightViewProjection;
    XMFLOAT4X4 meshToAsset;
    XMFLOAT4X4 assetToMesh;
    XMFLOAT3 eye;
    float nearPlane;
    float farPlane;
    float projectionZScale;
    float projectionZOffset;
    float shadowBias;
    XMFLOAT3 baseColor;
    UINT useNormals;
    UINT viewMode;
    UINT encodeColor;
    UINT receivesShadows;
    UINT mirrored;
    XMFLOAT3 toSun;
    float transmission;
    UINT shadowReceiver;
    UINT tightBrickProxies;
    UINT encodedCamera;
    UINT directionalSkylight;
    UINT lightingMode;
    XMFLOAT3 transmissionColor;
    UINT thinLeaf;
    UINT orientedBricks;
    UINT probePadding[2];
    DiffuseProbeGrid probes;
    XMFLOAT4 boughWind;
    XMFLOAT4 treePlacement, screenRayX, screenRayY, screenRayForward;
    XMFLOAT2 rasterSize, rasterPadding;
};

struct VoxelDrawConstants
{
    XMFLOAT3 origin;
    float cellSize;
    UINT brickCount;
    UINT firstPlacement;
};

struct GpuVoxelBrick
{
    VoxelCoordinate coordinate;
    std::array<std::uint32_t, 2> occupancy;
    std::uint32_t packedProxyBounds;
};

static_assert(offsetof(VoxelFrameConstants, probes) == 384);
static_assert(sizeof(VoxelDrawConstants) == 24);
static_assert(sizeof(GpuVoxelBrick) == 24);
constexpr std::uint16_t voxelBoxIndices[]{
    0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
    2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};

constexpr UINT drawConstantCount = sizeof(DrawConstants) / sizeof(float);
static_assert(drawConstantCount == 53);

std::array<XMVECTOR, 6> frustumPlanes(FXMMATRIX viewProjection)
{
    // The shader uses row vectors and D3D clip depth is [0, w]. Planes face inward.
    const auto columns = XMMatrixTranspose(viewProjection);
    const auto w = columns.r[3];
    return {w + columns.r[0], w - columns.r[0], w + columns.r[1], w - columns.r[1],
            columns.r[2], w - columns.r[2]};
}

float maxLinearStretch(FXMMATRIX transform, float* minStretch = nullptr)
{
    // Gram matrix row bounds enclose the squared smallest and largest singular values.
    const auto dot = [&](unsigned i, unsigned j) {
        return std::abs(XMVectorGetX(XMVector3Dot(transform.r[i], transform.r[j])));
    };
    const float xx = dot(0, 0), yy = dot(1, 1), zz = dot(2, 2);
    const float xy = dot(0, 1), xz = dot(0, 2), yz = dot(1, 2);
    const float x = xx + xy + xz, y = xy + yy + yz, z = xz + yz + zz;
    if (minStretch)
        *minStretch = std::sqrt(std::max(0.0f, std::min({2.0f * xx - x, 2.0f * yy - y, 2.0f * zz - z})));
    return std::sqrt(std::max({x, y, z}));
}

bool intersectsFrustum(const BoundingBox& box, const std::array<XMVECTOR, 6>& planes)
{
    const auto center = XMVectorSetW(XMLoadFloat3(&box.Center), 1.0f);
    const auto half = XMLoadFloat3(&box.Extents);
    for (const auto plane : planes)
    {
        const auto normalMagnitude = XMVectorAbs(plane);
        const float distance = XMVectorGetX(XMVector4Dot(plane, center));
        const float radius = XMVectorGetX(XMVector3Dot(normalMagnitude, half));
        const float scale = XMVectorGetX(XMVector4Dot(normalMagnitude, XMVectorAbs(center))) + radius;
        const float tolerance = 16.0f * std::numeric_limits<float>::epsilon() * scale;
        if (distance + radius < -tolerance) return false;
    }
    return true;
}

bool insideFrustum(const BoundingBox& box, const std::array<XMVECTOR, 6>& planes)
{
    const auto center = XMVectorSetW(XMLoadFloat3(&box.Center), 1.0f);
    const auto half = XMLoadFloat3(&box.Extents);
    return std::ranges::all_of(planes, [&](auto plane) {
        return XMVectorGetX(XMVector4Dot(plane, center)) >= XMVectorGetX(XMVector3Dot(XMVectorAbs(plane), half));
    });
}

BoundingBox worldWindBounds(const Bounds& bounds, const SceneObject& object, XMFLOAT2 windOffset)
{
    BoundingBox world;
    const BoundingBox local(bounds.center, bounds.halfExtent);
    local.Transform(world, object.placementMatrix());
    // The shader's saturated bend weight moves each vertex by zero to the full offset.
    const float dx = windOffset.x * object.placement.w * 0.5f;
    const float dz = windOffset.y * object.placement.w * 0.5f;
    world.Center.x += dx;
    world.Center.z += dz;
    world.Extents.x += std::abs(dx);
    world.Extents.z += std::abs(dz);
    return world;
}

std::array<XMVECTOR, 6> windExpandedLocalPlanes(FXMMATRIX localToClip, const std::array<XMVECTOR, 6>& worldPlanes,
                                               float instanceScale, XMFLOAT2 windOffset)
{
    auto localPlanes = frustumPlanes(localToClip);
    for (std::size_t p = 0; p < localPlanes.size(); ++p)
    {
        const auto normal = worldPlanes[p];
        const float shift = instanceScale * (XMVectorGetX(normal) * windOffset.x + XMVectorGetZ(normal) * windOffset.y);
        localPlanes[p] = XMVectorSetW(localPlanes[p], XMVectorGetW(localPlanes[p]) + std::max(0.0f, shift));
    }
    return localPlanes;
}

constexpr std::size_t cascadeGroupBlockSize = 16;
constexpr std::uint8_t cascadeCasterVisible = 1;
constexpr std::uint8_t cascadeCasterOverride = 2;

enum FrameTimestamp : UINT
{
    FrameStart, ProbeEnd, ShadowStart, ShadowEnd, AssetStart, LiveCutEnd, AssetEnd, BoughBrickStart, BoughBrickEnd,
    TreeDepthStart, TreeDepthEnd, CameraEnd, FrameEnd, TimestampCount
};
static_assert(offsetof(FrameConstants, probes) == 480);
static_assert(offsetof(FrameConstants, normalBiasWorld) == 160);
static_assert(offsetof(FrameConstants, cascadeViewProjections) == 192);
constexpr std::size_t frameConstantAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr std::size_t frameConstantStride = (sizeof(FrameConstants) + frameConstantAlignment - 1) /
                                           frameConstantAlignment * frameConstantAlignment;

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

bool supportsColorResolve(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) != 0 &&
           supportsSampleCount(device, format);
}

void transitionResource(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commands->ResourceBarrier(1, &barrier);
}
}

D3D12Renderer::D3D12Renderer(HWND window, const std::filesystem::path& skyPanorama, bool cullMeshBackfaces)
    : D3D12Renderer(window, defaultRenderWidth, defaultRenderHeight, skyPanorama, cullMeshBackfaces)
{
}

D3D12Renderer::D3D12Renderer(UINT width, UINT height, const std::filesystem::path& skyPanorama, bool cullMeshBackfaces)
    : D3D12Renderer(nullptr, width, height, skyPanorama, cullMeshBackfaces)
{
}

D3D12Renderer::D3D12Renderer(HWND window, UINT width, UINT height, const std::filesystem::path& skyPanorama, bool cullMeshBackfaces)
    : cullMeshBackfaces_(cullMeshBackfaces)
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
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapterDescription{};
    if (SUCCEEDED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
        SUCCEEDED(adapter->GetDesc1(&adapterDescription)))
    {
        adapterName_ = adapterDescription.Description;
    }

    const bool canUseMsaa = supportsColorResolve(device_.Get(), sceneColorFormat) &&
                            supportsColorResolve(device_.Get(), colorFormat) &&
                            supportsSampleCount(device_.Get(), depthFormat);

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device_->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&queue_)),
          "Create graphics queue");
    createFrameTimer();

    const UINT colorTargetCount = window ? bufferCount : 1;
    if (window)
    {
        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = width;
        swapDescription.Height = height;
        swapDescription.Format = colorFormat;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = bufferCount;
        swapDescription.Scaling = DXGI_SCALING_STRETCH;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> swapChain;
        check(factory->CreateSwapChainForHwnd(queue_.Get(), window, &swapDescription,
                                             nullptr, nullptr, &swapChain), "Create swap chain");
        check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Configure window association");
        check(swapChain.As(&swapChain_), "Get swap chain interface");
        for (UINT index = 0; index < colorTargetCount; ++index)
        {
            check(swapChain_->GetBuffer(index, IID_PPV_ARGS(&backBuffers_[index])), "Get swap chain buffer");
        }
    }
    else
    {
        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = width;
        color.Height = height;
        color.DepthOrArraySize = color.MipLevels = 1;
        color.Format = colorFormat;
        color.SampleDesc.Count = 1;
        color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_CLEAR_VALUE clear{colorFormat, {clearColor[0], clearColor[1], clearColor[2], clearColor[3]}};
        backBuffers_[0] = createRenderTexture(color, D3D12_RESOURCE_STATE_RENDER_TARGET, clear);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDescription.NumDescriptors = colorTargetCount + 1 + (canUseMsaa ? 2 : 0);
    check(device_->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&renderTargetHeap_)),
          "Create render target descriptor heap");

    auto target = renderTargetHeap_->GetCPUDescriptorHandleForHeapStart();
    const UINT descriptorSize = device_->GetDescriptorHandleIncrementSize(heapDescription.Type);
    for (UINT index = 0; index < colorTargetCount; ++index)
    {
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
    sceneTarget_ = target;
    sceneColor_ = createCameraColor(sceneColorFormat, {1, 0}, sceneTarget_,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    const UINT cameraDepthDescriptors = canUseMsaa ? 2 : 1;
    heapDescription.NumDescriptors = cameraDepthDescriptors + 1 + static_cast<UINT>(sunCascadeSplits.size());
    check(device_->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&depthHeap_)),
          "Create depth descriptor heap");
    singleSample_.depthTarget = depthHeap_->GetCPUDescriptorHandleForHeapStart();
    createDepthBuffer(singleSample_, backBuffer);
    const UINT depthDescriptorSize = device_->GetDescriptorHandleIncrementSize(heapDescription.Type);
    shadowDepthTarget_ = singleSample_.depthTarget;
    shadowDepthTarget_.ptr += cameraDepthDescriptors * depthDescriptorSize;
    for (std::size_t i = 0; i < cascadeDepthTargets_.size(); ++i)
        cascadeDepthTargets_[i].ptr = shadowDepthTarget_.ptr + (i + 1) * depthDescriptorSize;

    if (canUseMsaa)
    {
        multisampleTarget_.ptr = target.ptr + descriptorSize;
        encodedMultisampleTarget_.ptr = target.ptr + 2ull * descriptorSize;
        multisample_.depthTarget = singleSample_.depthTarget;
        multisample_.depthTarget.ptr += depthDescriptorSize;
        auto multisampleDescription = backBuffer;
        multisampleDescription.SampleDesc = msaaSamples;
        createDepthBuffer(multisample_, multisampleDescription);
    }
    createShadowMap();
    createMeshPipelineLayout();
    createBranchCommandSignature();
    createTreeCommandSignature();
    createTreeGpuPipelines();
    installMeshPipelines({meshVS, sizeof(meshVS)}, {meshPS, sizeof(meshPS)},
                         {shadowVS, sizeof(shadowVS)}, {shadowPS, sizeof(shadowPS)}, {}, canUseMsaa);
    createVoxelPipeline(canUseMsaa);
    createDisplayPipeline();
    if (window) createHud();
    frameConstants_ = createUploadBuffer((1 + sunCascadeSplits.size()) * frameConstantStride);
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
    if (!skyPanorama.empty()) createSky(skyPanorama);
    else
    {
        const SkyIrradianceConstants empty{};
        skyIrradiance_ = uploadBuffer(&empty, sizeof(empty));
    }
}

bool D3D12Renderer::supportsMsaa() const
{
    return multisample_.mesh.linear.Get() != nullptr;
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
        treeBatch_.historyValid = false;
    }
}

DXGI_FORMAT D3D12Renderer::backBufferFormat() const
{
    return colorFormat;
}

void D3D12Renderer::setScene(const ViewerScene& scene)
{
    probeGrid_ = {};
    diffuseProbes_.Reset();
    probeDensity_.Reset();
    advectedProbeDensity_.Reset();
    probeUpdateOrder_.Reset();
    probeGroundLight_.Reset();
    probesDirty_ = true;
    probesPending_ = probeCursor_ = groundUpdateRow_ = 0;
    cachedProbeWind_ = observedProbeWind_ = {};
    probeWindAge_ = 0;
    probeWindMoving_ = false;
    probeCatchup_ = false;
    probeLightChangedLastFrame_ = false;
    // Each frame and texture upload has completed before the viewer can switch scenes.
    if (asset_ != scene.asset || fixedLodAsset_ != scene.fixedLod || sprayLodAssets_ != scene.sprayLods ||
        sprayBoughAsset_ != scene.sprayBough || treeHierarchyAsset_ != scene.treeHierarchy || boughVoxelPath_ != scene.boughVoxelBake ||
        (!scene.boughVoxelBake.empty() && (scene.objects.size() > boughTreeCapacity_ || (scene.softwareBoughRaster && !brickClear_))) ||
        livePath_ != scene.liveCut || (!scene.liveCut.empty() && (liveCut_.packAssembly != scene.liveCutPackAssembly || bool(liveGpu_.views) != scene.liveCutGpu || liveCut_.treeCount != scene.objects.size())))
    {
        std::vector<GpuMesh> meshes;
        meshes.reserve(scene.asset->meshes.size());
        for (const auto& mesh : scene.asset->meshes)
        {
            meshes.push_back(uploadMesh(mesh));
        }
        const std::span<const std::uint32_t> members = scene.sprayBough ? std::span(scene.sprayBough->placements)
            : std::span<const std::uint32_t>{};
        sprayGroupTags_.clear();
        spraySourceGroupTags_.Reset();
        for (const auto& lod : scene.sprayLods)
        {
            if (!lod) continue;
            meshes[lod->meshIndex].sprayLod = uploadSprayLod(scene.asset->meshes[lod->meshIndex], *lod, members);
        }
        loadLiveCut(scene);
        uploadTreeHierarchy(scene, meshes);
        sprayBoughMeshes_ = {};
        if (scene.sprayBough)
        {
            BoundingBox bounds;
            for (std::size_t part = 0; part < sprayBoughMeshes_.size(); ++part)
            {
                sprayBoughMeshes_[part] = uploadMesh(scene.sprayBough->parts[part]);
                const auto& group = meshes[scene.sprayLods[part]->meshIndex].sprayLod->groups[1];
                const BoundingBox partBounds(group.bounds.center, group.bounds.halfExtent);
                const auto& vertices = scene.sprayBough->parts[part].vertices;
                BoundingBox proxyBounds;
                BoundingBox::CreateFromPoints(proxyBounds, vertices.size(), &vertices.front().position, sizeof(Vertex));
                BoundingBox combined;
                BoundingBox::CreateMerged(combined, partBounds, proxyBounds);
                if (part == 0) bounds = combined;
                else BoundingBox::CreateMerged(bounds, bounds, combined);
            }
            sprayBoughBounds_ = {bounds.Center, bounds.Extents};
        }
        std::array<GpuFixedLodLevel, fixedLodLevelCount> levels;
        ComPtr<ID3D12Resource> proxyVertices;
        branchGroupProjections_.clear();
        cascadeFoliageBlocks_.clear();
        cascadeProxyError_ = 0.0f;
        branchCullBounds_ = scene.assetBounds;
        if (scene.fixedLod)
        {
            const auto& lod = *scene.fixedLod;
            D3D12_VERTEX_BUFFER_VIEW proxyView{};
            if (!lod.proxyVertices.empty())
            {
                auto lower = XMLoadFloat3(&scene.assetBounds.center) - XMLoadFloat3(&scene.assetBounds.halfExtent);
                auto upper = XMLoadFloat3(&scene.assetBounds.center) + XMLoadFloat3(&scene.assetBounds.halfExtent);
                const auto transform = XMLoadFloat4x4(&scene.asset->meshes[lod.meshIndex].localToWorld);
                for (const auto& vertex : lod.proxyVertices)
                {
                    const auto point = XMVector3TransformCoord(XMLoadFloat3(&vertex.position), transform);
                    lower = XMVectorMin(lower, point);
                    upper = XMVectorMax(upper, point);
                }
                XMStoreFloat3(&branchCullBounds_.center, (lower + upper) * 0.5f);
                XMStoreFloat3(&branchCullBounds_.halfExtent, (upper - lower) * 0.5f);
                const auto bytes = static_cast<UINT>(lod.proxyVertices.size() * sizeof(Vertex));
                proxyVertices = uploadBuffer(lod.proxyVertices.data(), bytes);
                proxyView = {proxyVertices->GetGPUVirtualAddress(), bytes, sizeof(Vertex)};
            }
            for (std::size_t i = 0; i < lod.levels.size(); ++i)
            {
                levels[i] = uploadFixedLodLevel(lod, i);
                if (lod.usesProxyVertices(i)) levels[i].proxyVertices = proxyView;
            }
            if (lod.levels.size() > fixedLodTwoTriangleLevel)
            {
                branchGroupProjections_.reserve(lod.groups.size());
                for (const auto& group : lod.groups)
                    branchGroupProjections_.push_back(branchGroupProjection(group, scene.kind == SceneKind::ModeledOakWalk));
                branchInstanceProjection_ = branchInstanceProjection(branchGroupProjections_);
                if (scene.kind == SceneKind::ModeledOakWalk)
                {
                    const std::span<const BranchLodProjection> groups = branchGroupProjections_;
                    for (std::size_t first = 0; first < groups.size(); first += cascadeGroupBlockSize)
                        cascadeFoliageBlocks_.push_back(branchInstanceProjection(
                            groups.subspan(first, std::min(cascadeGroupBlockSize, groups.size() - first))).bounds);
                    for (const auto& group : lod.groups)
                    {
                        const auto& proxy = group.levels[fixedLodTwoTriangleLevel];
                        cascadeProxyError_ = std::max({cascadeProxyError_, proxy.surfaceError(), proxy.rimDeviation});
                    }
                }
            }
        }
        createTextures(scene.asset->textures);
        meshes_ = std::move(meshes);
        fixedLodLevels_ = std::move(levels);
        fixedLodProxyVertices_ = std::move(proxyVertices);
        uploadBranchVoxels(scene);
        uploadBoughVoxels(scene);
        asset_ = scene.asset;
        fixedLodAsset_ = scene.fixedLod;
        sprayLodAssets_ = scene.sprayLods;
        sprayBoughAsset_ = scene.sprayBough;
        treeHierarchyAsset_ = scene.treeHierarchy;
    }
    writeProbeView();
    ground_ = uploadMesh(scene.ground);
    std::vector<XMFLOAT4> placements{{0.0f, 0.0f, 0.0f, 1.0f}};
    for (const auto& object : scene.objects)
    {
        placements.push_back(object.placement);
    }
    const UINT bytes = static_cast<UINT>(placements.size() * sizeof(XMFLOAT4));
    instances_ = uploadBuffer(placements.data(), bytes);
    instanceView_ = {instances_->GetGPUVirtualAddress(), bytes, sizeof(XMFLOAT4)};
    std::size_t commandsPerTree = 0;
    for (std::size_t i = 0; i < treeParts_.size(); ++i)
    {
        if (!treeParts_[i].bake) continue;
        commandsPerTree += treeParts_[i].bake->nodes.size();
        if (const auto* lod = meshes_[i].sprayLod.get())
            commandsPerTree += lod->levels.size() + lod->localCuts.size();
    }
    const auto commandBytes = commandsPerTree * scene.objects.size() * (1 + sunCascadeSplits.size()) * sizeof(TreeDraw);
    if (commandBytes && (!treeArguments_ || treeArguments_->GetDesc().Width < commandBytes))
    {
        treeArguments_ = createUploadBuffer(commandBytes);
        const D3D12_RANGE noRead{0, 0};
        void* mapped = nullptr;
        check(treeArguments_->Map(0, &noRead, &mapped), "Map tree indirect commands");
        mappedTreeArguments_ = static_cast<TreeDraw*>(mapped);
    }
    std::size_t sprayCount = 0;
    for (const auto& mesh : meshes_)
    {
        if (mesh.sprayLod)
            sprayCount += mesh.meshInstanceCount * std::max<std::size_t>(1, mesh.sprayLod->localGroupCount);
    }
    if (sprayCount)
    {
        const auto capacity = sprayCount * scene.objects.size() * (1 + sunCascadeSplits.size());
        const auto instanceBytes = capacity * sizeof(MeshInstance);
        if (!sprayInstances_ || sprayInstances_->GetDesc().Width < instanceBytes)
        {
            sprayInstances_ = createUploadBuffer(instanceBytes);
            const D3D12_RANGE noRead{0, 0};
            void* mapped = nullptr;
            check(sprayInstances_->Map(0, &noRead, &mapped), "Map spray placement buffer");
            mappedSprayInstances_ = static_cast<MeshInstance*>(mapped);
            sprayInstanceTags_ = createUploadBuffer(capacity * sizeof(LodTag));
            check(sprayInstanceTags_->Map(0, &noRead, &mapped), "Map spray group tags");
            mappedSprayInstanceTags_ = static_cast<LodTag*>(mapped);
        }
    }
    if (!identityMeshInstance_)
    {
        const MeshInstance identity;
        identityMeshInstance_ = uploadBuffer(&identity, sizeof(identity));
        identityMeshInstanceView_ = {identityMeshInstance_->GetGPUVirtualAddress(), sizeof(identity), 0};
    }
    prepareTreeGpu(scene);
    if (scene.hasAutomaticBranchVoxels())
    {
        const auto placementBytes = (scene.branchVoxelRegions ? voxelBranchRegionCount : 1) *
                                    scene.instanceCount() * sizeof(XMFLOAT4);
        if (!voxelPlacements_ || voxelPlacements_->GetDesc().Width < placementBytes)
            voxelPlacements_ = createUploadBuffer(placementBytes);
    }
    if (scene.fixedLod)
    {
        const auto maxDraws = scene.fixedLod->groups.size() * scene.instanceCount();
        const auto passCount = scene.kind == SceneKind::ModeledOakWalk ? 1 + sunCascadeSplits.size() : 2;
        const auto argumentBytes = passCount * maxDraws * sizeof(BranchDraw);
        if (!branchArgumentBuffer_ || branchArgumentBuffer_->GetDesc().Width < argumentBytes)
        {
            branchArgumentBuffer_ = createUploadBuffer(argumentBytes);
        }
        for (auto level : {fixedLodRimLevel, fixedLodFarLevel, fixedLodTwoTriangleLevel})
        {
            cameraBranchDraws_.byLevel[level].reserve(maxDraws);
        }
    }
    updateCameraProjection(scene.nearPlane, scene.farPlane());

    const auto& bounds = scene.assetBounds;
    const float height = 2.0f * bounds.halfExtent.y;
    windStart_ = bounds.center.y - 0.38f * height;
    inverseWindHeight_ = scene.foliage && height > 0.0f ? 1.0f / (0.88f * height) : 0.0f;
    windAmplitude_ = scene.foliage ? 0.05f * height : 0.0f;
    updateSun(scene);
}

void D3D12Renderer::updateCameraProjection(float nearPlane, float farPlane)
{
    if (nearPlane == cameraNearPlane_ && farPlane == cameraFarPlane_) return;
    const float aspect = viewport_.Width / viewport_.Height;
    XMStoreFloat4x4(&projection_, XMMatrixPerspectiveFovRH(cameraVerticalFov, aspect, nearPlane, farPlane));
    cameraNearPlane_ = nearPlane;
    cameraFarPlane_ = farPlane;
}

void D3D12Renderer::updateOrbitCameraProjection(const ViewerScene& scene, float distance)
{
    const bool farView = isOakBranchScene(scene.kind) && distance >= branchVoxelFarDistances.front();
    // Keep normal depth precision until the camera moves into inspection range.
    updateCameraProjection(farView ? branchVoxelFarNearPlane : std::min(scene.nearPlane, distance * 0.1f),
                           farView ? branchVoxelFarClipPlane : scene.farPlane());
}

float D3D12Renderer::projectLodError(float localError, const Bounds& bounds,
                                   const XMFLOAT4X4& localToWorld, const XMFLOAT4X4& view, XMFLOAT2 worldWind) const
{
    if (localError == 0) return 0;
    const auto transform = XMLoadFloat4x4(&localToWorld) * XMLoadFloat4x4(&view);
    const auto wind = XMVector3TransformNormal(XMVectorSet(worldWind.x, 0, worldWind.y, 0),
                                               XMLoadFloat4x4(&view)) * 0.5f;
    const auto center = XMVector3TransformCoord(XMLoadFloat3(&bounds.center), transform) + wind;
    const auto half = XMVectorAbs(transform.r[0]) * bounds.halfExtent.x +
                      XMVectorAbs(transform.r[1]) * bounds.halfExtent.y +
                      XMVectorAbs(transform.r[2]) * bounds.halfExtent.z + XMVectorAbs(wind);
    const float zMin = -XMVectorGetZ(center) - XMVectorGetZ(half);
    if (zMin <= cameraNearPlane_) return std::numeric_limits<float>::infinity();
    const auto farSide = XMVectorAbs(center) + half;
    const float rMax = std::hypot(XMVectorGetX(farSide), XMVectorGetY(farSide));

    const float focalPixels = projection_._22 * viewport_.Height * 0.5f;
    return focalPixels * (localError * maxLinearStretch(transform) / zMin) * std::hypot(1.0f, rMax / zMin);
}

D3D12Renderer::BranchLodProjection D3D12Renderer::branchGroupProjection(const FixedLodGroup& group, bool fullCrown)
{
    const float farError = std::max(group.levels[fixedLodRimLevel].surfaceError(),
                                    group.levels[fixedLodFarLevel].surfaceError());
    const float proxyError = std::max(farError, group.levels[fixedLodTwoTriangleLevel].surfaceError());
    const float lockedRimError = std::max(group.levels[1].surfaceError(), group.levels[2].surfaceError());
    const float rimError = std::max(lockedRimError, group.levels[fixedLodRimLevel].surfaceError());

    // Give both transitions the same projection volume and pad for the sampled displacements.
    const auto center = XMLoadFloat3(&group.bounds.center);
    const auto extent = XMLoadFloat3(&group.bounds.halfExtent);
    const auto proxyCenter = XMLoadFloat3(&group.proxyBounds.center);
    const auto proxyExtent = XMLoadFloat3(&group.proxyBounds.halfExtent);
    const auto lower = XMVectorMin(center - extent, proxyCenter - proxyExtent);
    const auto upper = XMVectorMax(center + extent, proxyCenter + proxyExtent);
    Bounds projectionBounds;
    XMStoreFloat3(&projectionBounds.center, (upper + lower) * 0.5f);
    const float padding = fullCrown ? std::max(proxyError, rimError) : proxyError;
    XMStoreFloat3(&projectionBounds.halfExtent, (upper - lower) * 0.5f + XMVectorReplicate(padding));
    return {projectionBounds, farError, proxyError, lockedRimError, rimError};
}

D3D12Renderer::BranchLodProjection D3D12Renderer::branchInstanceProjection(std::span<const BranchLodProjection> groups)
{
    BranchLodProjection instance{};
    auto lower = XMVectorReplicate(std::numeric_limits<float>::max());
    auto upper = -lower;
    for (const auto& candidate : groups)
    {
        const auto center = XMLoadFloat3(&candidate.bounds.center);
        const auto half = XMLoadFloat3(&candidate.bounds.halfExtent);
        lower = XMVectorMin(lower, center - half);
        upper = XMVectorMax(upper, center + half);
        instance.farError = std::max(instance.farError, candidate.farError);
        instance.proxyError = std::max(instance.proxyError, candidate.proxyError);
        instance.lockedRimError = std::max(instance.lockedRimError, candidate.lockedRimError);
        instance.rimError = std::max(instance.rimError, candidate.rimError);
    }
    const auto center = (lower + upper) * 0.5f;
    auto half = XMVectorMax(center - lower, upper - center);
    // Round the enclosing box outward when collapsing its endpoints to center and extent.
    half += (XMVectorAbs(center) + half) * (4.0f * std::numeric_limits<float>::epsilon());
    XMStoreFloat3(&instance.bounds.center, center);
    XMStoreFloat3(&instance.bounds.halfExtent, half);
    return instance;
}

D3D12Renderer::BranchLodErrors D3D12Renderer::projectBranchLodErrors(
    const FixedLodGroup& group, const XMFLOAT4X4& localToWorld, const XMFLOAT4X4& view) const
{
    return projectBranchLodErrors(branchGroupProjection(group), localToWorld, view);
}

D3D12Renderer::BranchLodErrors D3D12Renderer::projectBranchLodErrors(
    const BranchLodProjection& projection, const XMFLOAT4X4& localToWorld, const XMFLOAT4X4& view) const
{
    return {projectLodError(projection.farError, projection.bounds, localToWorld, view),
            projectLodError(projection.proxyError, projection.bounds, localToWorld, view)};
}

void D3D12Renderer::updateBranchLod(ViewerScene& scene, const XMFLOAT4X4& view) const
{
    const bool fullCrown = scene.kind == SceneKind::ModeledOakWalk;
    const bool regional = scene.branchVoxelView == BranchVoxelView::Regional && scene.branchVoxelRegions;
    const bool automaticVoxels = automaticBranchVoxels(scene.branchVoxelView) && scene.hasAutomaticBranchVoxels();
    if ((!automaticVoxels && scene.branchLod != automaticBranchLod) || !scene.fixedLod ||
        scene.fixedLod->levels.size() <= fixedLodTwoTriangleLevel) return;

    auto& selection = scene.branchLodSelection;
    const auto count = scene.instanceCount();
    const auto groupCount = branchGroupProjections_.size();
    const auto cutSize = groupCount * count;
    if (selection.frozen && selection.groupLevels.size() == cutSize &&
        (!automaticVoxels || (selection.voxelCut.size() == count && selection.voxelCutMode == scene.branchVoxelView))) return;
    selection.tests = {};
    selection.groupLevels.resize(cutSize);
    if (automaticVoxels)
    {
        if (selection.voxelCutMode != scene.branchVoxelView) selection.voxelCut.clear();
        selection.voxelCut.resize(count);
        selection.voxelCutMode = scene.branchVoxelView;
    }
    const auto meshToAsset = XMLoadFloat4x4(&scene.asset->meshes[scene.fixedLod->meshIndex].localToWorld);
    for (unsigned instance = 0; instance < count; ++instance)
    {
        XMFLOAT4X4 localToWorld;
        XMStoreFloat4x4(&localToWorld, meshToAsset * scene.objects[instance].placementMatrix());
        const auto cut = std::span(selection.groupLevels).subspan(instance * groupCount, groupCount);
        bool coarsest = false;
        float pixelsPerUnit = 0;
        if (selection.useInstanceBounds || automaticVoxels)
        {
            ++selection.tests.instances;
            const auto& projection = branchInstanceProjection_;
            // Leave a margin for float transforms and projections at the shortcut's threshold.
            pixelsPerUnit = 1.0001f * projectLodError(1.0f, projection.bounds, localToWorld, view);
            const float farError = fullCrown ? std::max(projection.farError, projection.rimError) : projection.farError;
            const float proxyError = std::max(projection.proxyError, farError);
            coarsest = std::isfinite(pixelsPerUnit) && pixelsPerUnit * farError <= selection.farPixelLimit &&
                pixelsPerUnit * proxyError <= selection.proxyPixelLimit;
            if (coarsest && selection.useInstanceBounds)
            {
                std::fill(cut.begin(), cut.end(), fixedLodTwoTriangleLevel);
                ++selection.tests.acceptedInstances;
            }
        }
        if (!coarsest || !selection.useInstanceBounds)
        {
            selection.tests.groups += groupCount;
            for (std::size_t i = 0; i < groupCount; ++i)
            {
                auto& level = cut[i];
                if (fullCrown)
                {
                    const auto& group = branchGroupProjections_[i];
                    const float scale = projectLodError(1.0f, group.bounds, localToWorld, view);
                    const float farError = std::max(group.farError, group.rimError);
                    if (scale * farError <= selection.farPixelLimit)
                        level = scale * std::max(group.proxyError, farError) <= selection.proxyPixelLimit
                            ? fixedLodTwoTriangleLevel : fixedLodFarLevel;
                    else if (scale * group.rimError <= selection.nearPixelLimit)
                        level = fixedLodRimLevel;
                    else if (scale * group.lockedRimError <= selection.nearPixelLimit)
                        level = 2;
                    else
                        level = 0;
                    continue;
                }
                level = fixedLodRimLevel;
                const auto errors = projectBranchLodErrors(branchGroupProjections_[i], localToWorld, view);
                if (errors.farPixels <= selection.farPixelLimit)
                    level = errors.proxyPixels <= selection.proxyPixelLimit ? fixedLodTwoTriangleLevel : fixedLodFarLevel;
            }
        }
        if (!automaticVoxels) continue;
        auto& voxelCut = selection.voxelCut[instance];
        const float cellSize = static_cast<float>(scene.branchVoxels[automaticBranchVoxelIndex]->cellSize);
        const auto projectedLimit = [&](std::uint8_t previous) {
            return scene.branchVoxelEnterPixels * (previous ? branchVoxelExitFactor : 1.0f);
        };
        if (!regional)
        {
            voxelCut = coarsest && pixelsPerUnit * cellSize <= projectedLimit(voxelCut) ? allVoxelRegions : 0;
            continue;
        }
        std::uint8_t nextCut = 0;
        for (std::size_t region = 0; region < voxelBranchRegionCount; ++region)
        {
            const auto bit = static_cast<std::uint8_t>(1u << region);
            const auto& bounds = voxelRegionProjections_[region].bounds;
            if (projectLodError(cellSize, bounds, localToWorld, view) > projectedLimit(voxelCut & bit)) continue;
            const auto first = voxelRegionGroupStart(region, groupCount);
            const auto length = voxelRegionGroupStart(region + 1, groupCount) - first;
            if (coarsest || std::ranges::all_of(cut.subspan(first, length), [](auto level) { return level == fixedLodTwoTriangleLevel; }))
                nextCut |= bit;
        }
        voxelCut = nextCut;
    }
}

void D3D12Renderer::createBranchCommandSignature()
{
    static_assert(offsetof(BranchDraw, indexed) == sizeof(UINT));
    static_assert(sizeof(BranchDraw) == 6 * sizeof(UINT));
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.RootParameterIndex = 0;
    arguments[0].Constant.DestOffsetIn32BitValues = offsetof(DrawConstants, firstLodTag) / sizeof(UINT);
    arguments[0].Constant.Num32BitValuesToSet = 1;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    const D3D12_COMMAND_SIGNATURE_DESC description{
        sizeof(BranchDraw), static_cast<UINT>(std::size(arguments)), arguments, 0};
    check(device_->CreateCommandSignature(&description, rootSignature_.Get(), IID_PPV_ARGS(&branchCommandSignature_)),
          "Create branch draw command signature");
}

void D3D12Renderer::createTreeCommandSignature()
{
    static_assert(offsetof(TreeDraw, instances) == 16 && offsetof(TreeDraw, indices) == 32);
    static_assert(offsetof(TreeDraw, firstTag) == 48 && offsetof(TreeDraw, tags) == 56);
    static_assert(offsetof(TreeDraw, indexed) == 64 && sizeof(TreeDraw) == 88);
    static_assert(offsetof(TreeBatchDraw, trees) == 64 && offsetof(TreeBatchDraw, indexed) == 80 &&
                  sizeof(TreeBatchDraw) == 104);
    static_assert(offsetof(DrawConstants, lodLevel) == offsetof(DrawConstants, firstLodTag) + sizeof(UINT));
    D3D12_INDIRECT_ARGUMENT_DESC arguments[6]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    arguments[0].VertexBuffer.Slot = 0;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    arguments[1].VertexBuffer.Slot = 2;
    arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    arguments[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[3].Constant.RootParameterIndex = 0;
    arguments[3].Constant.DestOffsetIn32BitValues = offsetof(DrawConstants, firstLodTag) / sizeof(UINT);
    arguments[3].Constant.Num32BitValuesToSet = 2;
    arguments[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW;
    arguments[4].ShaderResourceView.RootParameterIndex = 4;
    arguments[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    const D3D12_COMMAND_SIGNATURE_DESC signature{sizeof(TreeDraw), static_cast<UINT>(std::size(arguments)), arguments, 0};
    check(device_->CreateCommandSignature(&signature, rootSignature_.Get(), IID_PPV_ARGS(&treeCommandSignature_)),
          "Create tree replay command signature");

    D3D12_INDIRECT_ARGUMENT_DESC batched[7]{};
    std::copy_n(arguments, 5, batched);
    batched[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    batched[5].VertexBuffer.Slot = 1;
    batched[6] = arguments[5];
    const D3D12_COMMAND_SIGNATURE_DESC draws{sizeof(TreeBatchDraw), static_cast<UINT>(std::size(batched)), batched, 0};
    check(device_->CreateCommandSignature(&draws, rootSignature_.Get(), IID_PPV_ARGS(&treeBatchDrawSignature_)),
          "Create batched tree draw signature");
    D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
    dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    const D3D12_COMMAND_SIGNATURE_DESC work{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &dispatch, 0};
    check(device_->CreateCommandSignature(&work, nullptr, IID_PPV_ARGS(&treeBatchDispatchSignature_)),
          "Create batched tree compute signature");
}

namespace
{
float projectShadowTexels(float localError, FXMMATRIX localToLightClip)
{
    const auto axes = XMMatrixTranspose(localToLightClip);
    const float xx = XMVectorGetX(XMVector3Dot(axes.r[0], axes.r[0]));
    const float xy = XMVectorGetX(XMVector3Dot(axes.r[0], axes.r[1]));
    const float yy = XMVectorGetX(XMVector3Dot(axes.r[1], axes.r[1]));
    // Largest singular value of the local-to-light XY transform, then NDC to texels.
    const float stretch = std::sqrt(0.5f * (xx + yy + std::hypot(xx - yy, 2.0f * xy)));
    return localError * stretch * (shadowMapSize * 0.5f);
}
}

float D3D12Renderer::projectShadowError(float localError, const XMFLOAT4X4& localToWorld) const
{
    return projectShadowTexels(localError, XMLoadFloat4x4(&localToWorld) * XMLoadFloat4x4(&lightViewProjection_));
}

void D3D12Renderer::PassVisibility::clear()
{
    ranges.clear();
    count = 0;
}

void D3D12Renderer::PassVisibility::add(UINT instance)
{
    add(instance, 1);
}

void D3D12Renderer::PassVisibility::add(UINT first, UINT length)
{
    if (!ranges.empty() && ranges.back().first + ranges.back().count == first)
    {
        ranges.back().count += length;
    }
    else
    {
        ranges.push_back({first, length});
    }
    count += length;
}

bool D3D12Renderer::PassVisibility::contains(UINT instance) const
{
    const auto next = std::ranges::upper_bound(ranges, instance, {}, &InstanceRange::first);
    if (next == ranges.begin()) return false;
    const auto& range = *(next - 1);
    return instance - range.first < range.count;
}

void D3D12Renderer::prepareVisibility(const ViewerScene& scene, FXMMATRIX viewProjection, XMFLOAT2 windOffset)
{
    const auto count = scene.instanceCount();
    const bool culling = scene.branchFrustumCulling;
    const bool experiment = scene.branchVoxelView != BranchVoxelView::Off && !automaticBranchVoxels(scene.branchVoxelView);
    const bool splitShadows = !experiment && scene.branchShadowLod >= 0 && scene.shadowLodOffCameraOnly;
    if (!culling && !splitShadows)
    {
        cameraVisibility_.ranges.assign(1, InstanceRange{0, count});
        cameraVisibility_.count = count;
        shadowVisibility_ = cameraVisibility_;
        return;
    }

    cameraVisibility_.clear();
    shadowVisibility_.clear();
    shadowMatchVisibility_.clear();
    shadowOverrideVisibility_.clear();
    const auto cameraPlanes = frustumPlanes(viewProjection);
    const auto shadowPlanes = frustumPlanes(XMLoadFloat4x4(&lightViewProjection_));
    const auto& bounds = scene.branchVoxelView != BranchVoxelView::Off ? voxelCullBounds_ : branchCullBounds_;
    for (UINT i = 0; i < count; ++i)
    {
        const auto worldBox = worldWindBounds(bounds, scene.objects[i], windOffset);
        const bool inCamera = intersectsFrustum(worldBox, cameraPlanes);
        if (!culling || inCamera) cameraVisibility_.add(i);
        if (culling && !scene.usesSunCascades() && !intersectsFrustum(worldBox, shadowPlanes)) continue;
        shadowVisibility_.add(i);
        if (splitShadows)
        {
            auto& list = inCamera ? shadowMatchVisibility_ : shadowOverrideVisibility_;
            list.add(i);
        }
    }
}

void D3D12Renderer::prepareFoliageGroupVisibility(const ViewerScene& scene, FXMMATRIX viewProjection, XMFLOAT2 windOffset)
{
    cameraGroupVisibility_.clear();
    cameraGroupsConsidered_ = cameraGroupsVisible_ = 0;
    if (scene.kind != SceneKind::ModeledOakWalk || !scene.fixedLod || scene.branchLod == sourceBranchLod) return;
    const auto groups = branchGroupProjections_.size();
    cameraGroupsConsidered_ = cameraGroupsVisible_ = groups * cameraVisibility_.count;
    if (!scene.branchGroupFrustumCulling || !cameraGroupsConsidered_) return;

    const auto worldPlanes = frustumPlanes(viewProjection);
    const auto meshToAsset = XMLoadFloat4x4(&scene.asset->meshes[scene.fixedLod->meshIndex].localToWorld);
    for (const auto range : cameraVisibility_.ranges)
    {
        for (UINT i = range.first; i < range.first + range.count; ++i)
        {
            const auto& object = scene.objects[i];
            if (insideFrustum(worldWindBounds(branchCullBounds_, object, windOffset), worldPlanes)) continue;
            const auto localPlanes = windExpandedLocalPlanes(meshToAsset * object.placementMatrix() * viewProjection,
                                                             worldPlanes, object.placement.w, windOffset);
            for (std::size_t group = 0; group < groups; ++group)
            {
                const auto& bounds = branchGroupProjections_[group].bounds;
                if (intersectsFrustum(BoundingBox(bounds.center, bounds.halfExtent), localPlanes)) continue;
                if (cameraGroupVisibility_.empty()) cameraGroupVisibility_.resize(groups * scene.instanceCount(), 1);
                cameraGroupVisibility_[i * groups + group] = 0;
                --cameraGroupsVisible_;
            }
        }
    }
}

void D3D12Renderer::prepareCascadeCasterDraws(const ViewerScene& scene, XMFLOAT2 windOffset, std::size_t& argumentBytes)
{
    const auto count = scene.instanceCount();
    cascadeInstanceFlags_.assign(count, 0);
    const bool hasFoliage = scene.fixedLod && scene.meshShadows[scene.fixedLod->meshIndex].cast;
    const bool coarsen = scene.coarsenSunCascadeFoliage && scene.canCoarsenCascadeFoliage();
    const auto groups = branchGroupProjections_.size();
    const auto meshToAsset = hasFoliage ? XMLoadFloat4x4(&scene.asset->meshes[scene.fixedLod->meshIndex].localToWorld) : XMMatrixIdentity();
    cascadeInstanceLevels_.resize(count);
    for (std::size_t slice = 0; slice < cascadeCasterDraws_.size(); ++slice)
    {
        auto& pass = cascadeCasterDraws_[slice];
        pass.instances.clear();
        pass.sourceFoliage.clear();
        pass.groupsConsidered = pass.groupsVisible = 0;
        cascadeBakedVisibility_.clear();
        cascadeGroupVisibility_.clear();
        const auto lightClip = XMLoadFloat4x4(&cascadeViewProjections_[slice]);
        const auto planes = frustumPlanes(lightClip);
        for (const auto range : shadowVisibility_.ranges)
        {
            for (UINT i = range.first; i < range.first + range.count; ++i)
            {
                const auto& object = scene.objects[i];
                const auto worldBox = worldWindBounds(branchCullBounds_, object, windOffset);
                // Test the light volume, including its blend overlap. Camera visibility is independent.
                if (scene.cullSunCascadeCasters && !intersectsFrustum(worldBox, planes)) continue;
                pass.instances.add(i);
                cascadeInstanceFlags_[i] |= cascadeCasterVisible;
                if (!hasFoliage) continue;

                const bool override = scene.branchShadowLod >= 0 &&
                    (!scene.shadowLodOffCameraOnly || shadowOverrideVisibility_.contains(i));
                int level = override ? scene.branchShadowLod : scene.branchLod;
                if (level == sourceBranchLod)
                {
                    pass.sourceFoliage.add(i);
                    continue;
                }

                const auto localClip = meshToAsset * object.placementMatrix() * lightClip;
                if (coarsen && projectShadowTexels(cascadeProxyError_, localClip) <= sunCascadeProxyErrorTexels)
                    level = static_cast<int>(fixedLodTwoTriangleLevel);
                if (override) cascadeInstanceFlags_[i] |= cascadeCasterOverride;
                cascadeInstanceLevels_[i] = level;
                cascadeBakedVisibility_.add(i);
                pass.groupsConsidered += groups;
                pass.groupsVisible += groups;
                if (!scene.cullSunCascadeCasters || insideFrustum(worldBox, planes)) continue;

                const auto localPlanes = windExpandedLocalPlanes(localClip, planes, object.placement.w, windOffset);
                const auto reject = [&](std::size_t first, std::size_t length) {
                    if (cascadeGroupVisibility_.empty()) cascadeGroupVisibility_.resize(count * groups, 1);
                    std::fill_n(cascadeGroupVisibility_.begin() + i * groups + first, length, 0);
                    pass.groupsVisible -= length;
                };
                for (std::size_t block = 0; block < cascadeFoliageBlocks_.size(); ++block)
                {
                    const auto& bounds = cascadeFoliageBlocks_[block];
                    const BoundingBox box(bounds.center, bounds.halfExtent);
                    if (insideFrustum(box, localPlanes)) continue;
                    const auto first = block * cascadeGroupBlockSize;
                    const auto length = std::min(cascadeGroupBlockSize, groups - first);
                    if (!intersectsFrustum(box, localPlanes))
                    {
                        reject(first, length);
                        continue;
                    }
                    for (std::size_t group = first; group < first + length; ++group)
                    {
                        const auto& candidate = branchGroupProjections_[group].bounds;
                        if (!intersectsFrustum(BoundingBox(candidate.center, candidate.halfExtent), localPlanes)) reject(group, 1);
                    }
                }
            }
        }
        if (hasFoliage)
            prepareBranchDraws(scene, cascadeBakedVisibility_, pass.bakedFoliage, argumentBytes, false,
                               cascadeGroupVisibility_, automaticBranchLod, cascadeInstanceLevels_);
    }
}

void D3D12Renderer::prepareBranchDraws(const ViewerScene& scene, const PassVisibility& visibility,
                                      BranchPassDraws& pass, std::size_t& argumentBytes, bool omitVoxelRegions,
                                      std::span<const std::uint8_t> visibleGroups, int forcedLevel,
                                      std::span<const int> instanceLevels)
{
    pass.argumentOffset.reset();
    pass.triangles.fill(0);
    for (auto& draws : pass.byLevel) draws.clear();
    const auto& lod = *scene.fixedLod;
    const auto groups = lod.groups.size();
    std::size_t drawCount = 0;
    const auto append = [&](std::size_t level, FixedLodIndexRange range, UINT instances, UINT firstInstance) {
        const UINT indexCount = range.indexCount ? range.indexCount : fixedLodLevels_[level].indexCount;
        pass.byLevel[level].push_back({range.firstIndex / 3, {indexCount, instances, range.firstIndex, 0, firstInstance}});
        pass.triangles[level] += static_cast<std::uint64_t>(indexCount / 3) * instances;
        ++drawCount;
    };

    const std::span<const std::size_t> levels = scene.branchLodSelection.groupLevels;
    const bool validCut = levels.size() == groups * scene.instanceCount();
    if (visibleGroups.empty() && instanceLevels.empty() && forcedLevel < 0 && !validCut)
    {
        for (const auto range : visibility.ranges) append(fixedLodRimLevel, {}, range.count, range.first + 1);
        return;
    }
    const auto fixedLevelOf = [&](UINT instance) {
        return instanceLevels.empty() ? forcedLevel : instanceLevels[instance];
    };
    const auto cutOf = [&](UINT instance) {
        return validCut && fixedLevelOf(instance) < 0 ? levels.subspan(instance * groups, groups) : std::span<const std::size_t>{};
    };
    const auto atLevel = [&](UINT instance, std::span<const std::size_t> cut, std::size_t group) {
        if (!cut.empty()) return cut[group];
        const auto level = fixedLevelOf(instance);
        return level < 0 ? fixedLodRimLevel : static_cast<std::size_t>(level);
    };
    const auto voxelMaskOf = [&](UINT instance) {
        return omitVoxelRegions ? scene.branchLodSelection.voxelCut[instance] : 0;
    };
    const auto groupsOf = [&](UINT instance) {
        return visibleGroups.empty() ? std::span<const std::uint8_t>{} : visibleGroups.subspan(instance * groups, groups);
    };
    const auto allVisible = [&](UINT instance) {
        const auto mask = groupsOf(instance);
        return std::ranges::find(mask, std::uint8_t{0}) == mask.end();
    };
    const auto allAtLevel = [](std::span<const std::size_t> cut, std::size_t level) {
        return std::all_of(cut.begin(), cut.end(), [level](std::size_t value) { return value == level; });
    };
    for (const auto range : visibility.ranges)
    {
        const UINT limit = range.first + range.count;
        for (UINT instance = range.first; instance < limit;)
        {
            const auto cut = cutOf(instance);
            const auto mask = voxelMaskOf(instance);
            if (!mask && (cut.empty() || allAtLevel(cut, cut.front())) && allVisible(instance))
            {
                const auto level = atLevel(instance, cut, 0);
                UINT end = instance + 1;
                while (end < limit && !voxelMaskOf(end) && allVisible(end))
                {
                    const auto next = cutOf(end);
                    if (atLevel(end, next, 0) != level || !allAtLevel(next, level)) break;
                    ++end;
                }
                append(level, {}, end - instance, instance + 1);
                instance = end;
                continue;
            }
            // Consecutive groups at the same level occupy one contiguous index range.
            const auto maskGroups = groupsOf(instance);
            const auto skipped = [&](std::size_t group) {
                return (mask && (mask & voxelGroupRegionBits_[group])) ||
                       (!maskGroups.empty() && !maskGroups[group]);
            };
            for (std::size_t first = 0; first < groups;)
            {
                if (skipped(first))
                {
                    ++first;
                    continue;
                }
                const auto level = atLevel(instance, cut, first);
                std::size_t end = first + 1;
                while (end < groups && atLevel(instance, cut, end) == level && !skipped(end)) ++end;
                append(level, lod.indicesForGroups(level, first, end - 1), 1, instance + 1);
                first = end;
            }
            ++instance;
        }
    }

    if (!scene.branchLodSelection.indirectDraws || drawCount <= 1) return;
    const auto bytes = drawCount * sizeof(BranchDraw);
    // drawFrame waits for the previous frame's GPU work before this buffer can be reused.
    const D3D12_RANGE noCpuReads{0, 0};
    void* mapped = nullptr;
    check(branchArgumentBuffer_->Map(0, &noCpuReads, &mapped), "Map branch draw arguments");
    auto* destination = static_cast<std::byte*>(mapped) + argumentBytes;
    for (const auto& draws : pass.byLevel)
    {
        if (draws.empty()) continue;
        const auto listBytes = draws.size() * sizeof(BranchDraw);
        std::memcpy(destination, draws.data(), listBytes);
        destination += listBytes;
    }
    const D3D12_RANGE written{argumentBytes, argumentBytes + bytes};
    branchArgumentBuffer_->Unmap(0, &written);
    pass.argumentOffset = argumentBytes;
    argumentBytes += bytes;
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

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::createCameraColor(
    DXGI_FORMAT format, DXGI_SAMPLE_DESC samples, D3D12_CPU_DESCRIPTOR_HANDLE target,
    D3D12_RESOURCE_STATES initialState)
{
    auto description = backBuffers_[0]->GetDesc();
    description.Format = format;
    description.SampleDesc = samples;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto* rgb = format == sceneColorFormat ? sceneClearColor : clearColor;
    const D3D12_CLEAR_VALUE clear{format, {rgb[0], rgb[1], rgb[2], rgb[3]}};
    auto color = createRenderTexture(description, initialState, clear);
    device_->CreateRenderTargetView(color.Get(), nullptr, target);
    return color;
}

D3D12Renderer::CameraPipeline D3D12Renderer::createCameraPipeline(
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description, const char* operation)
{
    description.RTVFormats[0] = sceneColorFormat;
    auto linear = createPipeline(device_.Get(), description, operation);
    description.RTVFormats[0] = colorFormat;
    auto encoded = createPipeline(device_.Get(), description, operation);
    return {std::move(linear), std::move(encoded)};
}

void D3D12Renderer::createDepthBuffer(RenderMode& mode, D3D12_RESOURCE_DESC description)
{
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = depthFormat;
    clear.DepthStencil.Depth = 1.0f;
    mode.depthBuffer = createRenderTexture(description, D3D12_RESOURCE_STATE_DEPTH_WRITE, clear);
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = depthFormat;
    view.ViewDimension = description.SampleDesc.Count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(mode.depthBuffer.Get(), &view, mode.depthTarget);
}

void D3D12Renderer::setOakRasterReplay(OakRaster mode, bool validate)
{
    if (mode != OakRaster::Live && (!oakPlacementKeys_ || !liveGpu_.readable || liveGpu_.packStride || liveCut_.replace != 7 ||
                                    msaaEnabled_ || encodedCamera_ || swapChain_))
        throw std::runtime_error("Prime a complete GPU-instanced oak cut in a shaded, single-sample capture before replay.");
    oakRaster_ = mode;
    validateOakRaster_ = validate;
    if (mode < OakRaster::Depth || oakRasterPipelines_[0][0]) return;

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 1;
    check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&oakIdHeap_)), "Create oak visibility descriptor");
    auto texture = sceneColor_->GetDesc();
    texture.Format = DXGI_FORMAT_R32G32_UINT;
    D3D12_CLEAR_VALUE empty{};
    empty.Format = texture.Format;
    oakIds_ = createRenderTexture(texture, D3D12_RESOURCE_STATE_RENDER_TARGET, empty);
    device_->CreateRenderTargetView(oakIds_.Get(), nullptr, oakIdHeap_->GetCPUDescriptorHandleForHeapStart());

    const auto& layout = meshPipelineDescription_.InputLayout;
    std::vector<D3D12_INPUT_ELEMENT_DESC> keyed(layout.pInputElementDescs, layout.pInputElementDescs + layout.NumElements);
    keyed.push_back({"CUTKEY", 0, DXGI_FORMAT_R32_UINT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1});
    for (auto arm : {OakRaster::Depth, OakRaster::Visibility, OakRaster::Flat, OakRaster::EarlyDepthFull})
    {
        auto desc = meshPipelineDescription_;
        desc.VS = {meshVS, sizeof(meshVS)};
        if (arm == OakRaster::Depth)
        {
            desc.NumRenderTargets = 0;
            desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        }
        else if (arm == OakRaster::Visibility)
        {
            desc.InputLayout = {keyed.data(), static_cast<UINT>(keyed.size())};
            desc.VS = {oakVisibilityVS, sizeof(oakVisibilityVS)};
            desc.PS = {oakVisibilityPS, sizeof(oakVisibilityPS)};
            desc.RTVFormats[0] = texture.Format;
        }
        else if (arm == OakRaster::Flat) desc.PS = {oakFlatPS, sizeof(oakFlatPS)};
        else desc.PS = {oakEarlyPS, sizeof(oakEarlyPS)};
        for (unsigned cull = 0; cull < 2; ++cull)
        {
            desc.RasterizerState.CullMode = cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            oakRasterPipelines_[int(arm) - int(OakRaster::Depth)][cull] =
                createPipeline(device_.Get(), desc, "Create fixed-cut oak raster pipeline");
        }
    }
}

void D3D12Renderer::readOakRasterValidation(std::vector<uint32_t>& depth, std::vector<uint64_t>& ids)
{
    readFrameColor(oakDepthReadback_, depth);
    ids.clear();
    if (oakRaster_ == OakRaster::Visibility) readFrameColor(oakIdReadback_, ids);
}

void D3D12Renderer::updateSun(const ViewerScene& scene)
{
    toSun_ = scene.sunDirection();
    // Fit the light to conservative placement bounds. A sun edit needs no GPU resource rebuild.
    const auto rotation = XMMatrixLookToRH(XMVectorZero(), -XMLoadFloat3(&toSun_),
                                           XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    XMStoreFloat4x4(&sunRotation_, rotation);
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
    float casterMin = XMVectorGetZ(minimum);
    float casterMax = XMVectorGetZ(maximum);
    const BoundingBox casterBounds(branchCullBounds_.center, branchCullBounds_.halfExtent);
    float maxInstanceScale = 1.0f;
    const auto& [center, half] = scene.assetBounds;
    for (const auto& object : scene.objects)
    {
        const auto& instance = object.placement;
        maxInstanceScale = std::max(maxInstanceScale, instance.w);
        const auto placement = object.placementMatrix();
        BoundingBox lightBounds;
        casterBounds.Transform(lightBounds, placement * rotation);
        casterMin = std::min(casterMin, lightBounds.Center.z - lightBounds.Extents.z);
        casterMax = std::max(casterMax, lightBounds.Center.z + lightBounds.Extents.z);
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
    cascadeCasterDepth_ = {casterMin - padding, casterMax + padding};
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

void D3D12Renderer::updateShadowCascades(const XMFLOAT4X4& view, const ViewerScene& scene)
{
    const auto lightRotation = XMLoadFloat4x4(&sunRotation_);
    const auto cameraToLight = XMMatrixInverse(nullptr, XMLoadFloat4x4(&view)) * lightRotation;
    constexpr float nearPlane = 1.0f;
    const float casterDepthSpan = cascadeCasterDepth_.y - cascadeCasterDepth_.x;
    const float tanHalfX = 1.0f / projection_._11;
    const float tanHalfY = 1.0f / projection_._22;
    const float tanDiagonalSquared = tanHalfX * tanHalfX + tanHalfY * tanHalfY;
    const bool stabilized = scene.stabilizeSunCascades;
    const float normalBiasTexels = scene.shadowBias.normalTexels;
    float splitStart = std::min(cameraNearPlane_, sunCascadeSplits.front());
    for (std::size_t cascade = 0; cascade < sunCascadeSplits.size(); ++cascade)
    {
        const float splitEnd = sunCascadeSplits[cascade];
        // Include the previous split's transition strip in the incoming map.
        const float fitStart = scene.blendSunCascades && cascade != 0 ?
            std::max(std::min(cameraNearPlane_, splitStart), splitStart - sunCascadeBlendWidths[cascade - 1]) : splitStart;
        auto lower = XMVectorReplicate(std::numeric_limits<float>::max());
        auto upper = -lower;
        float width;
        auto center = XMVectorZero();
        if (stabilized)
        {
            // Equate the distance from the center to the near and far corners.
            // For a wide slice, the far plane's bounding circle alone contains the near corners.
            const float centerDepth = std::min(splitEnd, 0.5f * (fitStart + splitEnd) * (1.0f + tanDiagonalSquared));
            const float offset = splitEnd - centerDepth;
            const float radius = std::sqrt(offset * offset + splitEnd * splitEnd * tanDiagonalSquared);
            width = 2.0f * radius;
            center = XMVector3TransformCoord(XMVectorSet(0, 0, -centerDepth, 1), cameraToLight);
        }
        else
        {
            for (const float depth : {fitStart, splitEnd})
            {
                const float halfX = depth / projection_._11;
                const float halfY = depth / projection_._22;
                for (const float x : {-halfX, halfX})
                    for (const float y : {-halfY, halfY})
                    {
                        const auto point = XMVector3TransformCoord(XMVectorSet(x, y, -depth, 1), cameraToLight);
                        lower = XMVectorMin(lower, point);
                        upper = XMVectorMax(upper, point);
                    }
            }
            const auto dimensions = upper - lower;
            width = std::max(XMVectorGetX(dimensions), XMVectorGetY(dimensions));
        }
        // Reserve space for PCF and, when snapping, half a texel of translation.
        const float guardTexels = (stabilized ? 2.5f : 2.0f) + normalBiasTexels;
        const float pad = guardTexels * width / (shadowMapSize - 2.0f * guardTexels);
        const float texelWidth = (width + 2.0f * pad) / shadowMapSize;
        const float normalOffset = normalBiasTexels * texelWidth;
        const float depthOffset = -cascadeCasterDepth_.y - normalOffset - nearPlane;
        const float depthSpan = casterDepthSpan + 2.0f * normalOffset;
        if (stabilized)
        {
            const auto snap = [texelWidth](float position) {
                return static_cast<float>(std::round(double(position) / texelWidth) * texelWidth);
            };
            const auto lightView = lightRotation *
                XMMatrixTranslation(-snap(XMVectorGetX(center)), -snap(XMVectorGetY(center)), depthOffset);
            const float span = width + 2.0f * pad;
            const auto lightProjection = XMMatrixOrthographicRH(span, span, nearPlane, nearPlane + depthSpan);
            XMStoreFloat4x4(&cascadeViewProjections_[cascade], lightView * lightProjection);
        }
        else
        {
            const auto lightView = lightRotation * XMMatrixTranslation(0.0f, 0.0f, depthOffset);
            const auto lightProjection = XMMatrixOrthographicOffCenterRH(
                XMVectorGetX(lower) - pad, XMVectorGetX(upper) + pad,
                XMVectorGetY(lower) - pad, XMVectorGetY(upper) + pad, nearPlane, nearPlane + depthSpan);
            XMStoreFloat4x4(&cascadeViewProjections_[cascade], lightView * lightProjection);
        }
        cascadeWorldUnitsPerTexel_[cascade] = texelWidth;
        cascadeDepthPerTexel_[cascade] = cascadeWorldUnitsPerTexel_[cascade] / depthSpan;
        splitStart = splitEnd;
    }
}

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::createShadowDepth(UINT16 slices)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = shadowMapSize;
    description.Height = shadowMapSize;
    description.DepthOrArraySize = slices;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = depthFormat;
    clear.DepthStencil.Depth = 1.0f;
    return createRenderTexture(description, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, clear);
}

void D3D12Renderer::createShadowMap()
{
    shadowMap_ = createShadowDepth(1);
    D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
    depthView.Format = depthFormat;
    depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(shadowMap_.Get(), &depthView, shadowDepthTarget_);
}

void D3D12Renderer::createCascadeShadowMaps()
{
    cascadeShadowMap_ = createShadowDepth(static_cast<UINT16>(sunCascadeSplits.size()));
    D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
    depthView.Format = depthFormat;
    depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    depthView.Texture2DArray.ArraySize = 1;
    for (UINT cascade = 0; cascade < cascadeDepthTargets_.size(); ++cascade)
    {
        depthView.Texture2DArray.FirstArraySlice = cascade;
        device_->CreateDepthStencilView(cascadeShadowMap_.Get(), &depthView, cascadeDepthTargets_[cascade]);
    }
    writeCascadeShadowView();
}

void D3D12Renderer::writeCascadeShadowView()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = static_cast<UINT>(sunCascadeSplits.size());
    device_->CreateShaderResourceView(cascadeShadowMap_.Get(), &view, cascadeShadowView_);
}

void D3D12Renderer::createMeshPipelineLayout()
{
    D3D12_DESCRIPTOR_RANGE colorTexture{};
    colorTexture.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorTexture.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE shadowTextures[]{colorTexture, colorTexture};
    shadowTextures[0].BaseShaderRegister = 1;
    shadowTextures[1].BaseShaderRegister = 3;
    shadowTextures[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_DESCRIPTOR_RANGE transmissionColor = colorTexture;
    transmissionColor.BaseShaderRegister = 4;
    D3D12_DESCRIPTOR_RANGE transmissionStrength = colorTexture;
    transmissionStrength.BaseShaderRegister = 5;
    D3D12_DESCRIPTOR_RANGE probeView = colorTexture;
    probeView.BaseShaderRegister = 6;
    D3D12_ROOT_PARAMETER parameters[9]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = drawConstantCount;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &colorTexture};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {static_cast<UINT>(std::size(shadowTextures)), shadowTextures};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[3].Descriptor.ShaderRegister = 1;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[4].Descriptor.ShaderRegister = 2;
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[5].Descriptor.ShaderRegister = 2;
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[6].DescriptorTable = {1, &transmissionColor};
    parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[7].DescriptorTable = {1, &transmissionStrength};
    parameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[8].DescriptorTable = {1, &probeView};
    parameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
        {"MESHINSTANCE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 2, offsetof(MeshInstance, translation),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"MESHINSTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, offsetof(MeshInstance, rotation),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"MESHINSTANCE", 2, DXGI_FORMAT_R32G32B32_FLOAT, 2, offsetof(MeshInstance, scale),
         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    auto& description = meshPipelineDescription_;
    description.pRootSignature = rootSignature_.Get();
    description.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // glTF front faces are counterclockwise in our right-handed camera.
    description.RasterizerState.FrontCounterClockwise = TRUE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.DepthStencilState.DepthEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    description.DSVFormat = depthFormat;
    description.SampleMask = UINT_MAX;
    description.SampleDesc.Count = 1;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = sceneColorFormat;
}

void D3D12Renderer::createSky(const std::filesystem::path& panorama)
{
    D3D12_DESCRIPTOR_RANGE texture{};
    texture.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = sizeof(SkyConstants) / sizeof(float);
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &texture};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = static_cast<UINT>(std::size(parameters));
    root.pParameters = parameters;
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
          "Serialize sky root signature");
    check(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&skyRoot_)), "Create sky root signature");

    auto pipeline = meshPipelineDescription_;
    pipeline.pRootSignature = skyRoot_.Get();
    pipeline.InputLayout = {};
    pipeline.VS = {skyVS, sizeof(skyVS)};
    pipeline.PS = {skyPS, sizeof(skyPS)};
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    singleSample_.sky = createCameraPipeline(pipeline, "Create sky pipelines");
    if (supportsMsaa())
    {
        pipeline.SampleDesc = msaaSamples;
        pipeline.RasterizerState.MultisampleEnable = TRUE;
        multisample_.sky = createCameraPipeline(pipeline, "Create 4x MSAA sky pipelines");
    }

    const auto sky = loadSkyLighting(panorama);
    skyRadianceScale_ = sky.radianceScale;
    XMStoreFloat3(&openSkyGround_, XMLoadFloat4(&sky.irradiance[0]) + XMLoadFloat4(&sky.irradiance[2]) +
                                   2.0f * XMLoadFloat4(&sky.irradiance[6]));
    const SkyIrradianceConstants irradiance{sky.irradiance};
    skyIrradiance_ = uploadBuffer(&irradiance, sizeof(irradiance));
    check(allocator_->Reset(), "Reset sky upload allocator");
    check(commands_->Reset(allocator_.Get(), nullptr), "Reset sky upload command list");
    std::vector<ComPtr<ID3D12Resource>> staging;
    skyPanorama_ = uploadTexture(sky.radianceMips, staging, sceneColorFormat,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    check(commands_->Close(), "Close sky upload command list");
    ID3D12CommandList* lists[] = {commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitForGpu();
}

void D3D12Renderer::drawSky(ID3D12PipelineState* pipeline, const XMFLOAT4X4& view, bool skyOnly)
{
    // Undo perspective scaling and rotate a camera-space direction into world space.
    const auto transform = XMMatrixScaling(1.0f / projection_._11, 1.0f / projection_._22, 1.0f) *
                           XMMatrixTranspose(XMLoadFloat4x4(&view));
    SkyConstants constants{};
    XMStoreFloat4x4(&constants.viewRayTransform, transform);
    constants.toSun = toSun_;
    constants.encodedCamera = encodedCamera_;
    constants.skyOnly = skyOnly;

    commands_->SetGraphicsRootSignature(skyRoot_.Get());
    commands_->SetPipelineState(pipeline);
    commands_->SetGraphicsRoot32BitConstants(0, sizeof(constants) / sizeof(float), &constants, 0);
    commands_->SetGraphicsRootDescriptorTable(1, skyTexture_);
    commands_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Renderer::createVoxelPipeline(bool enableMultisampling)
{
    D3D12_DESCRIPTOR_RANGE shadow{};
    shadow.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadow.NumDescriptors = 1;
    shadow.BaseShaderRegister = 2;
    auto probeView = shadow;
    probeView.BaseShaderRegister = 6;
    D3D12_ROOT_PARAMETER parameters[11]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {1, 0, sizeof(VoxelDrawConstants) / sizeof(UINT)};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[3].Descriptor.ShaderRegister = 1;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[4].DescriptorTable = {1, &shadow};
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[5].Descriptor.ShaderRegister = 3;
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[6].Descriptor.ShaderRegister = 2;
    parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[7].DescriptorTable = {1, &probeView};
    parameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[9].Descriptor.ShaderRegister = 4;
    parameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[10].Descriptor.ShaderRegister = 5;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 1;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC root{static_cast<UINT>(std::size(parameters)), parameters, 1, &sampler,
                                        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "Serialize voxel root signature");
    check(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&voxelRoot_)),
          "Create voxel root signature");

    auto description = meshPipelineDescription_;
    description.pRootSignature = voxelRoot_.Get();
    description.InputLayout = {};
    description.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    description.VS = {voxelVS, sizeof(voxelVS)};
    description.PS = {voxelPS, sizeof(voxelPS)};
    singleSample_.voxel = createCameraPipeline(description, "Create voxel pipelines");
    auto shadowDescription = description;
    shadowDescription.PS = {voxelShadowPS, sizeof(voxelShadowPS)};
    shadowDescription.NumRenderTargets = 0;
    shadowDescription.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    voxelShadowPipeline_ = createPipeline(device_.Get(), shadowDescription, "Create voxel shadow pipeline");
    if (enableMultisampling)
    {
        description.SampleDesc = msaaSamples;
        description.RasterizerState.MultisampleEnable = TRUE;
        multisample_.voxel = createCameraPipeline(description, "Create 4x MSAA voxel pipelines");
    }
    constexpr auto alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    constexpr auto stride = (sizeof(VoxelFrameConstants) + alignment - 1) / alignment * alignment;
    voxelConstants_ = createUploadBuffer(stride * (voxelBranchRegionCount + 2));
    voxelBoxIndices_ = uploadBuffer(voxelBoxIndices, sizeof(voxelBoxIndices));
}

D3D12Renderer::GpuVoxelLevel D3D12Renderer::uploadVoxelLevel(const VoxelFoliage& source, const VoxelNormalData* normals)
{
    GpuVoxelLevel level{};
    level.origin = {static_cast<float>(source.origin[0]), static_cast<float>(source.origin[1]),
                    static_cast<float>(source.origin[2])};
    level.cellSize = static_cast<float>(source.cellSize);
    level.brickCount = static_cast<UINT>(source.bricks.size());
    std::vector<GpuVoxelBrick> bricks;
    bricks.reserve(source.bricks.size());
    for (const auto& brick : source.bricks)
    {
        std::array<unsigned, 3> lower{voxelBrickSide, voxelBrickSide, voxelBrickSide};
        std::array<unsigned, 3> upper{};
        for (auto cells = brick.occupancy; cells; cells &= cells - 1)
        {
            const auto bit = static_cast<unsigned>(std::countr_zero(cells));
            const std::array cell{bit % voxelBrickSide, bit / voxelBrickSide % voxelBrickSide,
                                  bit / (voxelBrickSide * voxelBrickSide)};
            for (std::size_t axis = 0; axis < cell.size(); ++axis)
            {
                lower[axis] = std::min(lower[axis], cell[axis]);
                upper[axis] = std::max(upper[axis], cell[axis] + 1);
            }
        }
        std::uint32_t packedBounds = 0;
        // Three bits per corner coordinate, with upper bounds exclusive.
        for (std::size_t axis = 0; axis < lower.size(); ++axis)
            packedBounds |= (lower[axis] << (3 * axis)) | (upper[axis] << (3 * (axis + 3)));
        bricks.push_back({brick.coordinate, {static_cast<UINT>(brick.occupancy), static_cast<UINT>(brick.occupancy >> 32)},
                          packedBounds});
    }
    level.bricks = uploadBuffer(bricks.data(), bricks.size() * sizeof(GpuVoxelBrick));
    if (normals)
    {
        std::vector<UINT> buffer;
        buffer.reserve(bricks.size() + std::tuple_size_v<PackedVoxelNormals> * normals->cells.size());
        UINT offset = 0;
        for (const auto& brick : source.bricks)
        {
            buffer.push_back(offset);
            offset += std::popcount(brick.occupancy);
        }
        for (const auto& cell : normals->cells) buffer.insert(buffer.end(), cell.begin(), cell.end());
        level.normals = uploadBuffer(buffer.data(), buffer.size() * sizeof(UINT));
        level.hasDirectionalWeights = normals->hasDirectionalWeights;
        level.hasShadowReceivers = normals->hasShadowReceivers;
    }
    return level;
}

void D3D12Renderer::uploadBoughVoxels(const ViewerScene& scene)
{
    boughVoxelLevels_ = {};
    boughReplace_ = 0;
    mixedBoughVoxels_ = false;
    boughVoxelPath_ = scene.boughVoxelBake;
    if (boughVoxelPath_.empty()) return;
    constexpr size_t stride = (sizeof(VoxelFrameConstants) + 255) & ~size_t(255);
    voxelConstants_ = createUploadBuffer(stride * (voxelBranchRegionCount + 2 + 2 * scene.objects.size()));
    boughTreeCapacity_ = scene.objects.size();
    if (scene.softwareBoughRaster && !brickClear_)
    {
        auto create = [&](D3D12_SHADER_BYTECODE shader, ComPtr<ID3D12PipelineState>& pipeline) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = voxelRoot_.Get();
            desc.CS = shader;
            check(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "Create SM 6.6 bough brick raster pipeline");
        };
        create({BrickClearCS,sizeof(BrickClearCS)},brickClear_);
        create({BrickVisibilityCS,sizeof(BrickVisibilityCS)},brickVisibility_);
        auto desc = meshPipelineDescription_;
        desc.pRootSignature = voxelRoot_.Get();
        desc.InputLayout = {};
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.VS = {BrickResolveVS,sizeof(BrickResolveVS)};
        desc.PS = {BrickResolvePS,sizeof(BrickResolvePS)};
        brickResolve_ = createCameraPipeline(desc,"Create bough brick material resolve");
    }
    std::ifstream input(boughVoxelPath_, std::ios::binary);
    input.exceptions(std::ios::failbit | std::ios::badbit);
    auto read = [&](auto* ptr, size_t count) {
        input.read(reinterpret_cast<char*>(ptr), sizeof(*ptr) * count);
    };
    char magic[8];
    read(magic, 8);
    read(&boughFirst_, 1); read(&boughCount_, 1); read(&boughReplace_, 1);
    mixedBoughVoxels_ = std::memcmp(magic, "OAKMX001", 8) == 0;
    if ((!mixedBoughVoxels_ && std::memcmp(magic, "OAKVX001", 8)) || boughReplace_ > 3 ||
        (mixedBoughVoxels_ && !boughReplace_) || !boughCount_ ||
        !scene.treeHierarchy || !scene.sprayLods[0] || !scene.sprayLods[1])
        throw std::runtime_error("Invalid bough voxel fixture.");
    for (UINT slot = 0; slot < 2; ++slot)
    {
        auto& level = boughVoxelLevels_[slot];
        UINT cells;
        read(&level.brickCount, 1); read(&cells, 1);
        if (level.brickCount > 1000000 || cells > 4000000 || cells < level.brickCount || cells > 64ull * level.brickCount)
            throw std::runtime_error("Invalid bough voxel payload size.");
        if (!level.brickCount) continue;
        std::vector<GpuVoxelBrick> bricks(level.brickCount);
        std::vector<XMFLOAT4> axes(4ull * level.brickCount);
        std::vector<UINT> normals(level.brickCount + 4ull * cells);
        read(bricks.data(), bricks.size()); read(axes.data(), axes.size()); read(normals.data(), normals.size());
        auto lower = XMVectorReplicate(std::numeric_limits<float>::max()), upper = -lower;
        for (UINT i = 0; i < level.brickCount; ++i)
        {
            auto low = XMLoadFloat4(&axes[4*i]), high = low;
            for (UINT axis = 1; axis <= 3; ++axis)
            {
                const auto delta = 4 * XMLoadFloat4(&axes[4*i + axis]);
                low += XMVectorMin(delta, XMVectorZero());
                high += XMVectorMax(delta, XMVectorZero());
            }
            lower = XMVectorMin(lower,low);
            upper = XMVectorMax(upper,high);
        }
        XMStoreFloat3(&boughBrickBounds_[slot].center, (lower + upper) * .5f);
        XMStoreFloat3(&boughBrickBounds_[slot].halfExtent, (upper - lower) * .5f);
        UINT offset = 0;
        for (UINT i = 0; i < level.brickCount; ++i)
        {
            if (normals[i] != offset) throw std::runtime_error("Invalid cell attribute offset.");
            offset += std::popcount(bricks[i].occupancy[0]) + std::popcount(bricks[i].occupancy[1]);
        }
        if (offset != cells) throw std::runtime_error("Invalid bough occupancy.");
        level.bricks = uploadBuffer(bricks.data(), bricks.size() * sizeof(bricks[0]));
        level.axes = uploadBuffer(axes.data(), axes.size() * sizeof(axes[0]));
        level.normals = uploadBuffer(normals.data(), normals.size() * sizeof(UINT));
        level.cellSize = 1;
        level.meshIndex = scene.sprayLods[slot]->meshIndex;
    }
    if (mixedBoughVoxels_ && !boughVoxelLevels_[0].brickCount && !boughVoxelLevels_[1].brickCount)
        throw std::runtime_error("Mixed cut contains no bricks.");
    if (input.rdbuf()->sgetc() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing bough voxel data.");
}

void D3D12Renderer::uploadBranchVoxels(const ViewerScene& scene)
{
    voxelLevels_ = {};
    voxelRegionLevels_ = {};
    voxelGroupRegionBits_.clear();
    voxelCullBounds_ = branchCullBounds_;
    if (!scene.fixedLod) return;
    const auto model = XMLoadFloat4x4(&scene.asset->meshes[scene.fixedLod->meshIndex].localToWorld);
    auto boundsLower = XMLoadFloat3(&branchCullBounds_.center) - XMLoadFloat3(&branchCullBounds_.halfExtent);
    auto boundsUpper = XMLoadFloat3(&branchCullBounds_.center) + XMLoadFloat3(&branchCullBounds_.halfExtent);
    for (std::size_t i = 0; i < voxelLevels_.size(); ++i)
    {
        if (!scene.branchVoxels[i]) continue;
        const auto& source = *scene.branchVoxels[i];
        auto& level = voxelLevels_[i];
        level = uploadVoxelLevel(source, scene.branchVoxelNormals[i].get());
        auto lower = XMVectorReplicate(std::numeric_limits<float>::max());
        auto upper = -lower;
        const float edge = voxelBrickSide * level.cellSize;
        for (const auto& brick : source.bricks)
        {
            const auto corner = XMLoadFloat3(&level.origin) +
                edge * XMVectorSet(static_cast<float>(brick.coordinate[0]), static_cast<float>(brick.coordinate[1]),
                                   static_cast<float>(brick.coordinate[2]), 0);
            lower = XMVectorMin(lower, corner);
            upper = XMVectorMax(upper, corner + XMVectorReplicate(edge));
        }
        BoundingBox local, asset;
        XMStoreFloat3(&local.Center, (lower + upper) * 0.5f);
        XMStoreFloat3(&local.Extents, (upper - lower) * 0.5f);
        local.Transform(asset, model);
        boundsLower = XMVectorMin(boundsLower, XMLoadFloat3(&asset.Center) - XMLoadFloat3(&asset.Extents));
        boundsUpper = XMVectorMax(boundsUpper, XMLoadFloat3(&asset.Center) + XMLoadFloat3(&asset.Extents));
    }
    XMStoreFloat3(&voxelCullBounds_.center, (boundsLower + boundsUpper) * 0.5f);
    XMStoreFloat3(&voxelCullBounds_.halfExtent, (boundsUpper - boundsLower) * 0.5f);
    if (scene.branchVoxelRegions)
    {
        const auto groupCount = branchGroupProjections_.size();
        voxelGroupRegionBits_.resize(groupCount);
        for (std::size_t i = 0; i < voxelBranchRegionCount; ++i)
        {
            const auto& source = (*scene.branchVoxelRegions)[i];
            voxelRegionLevels_[i] = uploadVoxelLevel(source.foliage, &source.normals);
            const auto first = voxelRegionGroupStart(i, groupCount);
            const auto end = voxelRegionGroupStart(i + 1, groupCount);
            std::fill(voxelGroupRegionBits_.begin() + first, voxelGroupRegionBits_.begin() + end,
                      static_cast<std::uint8_t>(1u << i));
            auto projection = branchInstanceProjection(std::span(branchGroupProjections_).subspan(first, end - first));
            auto& half = projection.bounds.halfExtent;
            const auto cellSize = voxelRegionLevels_[i].cellSize;
            half.x += cellSize; half.y += cellSize; half.z += cellSize;
            voxelRegionProjections_[i] = projection;
        }
    }
}

void D3D12Renderer::drawBranchVoxels(const ViewerScene& scene, const GpuVoxelLevel& level, FXMMATRIX view,
                                   const PassVisibility& visibility, PassStats& stats, bool packPlacements, bool shadowPass, const SceneObject* tree)
{
    if (visibility.ranges.empty()) return;
    packPlacements = packPlacements && visibility.ranges.size() > 1;
    const auto meshIndex = level.axes ? level.meshIndex : scene.fixedLod->meshIndex;
    const auto& mesh = meshes_[meshIndex];
    const auto& material = scene.asset->materials[mesh.material];
    VoxelFrameConstants frame{};
    const auto viewProjection = shadowPass ? XMLoadFloat4x4(&lightViewProjection_) : view * XMLoadFloat4x4(&projection_);
    XMStoreFloat4x4(&frame.viewProjection, viewProjection);
    frame.lightViewProjection = lightViewProjection_;
    frame.meshToAsset = mesh.localToWorld;
    XMStoreFloat4x4(&frame.assetToMesh, XMMatrixInverse(nullptr, XMLoadFloat4x4(&mesh.localToWorld)));
    if (!shadowPass) XMStoreFloat3(&frame.eye, XMMatrixInverse(nullptr, view).r[3]);
    frame.nearPlane = cameraNearPlane_;
    frame.farPlane = cameraFarPlane_;
    frame.projectionZScale = -projection_._33;
    frame.projectionZOffset = projection_._43;
    frame.shadowBias = (scene.shadowBias.samplingTexels + scene.branchVoxelShadowOffsetTexels) * shadowDepthPerTexel_;
    frame.baseColor = {material.color.x, material.color.y, material.color.z};
    if (scene.branchVoxelUseNormals && level.normals)
        frame.useNormals = scene.branchVoxelDirectionalNormals && level.hasDirectionalWeights ? 2u : 1u;
    frame.toSun = toSun_;
    frame.transmission = material.transmission;
    if (scene.branchAppearance == BranchAppearance::Coverage) frame.viewMode = 1u;
    if (scene.branchAppearance == BranchAppearance::BaseColor) frame.viewMode = 3u;
    if (scene.branchAppearance == BranchAppearance::Diffuse) frame.viewMode = 4u;
    if (scene.branchAppearance == BranchAppearance::ReceivedShadows) frame.viewMode = 5u;
    if (scene.branchAppearance == BranchAppearance::CoverageMask) frame.viewMode = 6u;
    if (scene.branchAppearance == BranchAppearance::Representation) frame.viewMode = 7u;
    if (scene.branchVoxelDepthCapture) frame.viewMode = 2u;
    frame.receivesShadows = scene.meshShadows[meshIndex].receive;
    frame.mirrored = mesh.mirrored;
    const auto receiver = scene.branchVoxelShadowReceiver;
    frame.shadowReceiver = static_cast<UINT>(receiver == VoxelShadowReceiver::BakedLeaf && !level.hasShadowReceivers
        ? VoxelShadowReceiver::CellEntry : receiver);
    frame.tightBrickProxies = scene.branchVoxelTightProxies;
    if (level.axes)
    {
        frame.orientedBricks = 1;
        frame.useNormals = 1;
        frame.tightBrickProxies = 1;
        frame.shadowReceiver = 0;
        XMStoreFloat4x4(&frame.meshToAsset, XMMatrixIdentity());
        frame.assetToMesh = frame.meshToAsset;
        frame.boughWind = {frameWind_.x,frameWind_.y,windStart_,inverseWindHeight_};
        if (captureGeometryMask_) frame.viewMode = boughDepth_ ? 10 : boughMaterials_ ?
            (level.meshIndex == scene.sprayLods[0]->meshIndex ? 8 : 9) : 1;
    }
    frame.treePlacement = tree ? tree->placement : XMFLOAT4{0,0,0,1};
    const bool software = level.axes && scene.softwareBoughRaster && !msaaEnabled_ && !shadowPass;
    size_t slot = 0;
    UINT maxWidth = 0, maxHeight = 0;
    if (software)
    {
        slot = static_cast<size_t>(&level - boughVoxelLevels_.data());
        brickWorkItems_.clear();
        UINT64 pixels = 0;
        for (const auto range : visibility.ranges)
            for (UINT i = range.first; i < range.first + range.count; ++i)
            {
                const auto& object = scene.objects[i];
                auto bounds = worldWindBounds(boughBrickBounds_[slot],object,frameWind_);
                XMFLOAT3 corners[8];
                bounds.GetCorners(corners);
                float left = viewport_.Width, top = viewport_.Height, right = 0, bottom = 0;
                for (const auto& corner : corners)
                {
                    XMFLOAT4 p;
                    XMStoreFloat4(&p,XMVector4Transform(XMVectorSetW(XMLoadFloat3(&corner),1),viewProjection));
                    if (p.w <= cameraNearPlane_) { left = top = 0; right = viewport_.Width; bottom = viewport_.Height; break; }
                    const float x = (.5f + .5f*p.x/p.w)*viewport_.Width, y = (.5f - .5f*p.y/p.w)*viewport_.Height;
                    left = std::min(left,x); right = std::max(right,x); top = std::min(top,y); bottom = std::max(bottom,y);
                }
                D3D12_RECT rect = {std::clamp<LONG>(LONG(floor(left))-1,0,scissor_.right), std::clamp<LONG>(LONG(floor(top))-1,0,scissor_.bottom),
                        std::clamp<LONG>(LONG(ceil(right))+1,0,scissor_.right), std::clamp<LONG>(LONG(ceil(bottom))+1,0,scissor_.bottom)};
                if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
                UINT width = UINT(rect.right-rect.left), height = UINT(rect.bottom-rect.top);
                brickWorkItems_.push_back({object.placement,UINT(rect.left),UINT(rect.top),width,height,UINT(pixels)});
                maxWidth = std::max(maxWidth,width);
                maxHeight = std::max(maxHeight,height);
                pixels += UINT64(width)*height;
            }
        if (brickWorkItems_.empty()) return;
        if (pixels > UINT_MAX/8 || brickWorkItems_.size() > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
            throw std::runtime_error("Bough brick batch exceeds dispatch or byte address limits.");
        frame.rasterSize = {viewport_.Width,viewport_.Height};
        const auto camera = XMMatrixInverse(nullptr,view);
        XMStoreFloat4(&frame.screenRayX,camera.r[0]/projection_._11);
        XMStoreFloat4(&frame.screenRayY,camera.r[1]/projection_._22);
        XMStoreFloat4(&frame.screenRayForward,-camera.r[2]);
        // Each tree owns a compact rectangle in scratch. Hardware depth resolves overlaps between trees.
        const auto bytes = std::bit_ceil(std::max<UINT64>(65536,pixels*8));
        if (!brickHits_[slot] || brickHits_[slot]->GetDesc().Width < bytes)
            brickHits_[slot] = createProbeBuffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const size_t workBytes = brickWorkItems_.size()*sizeof(BrickWork);
        static_assert(sizeof(BrickWork) == 48);
        if (!brickWork_[slot] || brickWork_[slot]->GetDesc().Width < workBytes)
            brickWork_[slot] = createUploadBuffer(workBytes);
        void* items;
        const D3D12_RANGE noReads{};
        check(brickWork_[slot]->Map(0,&noReads,&items),"Map bough brick rectangles");
        std::memcpy(items,brickWorkItems_.data(),workBytes);
        const D3D12_RANGE written{0,workBytes};
        brickWork_[slot]->Unmap(0,&written);
    }
    frame.encodedCamera = encodedCamera_;
    frame.directionalSkylight = skyPanorama_ && !encodedCamera_ && !scene.legacyAmbientForDiagnostics;
    frame.lightingMode = static_cast<UINT>(scene.captureLighting);
    if (probesActive_ && !shadowPass) frame.probes = probeGrid_;
    if (useLeafResponse_ && frame.useNormals && material.diffuseTransmission)
    {
        const auto& leaf = *material.diffuseTransmission;
        frame.thinLeaf = 1;
        frame.transmissionColor = leaf.color;
        // Voxel cells store normal pairs and coverage. Use the material's coarsest mip for color.
        if (leaf.colorTexture)
        {
            const auto& rgba = scene.asset->textures[leaf.colorTexture->index].mips.back().rgba;
            frame.transmissionColor.x *= srgbToLinear(rgba[0] / 255.0f);
            frame.transmissionColor.y *= srgbToLinear(rgba[1] / 255.0f);
            frame.transmissionColor.z *= srgbToLinear(rgba[2] / 255.0f);
        }
        if (leaf.strengthTexture)
        {
            const auto& rgba = scene.asset->textures[leaf.strengthTexture->index].mips.back().rgba;
            frame.transmission *= rgba[3] / 255.0f;
        }
    }
    if (material.colorTexture)
    {
        const auto& rgba = scene.asset->textures[*material.colorTexture].mips.back().rgba;
        frame.baseColor.x *= srgbToLinear(rgba[0] / 255.0f);
        frame.baseColor.y *= srgbToLinear(rgba[1] / 255.0f);
        frame.baseColor.z *= srgbToLinear(rgba[2] / 255.0f);
        frame.encodeColor = 1;
    }
    const D3D12_RANGE noCpuReads{0, 0};
    constexpr auto alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    constexpr auto stride = (sizeof(VoxelFrameConstants) + alignment - 1) / alignment * alignment;
    const auto constantOffset = voxelDrawSlot_++ * stride;
    const D3D12_RANGE written{constantOffset, constantOffset + sizeof(frame)};
    void* mapped = nullptr;
    check(voxelConstants_->Map(0, &noCpuReads, &mapped), "Map voxel frame constants");
    std::memcpy(static_cast<std::byte*>(mapped) + constantOffset, &frame, sizeof(frame));
    voxelConstants_->Unmap(0, &written);

    const auto& mode = msaaEnabled_ ? multisample_ : singleSample_;
    commands_->SetPipelineState(shadowPass ? voxelShadowPipeline_.Get() : mode.voxel.get(encodedCamera_));
    commands_->SetGraphicsRootSignature(voxelRoot_.Get());
    commands_->SetGraphicsRootConstantBufferView(0, voxelConstants_->GetGPUVirtualAddress() + constantOffset);
    commands_->SetGraphicsRootShaderResourceView(2, level.bricks->GetGPUVirtualAddress());
    commands_->SetGraphicsRootShaderResourceView(3, (level.axes ? level.axes : packPlacements ? voxelPlacements_ : instances_)->GetGPUVirtualAddress());
    if (!shadowPass)
    {
        commands_->SetGraphicsRootDescriptorTable(4, shadowTexture_);
        commands_->SetGraphicsRootShaderResourceView(5, (level.normals ? level.normals : level.bricks)->GetGPUVirtualAddress());
        commands_->SetGraphicsRootConstantBufferView(6, skyIrradiance_->GetGPUVirtualAddress());
        commands_->SetGraphicsRootDescriptorTable(7, probeGpuView_);
    }
    if (software)
    {
        const UINT count = static_cast<UINT>(brickWorkItems_.size());
        const auto hits = brickHits_[slot]->GetGPUVirtualAddress();
        const auto workAddress = brickWork_[slot]->GetGPUVirtualAddress();
        commands_->SetGraphicsRootShaderResourceView(10,workAddress);
        const VoxelDrawConstants args{level.origin,level.cellSize,level.brickCount,0};
        commands_->SetComputeRootSignature(voxelRoot_.Get());
        commands_->SetComputeRootConstantBufferView(0,voxelConstants_->GetGPUVirtualAddress()+constantOffset);
        commands_->SetComputeRoot32BitConstants(1,sizeof(args)/4,&args,0);
        commands_->SetComputeRootShaderResourceView(2,level.bricks->GetGPUVirtualAddress());
        commands_->SetComputeRootShaderResourceView(3,level.axes->GetGPUVirtualAddress());
        commands_->SetComputeRootUnorderedAccessView(8,hits);
        commands_->SetComputeRootShaderResourceView(10,workAddress);
        commands_->SetPipelineState(brickClear_.Get());
        commands_->Dispatch((maxWidth+7)/8,(maxHeight+7)/8,count);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = brickHits_[slot].Get();
        commands_->ResourceBarrier(1,&barrier);
        commands_->SetPipelineState(brickVisibility_.Get());
        commands_->Dispatch((level.brickCount+63)/64,count,1);
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {brickHits_[slot].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        commands_->ResourceBarrier(1,&barrier);
        commands_->SetPipelineState(brickResolve_.get(encodedCamera_));
        commands_->SetGraphicsRoot32BitConstants(1,sizeof(args)/4,&args,0);
        commands_->SetGraphicsRootShaderResourceView(9,hits);
        commands_->DrawInstanced(6,count,0,0);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        commands_->ResourceBarrier(1,&barrier);
        ++stats.draws; ++stats.foliageDraws; ++stats.foliageCalls;
        stats.voxelBricks += uint64_t(count)*level.brickCount;
        stats.triangles += 2ull * count;
        return;
    }
    const D3D12_INDEX_BUFFER_VIEW box{voxelBoxIndices_->GetGPUVirtualAddress(), sizeof(voxelBoxIndices), DXGI_FORMAT_R16_UINT};
    commands_->IASetIndexBuffer(&box);
    const auto draw = [&](UINT firstPlacement, UINT placementCount)
    {
        const auto bricks = static_cast<std::uint64_t>(placementCount) * level.brickCount;
        if (bricks > UINT_MAX) throw std::runtime_error("Visible voxel brick count exceeds the draw limit.");
        const VoxelDrawConstants args{level.origin, level.cellSize, level.brickCount, firstPlacement};
        commands_->SetGraphicsRoot32BitConstants(1, sizeof(args) / sizeof(UINT), &args, 0);
        commands_->DrawIndexedInstanced(static_cast<UINT>(std::size(voxelBoxIndices)), static_cast<UINT>(bricks), 0, 0, 0);
        ++stats.draws;
        ++stats.foliageDraws;
        ++stats.foliageCalls;
        stats.voxelBricks += bricks;
        stats.triangles += bricks * 12;
        stats.foliageTriangles += bricks * 12;
    };
    if (packPlacements)
    {
        check(voxelPlacements_->Map(0, &noCpuReads, &mapped), "Map voxel placements");
        const auto first = voxelPlacementOffset_;
        auto* placements = static_cast<XMFLOAT4*>(mapped);
        for (const auto range : visibility.ranges)
            for (UINT i = range.first; i < range.first + range.count; ++i)
                placements[voxelPlacementOffset_++] = scene.objects[i].placement;
        const D3D12_RANGE placementWrites{first * sizeof(XMFLOAT4), voxelPlacementOffset_ * sizeof(XMFLOAT4)};
        voxelPlacements_->Unmap(0, &placementWrites);
        draw(first, visibility.count);
    }
    else
        for (const auto range : visibility.ranges) draw(range.first + 1, range.count);
}

void D3D12Renderer::installMeshPipelines(D3D12_SHADER_BYTECODE cameraVertex, D3D12_SHADER_BYTECODE cameraPixel,
                                         D3D12_SHADER_BYTECODE shadowVertex, D3D12_SHADER_BYTECODE shadowPixel,
                                         const ShadowBiasSettings& bias, bool enableMultisampling)
{
    auto description = meshPipelineDescription_;
    description.VS = cameraVertex;
    description.PS = cameraPixel;
    if (cullMeshBackfaces_) description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    auto singleSample = createCameraPipeline(description, "Create single-sample mesh pipelines");
    CameraPipeline multisample;
    auto shadow = createShadowPipeline(shadowVertex, shadowPixel, bias);
    if (enableMultisampling)
    {
        description.SampleDesc = msaaSamples;
        description.RasterizerState.MultisampleEnable = TRUE;
        multisample = createCameraPipeline(description, "Create 4x MSAA mesh pipelines");
    }

    // The frame loop fences each submission. Commit the whole set between frames.
    singleSample_.mesh = std::move(singleSample);
    multisample_.mesh = std::move(multisample);
    singleSample_.oneSidedMesh = {};
    multisample_.oneSidedMesh = {};
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
    heap.NumDescriptors = static_cast<UINT>(textures.size() + 9 + (skyPanorama_ ? 1u : 0u));
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
    cascadeShadowView_.ptr = handle.ptr + textureDescriptorSize_;
    writeCascadeShadowView();
    handle.ptr = cascadeShadowView_.ptr + textureDescriptorSize_;
    sceneColorView_.ptr = shadowTexture_.ptr + 2ull * textureDescriptorSize_;
    view.Format = sceneColorFormat;
    device_->CreateShaderResourceView(sceneColor_.Get(), &view, handle);
    if (skyPanorama_)
    {
        handle.ptr += textureDescriptorSize_;
        view.Format = sceneColorFormat;
        view.Texture2D.MipLevels = skyPanorama_->GetDesc().MipLevels;
        device_->CreateShaderResourceView(skyPanorama_.Get(), &view, handle);
        skyTexture_.ptr = sceneColorView_.ptr + textureDescriptorSize_;
    }
    probeCpuView_.ptr = handle.ptr + textureDescriptorSize_;
    probeGpuView_.ptr = sceneColorView_.ptr + (skyPanorama_ ? 2ull : 1ull) * textureDescriptorSize_;
    writeProbeView();
    writeTreeDepthViews({probeCpuView_.ptr + textureDescriptorSize_});
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
    const TextureData& mips, std::vector<ComPtr<ID3D12Resource>>& staging, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES ready)
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
    description.Format = format;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&texture)), "Create color texture");

    // D3D12 pads each row and subresource to device copy alignment.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(mips.size());
    std::vector<UINT64> rowSizes(mips.size());
    UINT64 uploadSize = 0;
    device_->GetCopyableFootprints(&description, 0, description.MipLevels, 0,
                                   layouts.data(), nullptr, rowSizes.data(), &uploadSize);
    auto upload = createUploadBuffer(static_cast<std::size_t>(uploadSize));
    const D3D12_RANGE noCpuReads{0, 0};
    void* mapped = nullptr;
    check(upload->Map(0, &noCpuReads, &mapped), "Map color texture upload");
    for (UINT level = 0; level < description.MipLevels; ++level)
    {
        const auto& mip = mips[level];
        const auto& layout = layouts[level];
        const auto rowBytes = static_cast<std::size_t>(rowSizes[level]);
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
                       ready);
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
    if (mesh.instances)
    {
        const auto instanceBytes = static_cast<UINT>(mesh.instances->size() * sizeof(MeshInstance));
        gpu.meshInstances = uploadBuffer(mesh.instances->data(), instanceBytes);
        gpu.meshInstanceView = {gpu.meshInstances->GetGPUVirtualAddress(), instanceBytes, sizeof(MeshInstance)};
        gpu.meshInstanceCount = static_cast<UINT>(mesh.instances->size());
    }
    gpu.localToWorld = mesh.localToWorld;
    const auto world = XMLoadFloat4x4(&mesh.localToWorld);
    XMVECTOR determinant;
    XMStoreFloat4x4(&gpu.normalToWorld, XMMatrixTranspose(XMMatrixInverse(&determinant, world)));
    gpu.mirrored = XMVectorGetX(determinant) < 0.0f;
    gpu.material = mesh.material;
    return gpu;
}

std::unique_ptr<D3D12Renderer::GpuSprayLod> D3D12Renderer::uploadSprayLod(
    const MeshData& mesh, const FixedLod& lod, std::span<const std::uint32_t> boughMembers)
{
    auto gpu = std::make_unique<GpuSprayLod>();
    const auto center = XMLoadFloat3(&mesh.localBounds.center);
    const auto extent = XMLoadFloat3(&mesh.localBounds.halfExtent);
    auto lower = center - extent;
    auto upper = center + extent;
    D3D12_VERTEX_BUFFER_VIEW proxyView{};
    if (!lod.proxyVertices.empty())
    {
        for (const auto& vertex : lod.proxyVertices)
        {
            const auto point = XMLoadFloat3(&vertex.position);
            lower = XMVectorMin(lower, point);
            upper = XMVectorMax(upper, point);
        }
        const auto bytes = static_cast<UINT>(lod.proxyVertices.size() * sizeof(Vertex));
        gpu->proxyVertices = uploadBuffer(lod.proxyVertices.data(), bytes);
        proxyView = {gpu->proxyVertices->GetGPUVirtualAddress(), bytes, sizeof(Vertex)};
    }
    XMStoreFloat3(&gpu->bounds.center, (lower + upper) * 0.5f);
    XMStoreFloat3(&gpu->bounds.halfExtent, (upper - lower) * 0.5f);
    gpu->levels.resize(lod.levels.size());
    for (std::size_t i = 1; i < lod.levels.size(); ++i)
    {
        gpu->levels[i] = uploadFixedLodLevel(lod, i, false);
        if (lod.usesProxyVertices(i)) gpu->levels[i].proxyVertices = proxyView;
        for (const auto& group : lod.groups)
        {
            const auto& error = group.levels[i];
            gpu->errors[i] = std::max({gpu->errors[i], error.surfaceError(), error.rimDeviation, error.simplifierError});
        }
        gpu->errors[i] = std::max(gpu->errors[i], gpu->errors[i - 1]);
    }
    if (!lod.sprayGroups.empty())
    {
        const auto bytes = static_cast<UINT>(lod.sprayGroupIndices.size() * sizeof(std::uint32_t));
        auto& indices = gpu->localLevels[0];
        indices.indexBuffer = uploadBuffer(lod.sprayGroupIndices.data(), bytes);
        indices.indices = {indices.indexBuffer->GetGPUVirtualAddress(), bytes, DXGI_FORMAT_R32_UINT};
        indices.indexCount = static_cast<UINT>(lod.sprayGroupIndices.size());
        gpu->localLevels[1] = indices;
        gpu->localLevels[1].proxyVertices = proxyView;
        gpu->localGroupCount = lod.sprayGroups.size();

        const auto minimum = lod.proxyVertices.empty() ? 0 : fixedLodRimLevel;
        std::vector<float> thresholds{0};
        for (std::size_t level = minimum + 1; level < lod.levels.size(); ++level)
            thresholds.push_back(gpu->errors[level]);
        std::vector<std::size_t> starts;
        for (const auto& group : lod.sprayGroups)
        {
            starts.push_back(gpu->localCuts.size());
            auto lo = XMVectorReplicate(FLT_MAX);
            auto hi = XMVectorReplicate(-FLT_MAX);
            for (const auto& cut : group)
            {
                thresholds.push_back(cut.sourceError);
                gpu->localCuts.push_back(cut);
                if (!lod.proxyVertices.empty())
                {
                    // Include the geometry and its measured displacement back to the reference.
                    const auto padding = XMVectorReplicate(cut.sourceError);
                    const auto& vertices = cut.fittedVertices ? lod.proxyVertices : mesh.vertices;
                    for (auto id : std::span(lod.sprayGroupIndices).subspan(cut.range.firstIndex, cut.range.indexCount))
                    {
                        const auto p = XMLoadFloat3(&vertices[id].position);
                        lo = XMVectorMin(lo, p - padding);
                        hi = XMVectorMax(hi, p + padding);
                    }
                }
            }
            if (!lod.proxyVertices.empty() && !group.empty())
            {
                auto& entry = gpu->leafGroups.emplace_back();
                XMStoreFloat3(&entry.bounds.center, (lo + hi) * 0.5f);
                XMStoreFloat3(&entry.bounds.halfExtent, (hi - lo) * 0.5f);
                entry.firstCut = starts.back();
                entry.cutCount = group.size();
            }
        }
        starts.push_back(gpu->localCuts.size());
        for (const auto& cut : lod.wholeSprayCuts)
        {
            thresholds.push_back(cut.sourceError);
            gpu->localCuts.push_back(cut);
        }
        std::ranges::sort(thresholds);
        thresholds.erase(std::unique(thresholds.begin(), thresholds.end()), thresholds.end());
        for (const auto error : thresholds)
        {
            auto level = lod.levels.size() - 1;
            while (level > minimum && gpu->errors[level] > error) --level;
            GpuSprayLod::CutPlan plan{error, level, {}};
            std::size_t localIndices = 0;
            for (std::size_t g = 0; g < lod.sprayGroups.size(); ++g)
            {
                const auto& group = lod.sprayGroups[g];
                const auto cut = std::ranges::find_if(group, [&](const auto& entry) { return entry.sourceError <= error; });
                if (cut == group.end()) break;
                localIndices += cut->range.indexCount;
                plan.localCuts.push_back(starts[g] + (cut - group.begin()));
            }
            if (plan.localCuts.size() != lod.sprayGroups.size() || localIndices >= lod.levels[level].indices.size())
                plan.localCuts.clear();
            const auto wholeCut = std::ranges::find_if(lod.wholeSprayCuts, [&](const auto& entry) {
                return entry.sourceError <= error;
            });
            const auto currentIndices = plan.localCuts.empty() ? lod.levels[level].indices.size() : localIndices;
            if (wholeCut != lod.wholeSprayCuts.end() && wholeCut->range.indexCount < currentIndices)
                plan.localCuts = {starts.back() + static_cast<std::size_t>(wholeCut - lod.wholeSprayCuts.begin())};
            if (gpu->cutPlans.empty() || gpu->cutPlans.back().wholeLevel != level ||
                gpu->cutPlans.back().localCuts != plan.localCuts)
                gpu->cutPlans.push_back(std::move(plan));
        }
    }
    buildSprayGroups(*gpu, mesh, boughMembers);
    return gpu;
}

std::size_t D3D12Renderer::GpuSprayLod::selectCut(float error) const
{
    const auto after = std::ranges::upper_bound(cutPlans, error, {}, &CutPlan::error);
    return static_cast<std::size_t>(after - cutPlans.begin() - 1);
}

void D3D12Renderer::buildSprayGroups(GpuSprayLod& lod, const MeshData& mesh, std::span<const std::uint32_t> boughMembers)
{
    struct PlacementBounds { BoundingBox world; float maxScale; float minScale; };
    std::vector<PlacementBounds> placements;
    placements.reserve(mesh.instances->size());
    const auto model = XMLoadFloat4x4(&mesh.localToWorld);
    const BoundingBox local(lod.bounds.center, lod.bounds.halfExtent);
    for (const auto& instance : *mesh.instances)
    {
        const auto transform = instance.matrix() * model;
        BoundingBox box;
        local.Transform(box, transform);
        float minScale;
        const float maxScale = maxLinearStretch(transform, &minScale);
        placements.push_back({box, maxScale, minScale});
    }
    std::vector<UINT> order(placements.size());
    std::iota(order.begin(), order.end(), 0u);
    if (!boughMembers.empty())
    {
        // The left root child owns the bough's exact replacement set.
        std::stable_partition(order.begin(), order.end(), [&](auto index) {
            return std::ranges::binary_search(boughMembers, index);
        });
    }
    const auto build = [&](auto&& self, std::span<UINT> ids, UINT first) -> void {
        auto box = placements[ids.front()].world;
        float scale = 0.0f;
        float minScale = std::numeric_limits<float>::max();
        for (auto id : ids)
        {
            BoundingBox::CreateMerged(box, box, placements[id].world);
            scale = std::max(scale, placements[id].maxScale);
            minScale = std::min(minScale, placements[id].minScale);
        }
        const auto node = lod.groups.size();
        lod.groups.push_back({{box.Center, box.Extents}, scale, minScale, first, static_cast<UINT>(ids.size())});
        constexpr UINT leafSize = 32;
        const bool splitBough = node == 0 && !boughMembers.empty();
        if (!splitBough && ids.size() <= leafSize) return;
        const auto split = static_cast<UINT>(splitBough ? boughMembers.size() : ids.size() / 2);
        if (!splitBough)
        {
            const auto half = box.Extents;
            const auto axis = half.x >= half.y && half.x >= half.z ? &XMFLOAT3::x
                : half.y >= half.z ? &XMFLOAT3::y : &XMFLOAT3::z;
            std::nth_element(ids.begin(), ids.begin() + split, ids.end(), [&](UINT a, UINT b) {
                const auto x = placements[a].world.Center.*axis;
                const auto y = placements[b].world.Center.*axis;
                return x == y ? a < b : x < y;
            });
        }
        self(self, ids.first(split), first);
        lod.groups[node].rightChild = lod.groups.size();
        self(self, ids.subspan(split), first + split);
    };
    build(build, order, 0);
    if (sprayGroupTags_.empty())
    {
        // Use the leaf hierarchy for both primitives so the leaves and twigs of a spray agree.
        sprayGroupTags_.resize(order.size());
        for (std::uint32_t groupId = 0; groupId < lod.groups.size(); ++groupId)
        {
            const auto& group = lod.groups[groupId];
            if (group.rightChild) continue;
            for (auto i = group.first; i < group.first + group.count; ++i)
                sprayGroupTags_[order[i]] = {0, groupId, 0};
        }
        spraySourceGroupTags_ = uploadBuffer(sprayGroupTags_.data(), sprayGroupTags_.size() * sizeof(LodTag));
    }
    lod.groupedInstances.reserve(order.size());
    lod.groupedTags.reserve(order.size());
    for (auto index : order)
    {
        lod.groupedInstances.push_back((*mesh.instances)[index]);
        lod.groupedTags.push_back(sprayGroupTags_[index]);
    }
}

D3D12Renderer::GpuFixedLodLevel D3D12Renderer::uploadFixedLodLevel(const FixedLod& lod, std::size_t levelIndex,
                                                                bool tagTriangles)
{
    const auto& level = lod.levels[levelIndex];
    const auto indexBytes = level.indices.size() * sizeof(std::uint32_t);
    GpuFixedLodLevel gpu;
    gpu.indexBuffer = uploadBuffer(level.indices.data(), indexBytes);
    gpu.indices = {gpu.indexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};
    gpu.indexCount = static_cast<UINT>(level.indices.size());
    if (!tagTriangles) return gpu;

    // Primitive IDs are draw-local. Use the same batch layout as the baked index buffer.
    std::vector<LodTag> tags;
    tags.reserve(level.indices.size() / 3);
    for (std::uint32_t group = 0; group < lod.groups.size(); ++group)
    {
        const auto& range = lod.groups[group].levels[levelIndex];
        for (auto cluster = range.firstCluster; cluster < range.firstCluster + range.clusterCount; ++cluster)
        {
            const auto& draw = level.clusters[cluster];
            for (auto i = draw.firstIndex; i < draw.firstIndex + draw.indexCount; i += 3)
            {
                std::array vertices{level.indices[i], level.indices[i + 1], level.indices[i + 2]};
                std::sort(vertices.begin(), vertices.end());
                std::uint32_t key = 2166136261u;
                for (auto index : vertices) key = (key ^ index) * 16777619u;
                tags.push_back({key, group, cluster});
            }
        }
    }
    gpu.triangleTags = uploadBuffer(tags.data(), tags.size() * sizeof(LodTag));
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

XMFLOAT2 D3D12Renderer::updateFrameConstants(const ViewerScene& scene, FXMMATRIX viewProjection, float windPhase)
{
    FrameConstants constants{};
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    constants.lightViewProjection = lightViewProjection_;
    constants.toSun = toSun_;
    constants.shadowBias = scene.shadowBias.samplingTexels * shadowDepthPerTexel_;
    constants.normalBiasWorld = scene.shadowBias.normalTexels * shadowWorldUnitsPerTexel_;
    // Harmonics give the whole tree a shared gust with a small crosswind component.
    const float gust = 0.70f * std::sin(windPhase) + 0.16f * std::sin(2.0f * windPhase + 0.55f);
    const float crosswind = 0.14f * std::sin(3.0f * windPhase + 0.7f);
    const float amplitude = scene.branchVoxelView == BranchVoxelView::Off ? windAmplitude_ * scene.windStrength : 0;
    constants.windOffset = {amplitude * (0.94f * gust - 0.34f * crosswind),
                            amplitude * (0.34f * gust + 0.94f * crosswind)};
    constants.windStart = windStart_;
    constants.inverseWindHeight = inverseWindHeight_;
    constants.useCascades = scene.usesSunCascades();
    constants.showCascadeColors = scene.showCascadeColors;
    constants.encodedCamera = encodedCamera_;
    constants.directionalSkylight = skyPanorama_ && !encodedCamera_ && !scene.legacyAmbientForDiagnostics;
    constants.lightingMode = static_cast<UINT>(scene.captureLighting);
    if (probesActive_) constants.probes = probeGrid_;
    if (constants.useCascades)
    {
        constants.cascadeSplits = {sunCascadeSplits[0], sunCascadeSplits[1], sunCascadeSplits[2], 0};
        constants.filterCascades = scene.filterSunCascades;
        if (scene.blendSunCascades)
            constants.cascadeBlendWidths = {sunCascadeBlendWidths[0], sunCascadeBlendWidths[1], sunCascadeBlendWidths[2], 0};
        constants.cascadeViewProjections = cascadeViewProjections_;
        constants.cascadeDepthPerTexel = {cascadeDepthPerTexel_[0], cascadeDepthPerTexel_[1], cascadeDepthPerTexel_[2], 0};
        constants.lightRight = {sunRotation_._11, sunRotation_._21, sunRotation_._31, 0};
        constants.cascadeShadowBias = {scene.shadowBias.samplingTexels * cascadeDepthPerTexel_[0],
                                       scene.shadowBias.samplingTexels * cascadeDepthPerTexel_[1],
                                       scene.shadowBias.samplingTexels * cascadeDepthPerTexel_[2], 0};
        constants.cascadeNormalBias = {scene.shadowBias.normalTexels * cascadeWorldUnitsPerTexel_[0],
                                       scene.shadowBias.normalTexels * cascadeWorldUnitsPerTexel_[1],
                                       scene.shadowBias.normalTexels * cascadeWorldUnitsPerTexel_[2], 0};
    }

    const D3D12_RANGE noCpuReads{0, 0};
    const D3D12_RANGE written{0, constants.useCascades ? sunCascadeSplits.size() * frameConstantStride + sizeof(constants) :
                                                     sizeof(constants)};
    void* mapped = nullptr;
    check(frameConstants_->Map(0, &noCpuReads, &mapped), "Map frame constants");
    std::memcpy(mapped, &constants, sizeof(constants));
    if (constants.useCascades)
    {
        auto shadowConstants = constants;
        for (std::size_t i = 0; i < sunCascadeSplits.size(); ++i)
        {
            shadowConstants.lightViewProjection = cascadeViewProjections_[i];
            std::memcpy(static_cast<std::byte*>(mapped) + (i + 1) * frameConstantStride, &shadowConstants, sizeof(shadowConstants));
        }
    }
    frameConstants_->Unmap(0, &written);
    return constants.windOffset;
}

void D3D12Renderer::createProbePipeline()
{
    D3D12_DESCRIPTOR_RANGE panorama{};
    panorama.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    panorama.NumDescriptors = 1;
    panorama.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER parameters[7]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 38;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {1, &panorama};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[4].Descriptor.ShaderRegister = 1;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[5].Descriptor.ShaderRegister = 2;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[6].Descriptor.ShaderRegister = 2;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    const D3D12_ROOT_SIGNATURE_DESC root{static_cast<UINT>(std::size(parameters)), parameters, 1, &sampler,
                                        D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> bytes;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, nullptr),
          "Serialize probe update root signature");
    check(device_->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                       IID_PPV_ARGS(&probeRoot_)), "Create probe update root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = probeRoot_.Get();
    pipeline.CS = {probeCS, sizeof(probeCS)};
    check(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&probePipeline_)), "Create probe update pipeline");
    pipeline.CS = {groundProbeCS, sizeof(groundProbeCS)};
    check(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&groundProbePipeline_)), "Create ground lighting pipeline");
    pipeline.CS = {advectProbeCS, sizeof(advectProbeCS)};
    check(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&advectProbePipeline_)), "Create probe density advection pipeline");
}

unsigned D3D12Renderer::probeCount() const
{
    return probeGrid_.size.x * probeGrid_.size.y * probeGrid_.size.z;
}

std::uint64_t D3D12Renderer::probeBytes() const
{
    if (!diffuseProbes_) return 0;
    return diffuseProbes_->GetDesc().Width + probeDensity_->GetDesc().Width +
           probeGroundLight_->GetDesc().Width + probeUpdateOrder_->GetDesc().Width +
           (advectedProbeDensity_ ? advectedProbeDensity_->GetDesc().Width : 0);
}

void D3D12Renderer::writeProbeView()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = std::max(1u, probeCount() * 6);
    view.Buffer.StructureByteStride = 16;
    device_->CreateShaderResourceView(diffuseProbes_.Get(), &view, probeCpuView_);
}

D3D12Renderer::ComPtr<ID3D12Resource> D3D12Renderer::createProbeBuffer(
    UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
          IID_PPV_ARGS(&resource)), "Allocate diffuse probe buffer");
    return resource;
}

void D3D12Renderer::initializeProbeField(const ViewerScene& scene)
{
    if (!probePipeline_) createProbePipeline();
    auto volume = buildProbeProxy(scene);
    probeGrid_ = volume.probes;
    probeDensityOrigin_ = volume.originCellSize;
    probeDensitySize_ = volume.size;

    const auto densityBytes = volume.density.size() * sizeof(XMFLOAT4);
    probeDensity_ = createProbeBuffer(densityBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    diffuseProbes_ = createProbeBuffer(static_cast<UINT64>(probeCount()) * 6 * 16,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const auto groundCells = ((volume.size.x + 1) / 2) * ((volume.size.z + 1) / 2);
    probeGroundLight_ = createProbeBuffer(static_cast<UINT64>(groundCells) * sizeof(XMFLOAT4),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    probeUpdateOrder_ = createUploadBuffer(probeCount() * sizeof(UINT));
    auto staging = uploadBuffer(volume.density.data(), densityBytes);
    check(allocator_->Reset(), "Reset probe upload allocator");
    check(commands_->Reset(allocator_.Get(), nullptr), "Reset probe upload command list");
    commands_->CopyBufferRegion(probeDensity_.Get(), 0, staging.Get(), 0, densityBytes);
    transitionResource(commands_.Get(), probeDensity_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    check(commands_->Close(), "Close probe upload command list");
    ID3D12CommandList* lists[] = {commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitForGpu();
    writeProbeView();
}

void D3D12Renderer::orderProbeUpdates()
{
    const auto planes = frustumPlanes(XMLoadFloat4x4(&frameView_) * XMLoadFloat4x4(&projection_));
    const auto& size = probeGrid_.size;
    const auto& origin = probeGrid_.originSpacing;
    // Include the interpolation footprint around the frustum. Each sweep still visits every probe.
    const float padding = 2.0f * origin.w;
    std::vector<UINT> order(probeCount());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_partition(order.begin(), order.end(), [&](UINT index) {
        const XMFLOAT3 center{origin.x + (index % size.x) * origin.w,
                             origin.y + (index / size.x % size.y) * origin.w,
                             origin.z + (index / (size.x * size.y)) * origin.w};
        return intersectsFrustum(BoundingBox(center, {padding, padding, padding}), planes);
    });
    const D3D12_RANGE noCpuReads{0, 0};
    const D3D12_RANGE written{0, order.size() * sizeof(UINT)};
    void* mapped = nullptr;
    check(probeUpdateOrder_->Map(0, &noCpuReads, &mapped), "Map probe update order");
    std::memcpy(mapped, order.data(), written.End);
    probeUpdateOrder_->Unmap(0, &written);
}

D3D12Renderer::ProbeUpdate D3D12Renderer::updateProbeField(const ViewerScene& scene)
{
    if (!probesActive_) return {};
    const bool bounce = scene.probeLighting == ProbeLightingMode::GroundBounce;
    const XMFLOAT3 groundColor{scene.groundMaterial.color.x, scene.groundMaterial.color.y, scene.groundMaterial.color.z};
    const bool changed = cachedProbeMode_ != scene.probeLighting || (bounce &&
        (cachedProbeLighting_ != scene.captureLighting ||
         !XMVector3Equal(XMLoadFloat3(&cachedProbeSun_), XMLoadFloat3(&toSun_)) ||
         !XMVector3Equal(XMLoadFloat3(&cachedGroundColor_), XMLoadFloat3(&groundColor))));
    const bool windMoving = observedProbeWind_.x != frameWind_.x || observedProbeWind_.y != frameWind_.y;
    const bool windStopped = probeWindMoving_ && !windMoving;
    observedProbeWind_ = frameWind_;
    probeWindMoving_ = windMoving;
    constexpr unsigned maxWindInterval = ViewerScene::probeWindMaxInterval;
    if (windMoving) probeWindAge_ = std::min(probeWindAge_ + 1, maxWindInterval);
    const float dx = frameWind_.x - cachedProbeWind_.x;
    const float dz = frameWind_.y - cachedProbeWind_.y;
    const float threshold = scene.probeWindRefreshVoxels * probeDensityOrigin_.w;
    const bool windDue = windMoving && (dx * dx + dz * dz >= threshold * threshold ||
                                       probeWindAge_ >= maxWindInterval);
    if ((changed && !probeLightChangedLastFrame_) || windStopped) probeCursor_ = 0;
    probeLightChangedLastFrame_ = changed;
    if (changed || windStopped) probeCatchup_ = true;
    if (changed || windDue || windStopped) probesPending_ = probeCount();
    if (!probesDirty_ && (!probesPending_ || (!probeCatchup_ && !windDue))) return {};
    // A lighting change or a stopped gust must converge at the current pose.
    // Ordinary wind work advances only when the proxy accepts a new pose.
    const bool refreshAllGround = probesDirty_ || changed || windStopped;
    const bool acceptWind = refreshAllGround || windDue;
    const auto wind = acceptWind ? frameWind_ : cachedProbeWind_;
    if (!probeCursor_ && (probesDirty_ || probesPending_ == probeCount())) orderProbeUpdates();
    constexpr unsigned updatesPerFrame = 256;
    const unsigned count = probesDirty_ ? probeCount() :
        std::min({updatesPerFrame, probesPending_, probeCount() - probeCursor_});
    constexpr unsigned groundRowsPerCycle = 8;
    const bool updateGround = bounce && acceptWind;
    const unsigned groundStride = refreshAllGround ? 1 : groundRowsPerCycle;
    const unsigned groundRow = refreshAllGround ? 0 : groundUpdateRow_;
    const bool windDisplaced = wind.x != 0 || wind.y != 0;
    const bool advectDensity = windDisplaced && ((acceptWind && (dx != 0 || dz != 0)) || !advectedProbeDensity_);
    if (advectDensity && !advectedProbeDensity_)
        advectedProbeDensity_ = createProbeBuffer(probeDensity_->GetDesc().Width,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    struct Constants
    {
        XMFLOAT4 proxyOriginCellSize;
        XMUINT4 proxySize;
        DiffuseProbeGrid grid;
        float skyRadianceScale;
        UINT first;
        UINT count;
        UINT bounce;
        XMFLOAT3 toSun;
        float sunStrength;
        XMFLOAT3 groundColor;
        float groundHalfWidth;
        XMFLOAT3 openSkyGround;
        float groundHalfDepth;
        XMFLOAT2 wind;
        float windStart;
        float inverseWindHeight;
        UINT groundStride;
        UINT groundRow;
    };
    static_assert(sizeof(Constants) == 38 * sizeof(UINT));
    const auto& extent = scene.ground.localBounds.halfExtent;
    const Constants constants{probeDensityOrigin_, probeDensitySize_, probeGrid_, skyRadianceScale_, probeCursor_, count, bounce,
        toSun_, scene.captureLighting == CaptureLighting::SkyOnly ? 0.0f : 0.65f, groundColor, extent.x,
        openSkyGround_, extent.z, wind, windStart_, inverseWindHeight_, groundStride, groundRow};
    transitionResource(commands_.Get(), diffuseProbes_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands_->SetComputeRootSignature(probeRoot_.Get());
    commands_->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(UINT), &constants, 0);
    commands_->SetComputeRootDescriptorTable(2, skyTexture_);
    commands_->SetComputeRootUnorderedAccessView(3, diffuseProbes_->GetGPUVirtualAddress());
    commands_->SetComputeRootUnorderedAccessView(4, probeGroundLight_->GetGPUVirtualAddress());
    commands_->SetComputeRootShaderResourceView(5, probeUpdateOrder_->GetGPUVirtualAddress());
    // Warp the coverage proxy once per distinct wind pose. Both tracing passes use the result.
    if (advectDensity)
    {
        transitionResource(commands_.Get(), advectedProbeDensity_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands_->SetComputeRootShaderResourceView(1, probeDensity_->GetGPUVirtualAddress());
        commands_->SetComputeRootUnorderedAccessView(6, advectedProbeDensity_->GetGPUVirtualAddress());
        commands_->SetPipelineState(advectProbePipeline_.Get());
        commands_->Dispatch((probeDensitySize_.x + 7) / 8, (probeDensitySize_.y + 3) / 4,
                            (probeDensitySize_.z + 3) / 4);
        transitionResource(commands_.Get(), advectedProbeDensity_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    const auto& density = windDisplaced ? advectedProbeDensity_ : probeDensity_;
    commands_->SetComputeRootShaderResourceView(1, density->GetGPUVirtualAddress());
    if (updateGround)
    {
        commands_->SetPipelineState(groundProbePipeline_.Get());
        const unsigned rows = ((probeDensitySize_.z + 1) / 2 + groundStride - 1) / groundStride;
        commands_->Dispatch((probeDensitySize_.x + 15) / 16, (rows + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = probeGroundLight_.Get();
        commands_->ResourceBarrier(1, &barrier);
    }
    commands_->SetPipelineState(probePipeline_.Get());
    commands_->Dispatch((count + 31) / 32, 1, 1);
    transitionResource(commands_.Get(), diffuseProbes_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    probesDirty_ = false;
    probeCursor_ = (probeCursor_ + count) % probeCount();
    probesPending_ -= std::min(probesPending_, count);
    if (!probesPending_) probeCatchup_ = false;
    if (updateGround) groundUpdateRow_ = refreshAllGround ? 0 : (groundRow + 1) % groundRowsPerCycle;
    if (acceptWind)
    {
        cachedProbeWind_ = wind;
        probeWindAge_ = 0;
    }
    cachedProbeMode_ = scene.probeLighting;
    cachedProbeLighting_ = scene.captureLighting;
    cachedProbeSun_ = toSun_;
    cachedGroundColor_ = groundColor;
    return {count, advectDensity, updateGround};
}

D3D12Renderer::FrameTiming D3D12Renderer::drawFrame(
    const ViewerScene& scene, const XMFLOAT4X4& view, float windPhase, std::string_view hud,
    const std::function<void(ID3D12GraphicsCommandList*)>& drawOverlay, std::vector<std::uint32_t>* capture,
    const XMFLOAT4X4* cascadeFitView, std::vector<std::uint64_t>* linearCapture)
{
    using Clock = std::chrono::steady_clock;
    const auto cpuStart = Clock::now();
    updateShadowPipeline(scene.shadowBias);
    const bool cascades = scene.usesSunCascades();
    const bool voxelView = scene.branchVoxelView != BranchVoxelView::Off;
    showShadowFactor_ = !voxelView && scene.showShadowFactor;
    captureGeometryMask_ = scene.captureGeometryMask;
    boughDepth_ = scene.captureBoughDepth;
    boughMaterials_ = scene.captureBoughMaterials;
    branchAppearance_ = voxelView ? scene.branchAppearance : BranchAppearance::Shaded;
    diagnosticFoliage_ = branchAppearance_ != BranchAppearance::Shaded ? &meshes_[scene.fixedLod->meshIndex] : nullptr;
    // Diagnostics use UNORM rasterization and resolve so encoded RGB and alpha tags retain their values.
    encodedCamera_ = captureGeometryMask_ || showShadowFactor_ || scene.branchVoxelDepthCapture ||
                     branchAppearance_ != BranchAppearance::Shaded || (cascades && scene.showCascadeColors) ||
                     (!voxelView && (scene.lodTint != FixedLodTint::Shaded || scene.highlightLodGroup));
    if (linearCapture && encodedCamera_)
        throw std::runtime_error("Linear HDR capture requires a shaded camera view.");
    if (scene.captureLighting != CaptureLighting::Combined && encodedCamera_)
        throw std::runtime_error("Lighting component capture requires a shaded camera view.");
    useLeafResponse_ = !encodedCamera_ && !scene.legacyAmbientForDiagnostics;
    probesActive_ = skyPanorama_ && useLeafResponse_ && scene.probeLighting != ProbeLightingMode::Off &&
                    scene.captureLighting != CaptureLighting::SunOnly;
    if (probesActive_ && !diffuseProbes_) initializeProbeField(scene);
    if (cascades)
    {
        if (!cascadeShadowMap_) createCascadeShadowMaps();
        updateShadowCascades(cascadeFitView ? *cascadeFitView : view, scene);
    }
    const auto viewProjection = XMLoadFloat4x4(&view) * XMLoadFloat4x4(&projection_);
    const auto windOffset = updateFrameConstants(scene, viewProjection, windPhase);
    frameView_ = view;
    frameWind_ = windOffset;
    sprayInstanceOffset_ = 0;
    treeArgumentOffset_ = 0;
    treeGpuReadbackParts_.clear();
    treeBatch_.passes.clear();
    treeBatch_.depthUsed = false;
    treeDepthReady_ = false;
    prepareVisibility(scene, viewProjection, windOffset);
    const bool automaticVoxels = automaticBranchVoxels(scene.branchVoxelView);
    const bool regional = scene.branchVoxelView == BranchVoxelView::Regional && scene.branchVoxelRegions;
    const bool experiment = voxelView && !automaticVoxels;
    double drawListMs = 0.0;
    voxelDrawSlot_ = 0;
    voxelPlacementOffset_ = 0;
    const auto drawListStart = Clock::now();
    prepareFoliageGroupVisibility(scene, viewProjection, windOffset);
    const bool groupDraws = !cameraGroupVisibility_.empty();
    const bool automaticTriangles = automaticVoxels || scene.branchLod == automaticBranchLod;
    const bool cascadeCasterLists = scene.usesCascadeCasterLists();
    std::size_t argumentBytes = 0;
    shareBranchDraws_ = true;
    if (!experiment && scene.fixedLod && (automaticTriangles || groupDraws))
    {
        bool mixedRegions = false;
        if (automaticVoxels)
        {
            cameraTriangleBranches_.clear();
            cameraVoxelBranches_.clear();
            for (auto& visibility : cameraVoxelRegions_) visibility.clear();
            const auto& cut = scene.branchLodSelection.voxelCut;
            for (const auto range : cameraVisibility_.ranges)
            {
                for (UINT i = range.first; i < range.first + range.count; ++i)
                {
                    const auto mask = i < cut.size() ? cut[i] : 0;
                    if (mask == allVoxelRegions)
                    {
                        cameraVoxelBranches_.add(i);
                        continue;
                    }
                    cameraTriangleBranches_.add(i);
                    if (!regional || !mask) continue;
                    mixedRegions = true;
                    for (std::size_t region = 0; region < voxelBranchRegionCount; ++region)
                        if (mask & (1u << region)) cameraVoxelRegions_[region].add(i);
                }
            }
        }
        const auto& triangleVisibility = automaticVoxels ? cameraTriangleBranches_ : cameraVisibility_;
        prepareBranchDraws(scene, triangleVisibility, cameraBranchDraws_, argumentBytes, mixedRegions,
                           cameraGroupVisibility_, automaticTriangles ? automaticBranchLod : scene.branchLod);
        const bool hasOverride = scene.branchShadowLod >= 0;
        if (!cascadeCasterLists && automaticTriangles && (!hasOverride || scene.shadowLodOffCameraOnly))
        {
            const auto& matching = hasOverride ? shadowMatchVisibility_ : shadowVisibility_;
            shareBranchDraws_ = !mixedRegions && !groupDraws && triangleVisibility.ranges == matching.ranges;
            if (!shareBranchDraws_)
            {
                prepareBranchDraws(scene, matching, shadowBranchDraws_, argumentBytes);
            }
        }
        drawListMs = std::chrono::duration<double, std::milli>(Clock::now() - drawListStart).count();
    }
    if (cascadeCasterLists)
    {
        prepareCascadeCasterDraws(scene, windOffset, argumentBytes);
        drawListMs = std::chrono::duration<double, std::milli>(Clock::now() - drawListStart).count();
    }
    const auto idleState = swapChain_ ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const UINT bufferIndex = swapChain_ ? swapChain_->GetCurrentBackBufferIndex() : 0;
    auto* backBuffer = backBuffers_[bufferIndex].Get();
    const auto& mode = msaaEnabled_ ? multisample_ : singleSample_;
    auto* resolvedColor = encodedCamera_ ? backBuffer : sceneColor_.Get();
    const auto linearIdle = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto resolvedIdle = encodedCamera_ ? idleState : linearIdle;
    const auto resolvedReady = encodedCamera_ ? D3D12_RESOURCE_STATE_RENDER_TARGET : linearIdle;
    const auto format = encodedCamera_ ? colorFormat : sceneColorFormat;
    auto* cameraColor = resolvedColor;
    auto colorTarget = encodedCamera_ ? renderTargets_[bufferIndex] : sceneTarget_;
    if (msaaEnabled_)
    {
        auto& color = encodedCamera_ ? encodedMultisampleColor_ : multisampleColor_;
        colorTarget = encodedCamera_ ? encodedMultisampleTarget_ : multisampleTarget_;
        if (!color) color = createCameraColor(format, msaaSamples, colorTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cameraColor = color.Get();
    }
    check(allocator_->Reset(), "Reset command allocator");
    check(commands_->Reset(allocator_.Get(), shadowPipeline_.Get()), "Reset command list");
    const auto timestamp = [&](FrameTimestamp index) {
        if (timestampQueries_) commands_->EndQuery(timestampQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    };
    timestamp(FrameStart);

    ID3D12DescriptorHeap* heaps[] = {textureHeap_.Get()};
    commands_->SetDescriptorHeaps(1, heaps);
    const auto probeUpdate = updateProbeField(scene);
    timestamp(ProbeEnd);
    commands_->SetPipelineState(shadowPipeline_.Get());
    commands_->SetGraphicsRootSignature(rootSignature_.Get());
    commands_->SetGraphicsRootConstantBufferView(3, frameConstants_->GetGPUVirtualAddress());
    commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    timestamp(ShadowStart);
    std::array<PassStats, sunCascadeSplits.size()> shadowCascades{};
    auto shadow = cascades ? drawCascadeShadowMaps(scene, shadowCascades) : drawShadowMap(scene);
    timestamp(ShadowEnd);
    commands_->SetPipelineState(mode.mesh.get(encodedCamera_));
    commands_->SetGraphicsRootDescriptorTable(2, shadowTexture_);
    commands_->SetGraphicsRootConstantBufferView(5, skyIrradiance_->GetGPUVirtualAddress());
    commands_->SetGraphicsRootDescriptorTable(8, probeGpuView_);

    if (!msaaEnabled_)
    {
        transitionResource(commands_.Get(), cameraColor, resolvedIdle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    constexpr float geometryMaskBackground[] = {0, 0, 0, 1};
    const auto* background = encodedCamera_ ? clearColor : sceneClearColor;
    const bool showBackdrop = !captureGeometryMask_ && scene.captureLighting != CaptureLighting::Transmission &&
                              scene.captureLighting != CaptureLighting::ShadowVisibility;
    if (!showBackdrop) background = geometryMaskBackground;
    commands_->ClearRenderTargetView(colorTarget, background, 0, nullptr);
    commands_->ClearDepthStencilView(mode.depthTarget, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commands_->RSSetViewports(1, &viewport_);
    commands_->RSSetScissorRects(1, &scissor_);
    commands_->OMSetRenderTargets(1, &colorTarget, FALSE, &mode.depthTarget);
    for (auto& selected : selectedBoughTrees_) selected.clear();
    if (scene.cameraPipelineStats)
    {
        if (!timestampReadback_) throw std::runtime_error("Camera pipeline statistics require a query readback.");
        if (!cameraPipelineQuery_)
        {
            const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,1,0};
            check(device_->CreateQueryHeap(&query,IID_PPV_ARGS(&cameraPipelineQuery_)),"Create camera pipeline statistics");
        }
        commands_->BeginQuery(cameraPipelineQuery_.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
    }
    if (oakRaster_ == OakRaster::Visibility)
    {
        const auto target = oakIdHeap_->GetCPUDescriptorHandleForHeapStart();
        constexpr float empty[4]{};
        commands_->ClearRenderTargetView(target, empty, 0, nullptr);
        commands_->OMSetRenderTargets(1, &target, FALSE, &mode.depthTarget);
    }
    else if (oakRaster_ == OakRaster::Depth)
        commands_->OMSetRenderTargets(0, nullptr, FALSE, &mode.depthTarget);
    auto camera = drawMeshes(scene, false);
    if (scene.cameraPipelineStats)
    {
        if (oakRaster_ == OakRaster::Live)
            commands_->EndQuery(cameraPipelineQuery_.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
        commands_->ResolveQueryData(cameraPipelineQuery_.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0,1,
                                    timestampReadback_.Get(),TimestampCount*sizeof(UINT64));
    }
    timestamp(BoughBrickStart);
    for (UINT slot = 0; slot < boughVoxelLevels_.size(); ++slot)
        if (boughVoxelLevels_[slot].brickCount)
        {
            const auto& visible = boughReplace_ ? selectedBoughTrees_[slot] : cameraVisibility_;
            if (scene.softwareBoughRaster && !msaaEnabled_)
            {
                drawBranchVoxels(scene,boughVoxelLevels_[slot],XMLoadFloat4x4(&view),visible,camera,false);
                continue;
            }
            for (const auto range : visible.ranges)
                for (UINT i = range.first; i < range.first + range.count; ++i)
                {
                    PassVisibility one;
                    one.add(i);
                    drawBranchVoxels(scene,boughVoxelLevels_[slot],XMLoadFloat4x4(&view),one,camera,false,false,&scene.objects[i]);
                }
        }
    timestamp(BoughBrickEnd);
    if (const auto voxelIndex = branchVoxelIndex(scene.branchVoxelView))
        drawBranchVoxels(scene, voxelLevels_[*voxelIndex], XMLoadFloat4x4(&view), cameraVisibility_, camera, false);
    else if (automaticVoxels)
    {
        drawBranchVoxels(scene, voxelLevels_[automaticBranchVoxelIndex], XMLoadFloat4x4(&view), cameraVoxelBranches_, camera, true);
        if (regional)
            for (std::size_t i = 0; i < voxelBranchRegionCount; ++i)
                drawBranchVoxels(scene, voxelRegionLevels_[i], XMLoadFloat4x4(&view), cameraVoxelRegions_[i], camera, true);
    }

    timestamp(TreeDepthStart);
    finishTreeBatchDepth(viewProjection);
    timestamp(TreeDepthEnd);
    if (auto* skyPipeline = mode.sky.get(encodedCamera_); skyPipeline && showBackdrop)
    {
        drawSky(skyPipeline, view, scene.captureLighting == CaptureLighting::SkyOnly);
        ++camera.draws;
        ++camera.triangles;
    }

    if (msaaEnabled_)
    {
        transitionResource(commands_.Get(), cameraColor, D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        transitionResource(commands_.Get(), resolvedColor, resolvedIdle, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        commands_->ResolveSubresource(resolvedColor, 0, cameraColor, 0, format);
        transitionResource(commands_.Get(), cameraColor, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
        transitionResource(commands_.Get(), resolvedColor, D3D12_RESOURCE_STATE_RESOLVE_DEST, resolvedReady);
    }
    else
    {
        transitionResource(commands_.Get(), resolvedColor, D3D12_RESOURCE_STATE_RENDER_TARGET, resolvedReady);
    }
    if (!encodedCamera_)
    {
        transitionResource(commands_.Get(), backBuffer, idleState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands_->OMSetRenderTargets(1, &renderTargets_[bufferIndex], FALSE, nullptr);
        drawDisplay(scene.exposureEv);
    }
    timestamp(CameraEnd);
    if (linearCapture) copyFrameColor(sceneColor_.Get(), linearIdle, linearIdle, linearReadback_);
    if (drawOverlay) commands_->OMSetRenderTargets(1, &renderTargets_[bufferIndex], FALSE, nullptr);
    if (swapChain_ && !hud.empty()) drawHud(renderTargets_[bufferIndex], hud);
    if (drawOverlay)
    {
        drawOverlay(commands_.Get());
    }
    if (capture) copyFrameColor(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, idleState, displayReadback_);
    else transitionResource(commands_.Get(), backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, idleState);
    timestamp(FrameEnd);
    resolveTreeBatchTimes();
    if (timestampQueries_)
    {
        commands_->ResolveQueryData(timestampQueries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, TimestampCount,
                                    timestampReadback_.Get(), 0);
    }
    check(commands_->Close(), "Close command list");

    ID3D12CommandList* lists[] = {commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const double cpuMs = std::chrono::duration<double, std::milli>(Clock::now() - cpuStart).count();
    const HRESULT presentResult = swapChain_ ? swapChain_->Present(1, 0) : S_OK;

    // Keep one frame in flight so we can safely reuse this allocator next frame.
    waitForGpu();
    check(presentResult, "Present frame");
    readTreeGpuStats(scene, camera, shadow, shadowCascades);
    readLiveGpuStats(camera);
    readTreeBatchStats(scene, camera, shadow, shadowCascades);
    if (scene.cameraPipelineStats)
    {
        const size_t offset=TimestampCount*sizeof(UINT64);
        const D3D12_RANGE range{offset,offset+sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
        void* mapped=nullptr;
        check(timestampReadback_->Map(0,&range,&mapped),"Read camera pipeline statistics");
        D3D12_QUERY_DATA_PIPELINE_STATISTICS counters{};
        std::memcpy(&counters,static_cast<const std::byte*>(mapped)+offset,sizeof(counters));
        const D3D12_RANGE noWrites{0,0};
        timestampReadback_->Unmap(0,&noWrites);
        camera.vertexInvocations=counters.VSInvocations;
        camera.inputPrimitives=counters.IAPrimitives;
        camera.pixelInvocations=counters.PSInvocations;
    }
    if (capture) readFrameColor(displayReadback_, *capture);
    if (linearCapture) readFrameColor(linearReadback_, *linearCapture);
    FrameTiming timing{};
    timing.cpuMilliseconds = cpuMs;
    timing.probesUpdated = probeUpdate.count;
    timing.probesPending = probesActive_ ? probesPending_ : 0;
    timing.probeWindAdvection = probeUpdate.advected;
    timing.probeGroundUpdate = probeUpdate.groundUpdated;
    timing.drawCalls = shadow.draws + camera.draws - shadow.treeDraws - camera.treeDraws +
                       shadow.treeSubmissionCalls + camera.treeSubmissionCalls + (encodedCamera_ ? 0u : 1u) +
                       (swapChain_ && !hud.empty() ? 1u : 0u);
    timing.camera = camera;
    timing.shadow = shadow;
    timing.shadowCascades = shadowCascades;
    timing.cpuBranchDrawListMilliseconds = drawListMs;
    readGpuTimes(timing);
    return timing;
}

void D3D12Renderer::copyFrameColor(ID3D12Resource* color, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after, FrameReadback& readback)
{
    if (!readback.buffer)
    {
        const auto texture = color->GetDesc();
        device_->GetCopyableFootprints(&texture, 0, 1, 0, &readback.layout, nullptr, nullptr, &readback.bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = readback.bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback.buffer)), "Create frame color readback");
    }
    D3D12_TEXTURE_COPY_LOCATION source{color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION destination{readback.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = readback.layout;
    transitionResource(commands_.Get(), color, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transitionResource(commands_.Get(), color, D3D12_RESOURCE_STATE_COPY_SOURCE, after);
}

template <typename Pixel>
void D3D12Renderer::readFrameColor(const FrameReadback& readback, std::vector<Pixel>& pixels)
{
    const auto& footprint = readback.layout.Footprint;
    pixels.resize(static_cast<std::size_t>(footprint.Width) * footprint.Height);
    const D3D12_RANGE readRange{0, readback.bytes};
    void* mapped = nullptr;
    check(readback.buffer->Map(0, &readRange, &mapped), "Read frame color");
    for (UINT y = 0; y < footprint.Height; ++y)
    {
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * footprint.Width,
                    static_cast<const std::byte*>(mapped) + static_cast<std::size_t>(y) * footprint.RowPitch,
                    footprint.Width * sizeof(Pixel));
    }
    const D3D12_RANGE noWrites{0, 0};
    readback.buffer->Unmap(0, &noWrites);
}

D3D12Renderer::PassStats D3D12Renderer::drawShadowMap(const ViewerScene& scene)
{
    transitionResource(commands_.Get(), shadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commands_->ClearDepthStencilView(shadowDepthTarget_, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, shadowMapSize, shadowMapSize, 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, shadowMapSize, shadowMapSize};
    commands_->RSSetViewports(1, &viewport);
    commands_->RSSetScissorRects(1, &scissor);
    commands_->OMSetRenderTargets(0, nullptr, FALSE, &shadowDepthTarget_);
    const auto voxelIndex = scene.diagnosticVoxelShadowCasters ? branchVoxelIndex(scene.branchVoxelView) : std::nullopt;
    auto stats = drawMeshes(scene, true, voxelIndex.has_value());
    if (voxelIndex && scene.meshShadows[scene.fixedLod->meshIndex].cast)
    {
        drawBranchVoxels(scene, voxelLevels_[*voxelIndex], XMMatrixIdentity(), shadowVisibility_, stats, false, true);
        commands_->SetGraphicsRootSignature(rootSignature_.Get());
        commands_->SetGraphicsRootConstantBufferView(3, frameConstants_->GetGPUVirtualAddress());
    }
    transitionResource(commands_.Get(), shadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return stats;
}

D3D12Renderer::PassStats D3D12Renderer::drawCascadeShadowMaps(const ViewerScene& scene,
                                                           std::array<PassStats, sunCascadeSplits.size()>& slices)
{
    transitionResource(commands_.Get(), cascadeShadowMap_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, shadowMapSize, shadowMapSize, 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, shadowMapSize, shadowMapSize};
    commands_->RSSetViewports(1, &viewport);
    commands_->RSSetScissorRects(1, &scissor);
    PassStats total;
    const bool separateCasters = scene.usesCascadeCasterLists();
    for (std::size_t cascade = 0; cascade < cascadeDepthTargets_.size(); ++cascade)
    {
        const auto target = cascadeDepthTargets_[cascade];
        commands_->OMSetRenderTargets(0, nullptr, FALSE, &target);
        commands_->ClearDepthStencilView(target, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commands_->SetGraphicsRootConstantBufferView(3, frameConstants_->GetGPUVirtualAddress() +
                                                        (cascade + 1) * frameConstantStride);
        const auto pass = drawMeshes(scene, true, false, separateCasters ? &cascadeCasterDraws_[cascade] : nullptr,
                                     cascade);
        slices[cascade] = pass;
        total.draws += pass.draws;
        total.foliageDraws += pass.foliageDraws;
        total.foliageCalls += pass.foliageCalls;
        total.triangles += pass.triangles;
        total.foliageTriangles += pass.foliageTriangles;
        total.proxyFoliageTriangles += pass.proxyFoliageTriangles;
        total.sprayWoodTriangles += pass.sprayWoodTriangles;
        for (std::size_t level = 0; level < fixedLodLevelCount; ++level)
        {
            total.sprayWoodByLod[level] += pass.sprayWoodByLod[level];
            total.sprayLeavesByLod[level] += pass.sprayLeavesByLod[level];
        }
        total.sprayPlacements += pass.sprayPlacements;
        total.visibleSprayPlacements += pass.visibleSprayPlacements;
        total.sprayGroupTests += pass.sprayGroupTests;
        total.sprayPlacementTests += pass.sprayPlacementTests;
        for (std::size_t part = 0; part < total.sprayLocalPlacements.size(); ++part)
        {
            total.sprayLocalPlacements[part] += pass.sprayLocalPlacements[part];
            total.sprayLocalGroups[part] += pass.sprayLocalGroups[part];
            total.sprayIntermediateGroups[part] += pass.sprayIntermediateGroups[part];
        }
        total.boughProxyTriangles += pass.boughProxyTriangles;
        total.boughProxyPlacements += pass.boughProxyPlacements;
        total.treeNodeTests += pass.treeNodeTests;
        total.treeNodesDrawn += pass.treeNodesDrawn;
        total.treeTrunkTriangles += pass.treeTrunkTriangles;
        total.treeDraws += pass.treeDraws;
        total.treeSubmissionCalls += pass.treeSubmissionCalls;
        total.treeSubmissionMilliseconds += pass.treeSubmissionMilliseconds;
        total.treeCutHash = (total.treeCutHash ^ pass.treeCutHash) * 1099511628211ull;
        total.gpuTreeDispatches += pass.gpuTreeDispatches;
        total.treeDepthRejects += pass.treeDepthRejects;
        for (std::size_t part = 0; part < total.treeProxySprays.size(); ++part)
        {
            total.treeProxySprays[part] += pass.treeProxySprays[part];
            total.treeCanopyTriangles[part] += pass.treeCanopyTriangles[part];
        }
        total.visibleInstances = pass.visibleInstances;
        total.shadowLodOverrides = pass.shadowLodOverrides;
    }
    if (separateCasters)
    {
        total.visibleInstances = static_cast<unsigned>(std::ranges::count_if(cascadeInstanceFlags_,
            [](auto flags) { return flags & cascadeCasterVisible; }));
        total.shadowLodOverrides = static_cast<unsigned>(std::ranges::count_if(cascadeInstanceFlags_,
            [](auto flags) { return flags & cascadeCasterOverride; }));
    }
    commands_->SetGraphicsRootConstantBufferView(3, frameConstants_->GetGPUVirtualAddress());
    transitionResource(commands_.Get(), cascadeShadowMap_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return total;
}

void D3D12Renderer::uploadTreeHierarchy(const ViewerScene& scene, const std::vector<GpuMesh>& meshes)
{
    treeParts_.clear();
    treeParts_.resize(meshes.size());
    if (!scene.treeHierarchy) return;
    check(allocator_->Reset(), "Reset tree upload allocator");
    check(commands_->Reset(allocator_.Get(), nullptr), "Begin tree upload");
    std::vector<ComPtr<ID3D12Resource>> staging;
    const auto resident = [&](ComPtr<ID3D12Resource> upload, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        const auto description = upload->GetDesc();
        ComPtr<ID3D12Resource> buffer;
        check(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)),
              "Allocate resident tree geometry");
        commands_->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, description.Width);
        transitionResource(commands_.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
        staging.push_back(std::move(upload));
        return buffer;
    };
    if (!liveCut_.nodes.empty())
    {
        if (scene.profileOakRaster)
        {
            // Bins have disjoint instance slots. ID zero represents an uncovered pixel.
            if (uint64_t(liveGpu_.capacity) * 3 * liveCut_.treeCount > UINT_MAX)
                throw std::runtime_error("Oak visibility IDs exceed the capture format.");
            std::vector<UINT> keys(liveGpu_.capacity);
            std::iota(keys.begin(), keys.end(), 1u);
            oakPlacementKeys_ = resident(uploadBuffer(keys.data(), keys.size() * sizeof(UINT)),
                                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        for (auto& input : liveGpu_.inputs)
            if (input) input = resident(std::move(input),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto& mesh = liveCut_.mesh;
        mesh.vertexBuffer = resident(mesh.vertexBuffer,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        const auto indexState=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_INDEX_BUFFER |
            (scene.liveCutGpu ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON));
        mesh.indexBuffer = resident(mesh.indexBuffer,indexState);
        mesh.vertices.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
        mesh.indices.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    }
    for (const auto& part : scene.treeHierarchy->parts)
    {
        auto& gpu = treeParts_[part.meshIndex];
        gpu.bake = &part;
        if (!part.geometry.indices.empty())
        {
            gpu.geometry = uploadMesh(part.geometry);
            auto& geometry = gpu.geometry;
            geometry.vertexBuffer = resident(std::move(geometry.vertexBuffer), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            geometry.indexBuffer = resident(std::move(geometry.indexBuffer), D3D12_RESOURCE_STATE_INDEX_BUFFER);
            geometry.vertices.BufferLocation = geometry.vertexBuffer->GetGPUVirtualAddress();
            geometry.indices.BufferLocation = geometry.indexBuffer->GetGPUVirtualAddress();
        }
        const auto& source = scene.asset->meshes[part.meshIndex];
        if (!source.instances) continue;
        const auto model = XMLoadFloat4x4(&source.localToWorld);
        std::vector<float> scales;
        for (const auto id : part.placementOrder.empty() ? scene.treeHierarchy->placementOrder : part.placementOrder)
        {
            const auto& instance = (*source.instances)[id];
            gpu.instances.push_back(instance);
            if (id < sprayGroupTags_.size()) gpu.tags.push_back(sprayGroupTags_[id]);
            scales.push_back(maxLinearStretch(instance.matrix() * model));
        }
        for (const auto& node : part.nodes)
        {
            gpu.maxScale.push_back(std::ranges::max(std::span(scales).subspan(node.firstPlacement, node.placementCount)));
        }
        if (!part.instancedNodes.empty())
            gpu.orderedInstances = resident(uploadBuffer(gpu.instances.data(), gpu.instances.size() * sizeof(MeshInstance)),
                                            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    check(commands_->Close(), "End tree upload");
    ID3D12CommandList* const lists[]{commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitForGpu();
}

void D3D12Renderer::loadLiveCut(const ViewerScene& scene)
{
    liveCut_ = {};
    liveGpu_ = {};
    livePath_ = scene.liveCut;
    if (livePath_.empty()) return;
    if (!scene.treeHierarchy) throw std::runtime_error("A live oak cut needs the matching tree asset.");
    auto& cut = liveCut_;
    cut.packAssembly = scene.liveCutPackAssembly;
    cut.treeCount = static_cast<UINT>(scene.objects.size());
    std::ifstream file(livePath_,std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    const auto size = std::filesystem::file_size(livePath_);
    const auto read = [&](auto* value, size_t count = 1) {
        const auto bytes = sizeof(*value)*count;
        if (bytes) file.read(reinterpret_cast<char*>(value),bytes);
    };
    const auto array = [&](auto& values, UINT limit) {
        UINT n;
        read(&n);
        if (n > limit || uint64_t(n)*sizeof(values[0]) > size-static_cast<uint64_t>(file.tellg()))
            throw std::runtime_error("Invalid live cut array.");
        values.resize(n);
        read(values.data(),n);
    };
    char signature[8];
    read(signature,8);
    read(&cut.replace);
    if (std::memcmp(signature,"OAKCUT01",8) || cut.replace>7) throw std::runtime_error("Invalid live cut header.");
    static_assert(sizeof(LiveCut::Node)==64 && sizeof(MeshInstance)==40);
    array(cut.regions,10000);
    array(cut.placements,1000000);
    array(cut.nodes,4000000);
    array(cut.geometry,200000);
    array(cut.tiers,64);
    MeshData data{};
    XMStoreFloat4x4(&data.localToWorld,XMMatrixIdentity());
    array(data.vertices,16000000);
    array(data.indices,60000000);
    if (file.rdbuf()->sgetc()!=std::char_traits<char>::eof() || cut.nodes.empty() || cut.geometry.empty() ||
        data.vertices.empty() || data.indices.empty() || std::ranges::max(data.indices)>=data.vertices.size())
        throw std::runtime_error("Invalid live cut geometry.");
    for (const auto& ranges:cut.geometry)
        for (const auto r:ranges)
            if (uint64_t(r.firstIndex)+r.indexCount>data.indices.size() || r.indexCount%3)
                throw std::runtime_error("Invalid live cut material range.");
    for (const auto& tier:cut.tiers)
        if (tier.geometry>=cut.geometry.size() || !(tier.limit>0)) throw std::runtime_error("Invalid spray tier.");
    size_t drawNodes=0, assemblyCapacity=0;
    for (size_t i=0;i<cut.nodes.size();++i)
    {
        const auto& n=cut.nodes[i];
        if (n.end<=i || n.end>cut.nodes.size() || n.kind>3 || n.geometry>=cut.geometry.size() ||
            n.instance>=cut.placements.size() || (n.kind==3 && (cut.tiers.empty() || n.instance==cut.placements.size()-1)))
            throw std::runtime_error("Invalid live cut traversal node.");
        drawNodes+=n.kind>=2;
        if (scene.liveCutPackAssembly && n.kind==2 && n.instance==cut.placements.size()-1)
            for (auto range:cut.geometry[n.geometry]) assemblyCapacity+=range.indexCount;
    }
    cut.mesh=uploadMesh(data);
    cut.mesh.meshInstanceCount=1;
    if (scene.liveCutGpu) { prepareLiveGpu(); return; }
    cut.instances=uploadBuffer(cut.placements.data(),cut.placements.size()*sizeof(MeshInstance));
    cut.packed=createUploadBuffer(std::max<size_t>(1,drawNodes)*cut.treeCount*3*sizeof(MeshInstance));
    const D3D12_RANGE noReads{0,0};
    void* mapped=nullptr;
    if (assemblyCapacity)
    {
        cut.sourceIndices=std::move(data.indices);
        cut.assemblyIndices=createUploadBuffer(assemblyCapacity*cut.treeCount*sizeof(UINT));
        check(cut.assemblyIndices->Map(0,&noReads,&mapped),"Map live assembly index stream");
        cut.mappedIndices=static_cast<UINT*>(mapped);
    }
    check(cut.packed->Map(0,&noReads,&mapped),"Map live spray instances");
    cut.mapped=static_cast<MeshInstance*>(mapped);
    cut.batches.resize(cut.treeCount);
    for (auto& tree : cut.batches) tree.bins.resize(cut.geometry.size());
}

void D3D12Renderer::selectLiveCut(const ViewerScene& scene, UINT objectIndex, PassStats& stats)
{
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();
    auto& cut=liveCut_;
    auto& tree=cut.batches[objectIndex];
    for (auto g:tree.used) tree.bins[g].clear();
    tree.used.clear();
    const auto view=XMLoadFloat4x4(&frameView_);
    const auto planes=frustumPlanes(view*XMLoadFloat4x4(&projection_));
    const auto& object=scene.objects[objectIndex];
    const auto objectView=object.placementMatrix()*view;
    const auto ax=XMVectorAbs(objectView.r[0]),ay=XMVectorAbs(objectView.r[1]),az=XMVectorAbs(objectView.r[2]);
    const auto wind=XMVector3TransformNormal(XMVectorSet(frameWind_.x,0,frameWind_.y,0),view)*(.5f*object.placement.w);
    const float focal=projection_._22*viewport_.Height*.5f*object.placement.w/scene.liveCutPixelError;
    const float nearZ=cameraNearPlane_;
    size_t insideEnd=0;
    for (size_t i=0;i<cut.nodes.size();)
    {
        const auto& n=cut.nodes[i];
        ++stats.treeNodeTests;
        if (scene.sprayFrustumCulling && i>=insideEnd)
        {
            const auto bounds=worldWindBounds(n.bounds,object,frameWind_);
            if (!intersectsFrustum(bounds,planes)) { i=n.end; continue; }
            if (n.kind<2 && insideFrustum(bounds,planes)) insideEnd=n.end;
        }
        UINT bin=n.geometry;
        if (n.kind==3)
        {
            const auto c=XMVector3TransformCoord(XMLoadFloat3(&n.bounds.center),objectView)+wind;
            const auto extent=n.bounds.halfExtent;
            const auto e=ax*extent.x+ay*extent.y+az*extent.z+XMVectorAbs(wind);
            const float z=-XMVectorGetZ(c)-XMVectorGetZ(e);
            const auto r=XMVectorAbs(c)+e;
            const float xy2=XMVectorGetX(XMVector2LengthSq(r));
            const float pixels=z>nearZ ? focal/z*std::sqrt(1+xy2/(z*z)) : FLT_MAX;
            bin=cut.tiers.back().geometry;
            for (const auto& tier:cut.tiers)
                if (pixels<=tier.limit*.9999f) { bin=tier.geometry; break; }
        }
        else
        {
            // UE GetProjectedEdgeScales, in metres, for a perspective camera.
            const auto c=XMVector3TransformCoord(XMLoadFloat4(&n.sphere),objectView);
            const float z=-XMVectorGetZ(c), radius=n.sphere.w*object.placement.w;
            const float x2=XMVectorGetX(XMVector2LengthSq(c));
            const float dist2=x2+z*z, x=std::sqrt(x2);
            const float tangent=std::sqrt(std::max(0.f,dist2-radius*radius));
            float lo=0,hi=0;
            if (z+radius>nearZ)
            {
                const float lowCos=(tangent*z-radius*x)/std::max(dist2,1e-18f);
                const float highCos=(tangent*z+radius*x)/std::max(dist2,1e-18f);
                const float h=nearZ-z;
                const float hRadius=std::sqrt(std::max(0.f,radius*radius-h*h));
                const auto clipped=[&](float side){return nearZ/std::sqrt(side*side+nearZ*nearZ);};
                lo=std::max(z-radius,nearZ)*(tangent*lowCos<nearZ ? clipped(x+hRadius) : lowCos);
                const bool spans=radius*radius-x2>=std::max(h,0.f)*std::max(h,0.f);
                hi=std::max(z+radius,nearZ)*(spans ? 1.f : tangent*highCos<nearZ ? clipped(std::max(0.f,x-hRadius)) : highCos);
            }
            if (n.kind<2)
            {
                i=(lo<=focal*n.maximum && (n.kind==0 || hi>=focal*n.minimum)) ? i+1 : n.end;
                continue;
            }
            if (lo<=n.minimum*focal && n.minimum>0) {++i;continue;}
        }
        auto& batch=tree.bins[bin];
        if (batch.empty()) tree.used.push_back(bin);
        batch.push_back(n.instance);
        ++i;
    }
    stats.treeSelectionMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}

void D3D12Renderer::drawLiveCut(const ViewerScene& scene, size_t meshIndex, UINT slot, UINT objectIndex, PassStats& stats)
{
    auto& cut=liveCut_;
    auto& proxy=cut.mesh;
    proxy.material=meshes_[meshIndex].material;
    const D3D12_VERTEX_BUFFER_VIEW tree{instanceView_.BufferLocation+(objectIndex+1)*sizeof(XMFLOAT4),sizeof(XMFLOAT4),0};
    commands_->IASetVertexBuffers(1,1,&tree);
    const size_t firstIndex=cut.indexOffset;
    const UINT identity=static_cast<UINT>(cut.placements.size()-1);
    const auto& batches=cut.batches[objectIndex];
    for (const auto g:batches.used)
    {
        const auto geometry=cut.geometry[g][slot];
        if (!geometry.indexCount) continue;
        const auto& ids=batches.bins[g];
        const auto emit=[&](D3D12_GPU_VIRTUAL_ADDRESS address, UINT count) {
            const D3D12_VERTEX_BUFFER_VIEW stream{address,count*UINT(sizeof(MeshInstance)),sizeof(MeshInstance)};
            treeInstancedProxyDraws_.push_back(treeDraw(proxy,nullptr,geometry,count,0,stream));
        };
        if (scene.liveCutUnbatched)
            for (const auto id:ids) emit(cut.instances->GetGPUVirtualAddress()+id*sizeof(MeshInstance),1);
        else
        {
            const size_t start=cut.offset;
            for (const auto id:ids)
            {
                if (cut.mappedIndices && id==identity)
                {
                    std::memcpy(cut.mappedIndices+cut.indexOffset,cut.sourceIndices.data()+geometry.firstIndex,
                                geometry.indexCount*sizeof(UINT));
                    cut.indexOffset+=geometry.indexCount;
                }
                else cut.mapped[cut.offset++]=cut.placements[id];
            }
            if (cut.offset>start)
                emit(cut.packed->GetGPUVirtualAddress()+start*sizeof(MeshInstance),static_cast<UINT>(cut.offset-start));
        }
        const auto tris=uint64_t(geometry.indexCount/3)*ids.size();
        stats.triangles+=tris;
        if (slot<2) stats.treeCanopyTriangles[slot]+=tris;
        if (!slot) stats.foliageTriangles+=tris;
        else if (slot==1) stats.sprayWoodTriangles+=tris;
        else stats.treeTrunkTriangles+=tris;
    }
    if (cut.indexOffset>firstIndex)
    {
        const UINT count=static_cast<UINT>(cut.indexOffset-firstIndex);
        auto draw=treeDraw(proxy,nullptr,{0,count},1,0,identityMeshInstanceView_);
        draw.indices={cut.assemblyIndices->GetGPUVirtualAddress()+firstIndex*sizeof(UINT),count*UINT(sizeof(UINT)),DXGI_FORMAT_R32_UINT};
        treeInstancedProxyDraws_.push_back(draw);
    }
    stats.draws+=static_cast<unsigned>(treeInstancedProxyDraws_.size());
    submitTreeDraws(scene,proxy,scene.meshShadows[meshIndex].receive,FixedLodTint::Shaded,treeInstancedProxyDraws_,stats);
}

void D3D12Renderer::selectTreeCut(const GpuTreePart& part, const ViewerScene& scene, const SceneObject& object,
                                 FXMMATRIX viewProjection, bool shadowPass, PassStats& stats)
{
    const auto selectionStart = std::chrono::steady_clock::now();
    treeSelection_.clear();
    const auto& nodes = part.bake->nodes;
    const auto planes = frustumPlanes(viewProjection);
    const auto objectToWorld = object.placementMatrix();
    const auto clip = objectToWorld * viewProjection;
    XMFLOAT4X4 matrix;
    XMStoreFloat4x4(&matrix, objectToWorld);
    const XMFLOAT2 wind{frameWind_.x * object.placement.w, frameWind_.y * object.placement.w};
    const float limit = shadowPass ? 0.75f : scene.sprayPixelLimit;
    const auto visit = [&](auto&& self, std::uint32_t index, bool inside) -> void {
        ++stats.treeNodeTests;
        const auto& node = nodes[index];
        if (!inside)
        {
            const auto box = worldWindBounds(node.bounds, object, frameWind_);
            if (!intersectsFrustum(box, planes)) return;
            inside = insideFrustum(box, planes);
        }
        const auto kind = part.orderedInstances ? part.bake->instancedNodes[index] : 0;
        bool overlapsReplacement = false;
        if (!shadowPass && part.orderedInstances)
            for (const auto& region:liveCut_.regions)
            {
                const auto end=uint64_t(node.firstPlacement)+node.placementCount;
                const auto regionEnd=uint64_t(region[0])+region[1];
                if (node.firstPlacement>=region[0] && end<=regionEnd) return;
                overlapsReplacement |= node.firstPlacement<regionEnd && region[0]<end;
            }
        const bool boughGroup = node.firstPlacement == boughFirst_ && node.placementCount == boughCount_;
        if (kind == 3)
        {
            bool mixed = false;
            if (mixedBoughVoxels_ && boughGroup && !shadowPass &&
                projectLodError(node.error,node.bounds,matrix,frameView_,wind) <= scene.sprayTemplatePixelLimit * 0.9999f)
                for (UINT slot = 0; slot < boughVoxelLevels_.size(); ++slot)
                    if ((boughReplace_ & (1u << slot)) && part.bake->meshIndex == scene.sprayLods[slot]->meshIndex)
                    {
                        mixed = true;
                        selectedBoughTrees_[slot].add(static_cast<UINT>(&object-scene.objects.data()));
                    }
            self(self, node.firstChild + (mixed ? 0 : 1), inside);
            return;
        }
        if (kind && !mixedBoughVoxels_ && !shadowPass && boughGroup)
            for (UINT slot = 0; slot < 2; ++slot)
                if ((boughReplace_ & (1u << slot)) && part.bake->meshIndex == boughVoxelLevels_[slot].meshIndex)
                {
                    selectedBoughTrees_[slot].add(static_cast<UINT>(&object-scene.objects.data()));
                    return;
                }
        if (kind == 2)
        {
            const bool admit = !shadowPass && projectLodError(node.error,node.bounds,matrix,frameView_,wind)
                <= scene.sprayTemplatePixelLimit * 0.9999f;
            self(self,node.firstChild + (admit ? 0 : 1),inside);
            return;
        }
        const float nodeLimit = !shadowPass && kind == 1
            ? scene.sprayTemplatePixelLimit : limit;
        const bool useProxy = !overlapsReplacement && node.geometry.indexCount &&
            (shadowPass ? projectShadowTexels(node.error, clip)
                        : projectLodError(node.error, node.bounds, matrix, frameView_, wind)) <= nodeLimit * 0.9999f;
        if (useProxy || !node.childCount)
        {
            treeSelection_.push_back({index, useProxy, inside});
            return;
        }
        for (auto child = node.firstChild; child < node.firstChild + node.childCount; ++child)
            self(self, child, inside);
    };
    visit(visit, 0, !scene.sprayFrustumCulling);
    stats.treeSelectionMilliseconds += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-selectionStart).count();
    for (const auto& selected : treeSelection_)
    {
        for (const auto value : {object.id, part.bake->meshIndex, selected.node, UINT(selected.proxy)})
            stats.treeCutHash = (stats.treeCutHash ^ value) * 1099511628211ull;
    }
}

D3D12Renderer::TreeDraw D3D12Renderer::treeDraw(const GpuMesh& mesh, const GpuFixedLodLevel* lod,
                                               FixedLodIndexRange range, UINT count, UINT firstInstance,
                                               D3D12_VERTEX_BUFFER_VIEW instances, UINT level, UINT firstTag,
                                               D3D12_GPU_VIRTUAL_ADDRESS tags) const
{
    const auto& vertices = lod && lod->proxyVertices.BufferLocation ? lod->proxyVertices : mesh.vertices;
    const auto indexCount = range.indexCount ? range.indexCount : lod ? lod->indexCount : mesh.indexCount;
    return {vertices, instances, lod ? lod->indices : mesh.indices, firstTag, level,
            tags ? tags : frameConstants_->GetGPUVirtualAddress(), {indexCount, count, range.firstIndex, 0, firstInstance}};
}

void D3D12Renderer::submitTreeDraws(const ViewerScene& scene, const GpuMesh& mesh, bool receiveShadows,
                                   FixedLodTint tint, std::vector<TreeDraw>& draws, PassStats& stats)
{
    if (draws.empty()) return;
    const auto count = static_cast<UINT>(draws.size());
    for (const auto& draw : draws)
    {
        const auto& args = draw.indexed;
        for (const auto value : {args.IndexCountPerInstance, args.InstanceCount, args.StartIndexLocation,
                                 args.StartInstanceLocation, draw.firstTag, draw.lodLevel})
            stats.treeCutHash = (stats.treeCutHash ^ value) * 1099511628211ull;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    bindMesh(mesh, scene.asset->materials[mesh.material], scene.foliage, receiveShadows, nullptr, tint, 0, 0, 0);
    if (scene.treeSubmission != TreeSubmission::Direct)
    {
        const auto end = treeArgumentOffset_ + draws.size();
        if (end * sizeof(TreeDraw) > treeArguments_->GetDesc().Width)
            throw std::runtime_error("Tree command buffer capacity exceeded.");
        std::memcpy(mappedTreeArguments_ + treeArgumentOffset_, draws.data(), draws.size() * sizeof(TreeDraw));
        commands_->ExecuteIndirect(treeCommandSignature_.Get(), count, treeArguments_.Get(),
                                   treeArgumentOffset_ * sizeof(TreeDraw), nullptr, 0);
        treeArgumentOffset_ = end;
    }
    else
    {
        for (const auto& draw : draws)
        {
            commands_->IASetVertexBuffers(0, 1, &draw.vertices);
            commands_->IASetVertexBuffers(2, 1, &draw.instances);
            commands_->IASetIndexBuffer(&draw.indices);
            const std::array tags{draw.firstTag, draw.lodLevel};
            commands_->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(tags.size()), tags.data(),
                                                      offsetof(DrawConstants, firstLodTag) / sizeof(UINT));
            commands_->SetGraphicsRootShaderResourceView(4, draw.tags);
            const auto& args = draw.indexed;
            commands_->DrawIndexedInstanced(args.IndexCountPerInstance, args.InstanceCount, args.StartIndexLocation,
                                            args.BaseVertexLocation, args.StartInstanceLocation);
        }
    }
    stats.treeSubmissionMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    stats.treeDraws += count;
    stats.treeSubmissionCalls += scene.treeSubmission != TreeSubmission::Direct ? 1 : count;
    draws.clear();
}

void D3D12Renderer::drawInstancedMesh(const ViewerScene& scene, std::size_t meshIndex, const PassVisibility& visibility,
                                     bool shadowPass, std::optional<std::size_t> shadowSlice, PassStats& stats)
{
    const auto& mesh = meshes_[meshIndex];
    const auto& material = scene.asset->materials[mesh.material];
    const auto* lod = mesh.sprayLod.get();
    const bool selectLod = lod && scene.branchLod != sourceBranchLod;
    const bool cull = lod && scene.sprayFrustumCulling;
    const bool receive = scene.meshShadows[meshIndex].receive;
    const bool spray = scene.kind == SceneKind::ForkedOak;
    const bool leaves = material.hasBacklighting;
    const auto tint = lod && !shadowPass ? scene.lodTint : FixedLodTint::Shaded;
    const bool groupTint = tint == FixedLodTint::SprayGroup;
    const auto* treePart = scene.sprayTraversal == SprayTraversal::Hierarchy && scene.branchLod == automaticBranchLod &&
                          treeParts_[meshIndex].bake ? &treeParts_[meshIndex] : nullptr;
    const auto view = XMLoadFloat4x4(&frameView_);
    const auto viewProjection = shadowPass
        ? XMLoadFloat4x4(shadowSlice ? &cascadeViewProjections_[*shadowSlice] : &lightViewProjection_)
        : view * XMLoadFloat4x4(&projection_);
    const auto planes = frustumPlanes(viewProjection);

    const auto record = [&](unsigned count, std::uint64_t triangles, std::size_t level = 0, bool newDraw = true) {
        stats.draws += newDraw;
        stats.triangles += triangles;
        if (leaves)
        {
            stats.foliageTriangles += triangles;
            stats.foliageDraws += newDraw;
            stats.foliageCalls += newDraw;
            if (spray)
            {
                stats.visibleSprayPlacements += count;
                stats.sprayLeavesByLod[level] += count;
            }
        }
        else if (spray)
        {
            stats.sprayWoodTriangles += triangles;
            stats.sprayWoodByLod[level] += count;
        }
    };

    for (const auto range : visibility.ranges)
    {
        for (UINT placement = range.first; placement < range.first + range.count; ++placement)
        {
            // A zero stride holds the tree transform while the second stream selects sprays.
            const D3D12_VERTEX_BUFFER_VIEW scenePlacement{
                instanceView_.BufferLocation + (placement + 1ULL) * sizeof(XMFLOAT4), sizeof(XMFLOAT4), 0};
            commands_->IASetVertexBuffers(1, 1, &scenePlacement);
            if (spray && leaves) stats.sprayPlacements += mesh.meshInstanceCount;
            if (treePart && scene.treeSubmission == TreeSubmission::GpuPerPart && treePart->gpuNodes &&
                (shadowPass || !scene.cullSprayWoodBackfaces))
            {
                drawGpuTreePart(scene, meshIndex, scene.objects[placement], viewProjection, shadowPass, shadowSlice, tint, stats);
                continue;
            }
            if (!selectLod && !cull)
            {
                const auto tags = groupTint ? spraySourceGroupTags_->GetGPUVirtualAddress() : 0;
                record(mesh.meshInstanceCount,
                       drawMesh(mesh, material, scene.foliage, receive, mesh.meshInstanceCount, 0,
                                nullptr, tint, 0, 0, {}, tags));
                continue;
            }

            const auto& object = scene.objects[placement];
            const auto objectToWorld = object.placementMatrix();
            const auto model = XMLoadFloat4x4(&mesh.localToWorld) * objectToWorld;
            const XMFLOAT2 wind{frameWind_.x * object.placement.w, frameWind_.y * object.placement.w};
            const auto& instances = *scene.asset->meshes[meshIndex].instances;
            const auto levelCount = lod->levels.size();
            const bool automatic = selectLod && scene.branchLod == automaticBranchLod;
            const bool localLod = automatic && scene.sprayLocalLod && !lod->cutPlans.empty() &&
                                  scene.sprayTraversal != SprayTraversal::Flat;
            std::span<const MeshInstance> groupedInstances = lod->groupedInstances;
            std::span<const LodTag> groupedTags = lod->groupedTags;
            const auto minimum = automatic && leaves ? fixedLodRimLevel : std::size_t{0};
            const auto forced = selectLod && !automatic
                ? std::min(static_cast<std::size_t>(scene.branchLod), levelCount - 1) : minimum;
            const auto limit = shadowPass ? 0.75f : scene.sprayPixelLimit;
            const auto classify = [&](const MeshInstance& instance, bool testFrustum) -> std::optional<std::size_t> {
                ++stats.sprayPlacementTests;
                const auto world = instance.matrix() * model;
                const auto clip = world * viewProjection;
                if (testFrustum)
                {
                    const auto localPlanes = windExpandedLocalPlanes(clip, planes, object.placement.w, frameWind_);
                    if (!intersectsFrustum(BoundingBox(lod->bounds.center, lod->bounds.halfExtent), localPlanes))
                        return std::nullopt;
                }
                if (automatic)
                {
                    XMFLOAT4X4 localToWorld;
                    XMStoreFloat4x4(&localToWorld, world);
                    const float scale = shadowPass ? projectShadowTexels(1.0f, clip)
                        : projectLodError(1.0f, lod->bounds, localToWorld, frameView_, wind);
                    if (std::isfinite(scale))
                    {
                        if (localLod) return lod->selectCut(limit / scale);
                        for (auto candidate = levelCount - 1; candidate > minimum; --candidate)
                            if (lod->errors[candidate] * scale <= limit) return candidate;
                    }
                }
                return localLod ? 0 : forced;
            };

            const auto drawIndexed = [&](const GpuFixedLodLevel* baked, UINT level, FixedLodIndexRange range,
                                         UINT count, std::size_t offset, bool full, bool local) {
                const auto tags = !groupTint ? 0 : full ? spraySourceGroupTags_->GetGPUVirtualAddress()
                                                       : sprayInstanceTags_->GetGPUVirtualAddress();
                const auto address = sprayInstances_->GetGPUVirtualAddress() + offset * sizeof(MeshInstance);
                const D3D12_VERTEX_BUFFER_VIEW instanceView = full ? mesh.meshInstanceView : D3D12_VERTEX_BUFFER_VIEW{
                    address, count * static_cast<UINT>(sizeof(MeshInstance)), sizeof(MeshInstance)};
                const auto indexCount = range.indexCount ? range.indexCount : baked ? baked->indexCount : mesh.indexCount;
                const auto firstTag = groupTint && !full ? static_cast<UINT>(offset) : 0;
                if (treePart)
                    treeSourceDraws_.push_back(treeDraw(mesh, baked, range, count, 0, instanceView, level, firstTag, tags));
                else
                {
                    bindMesh(mesh, material, scene.foliage, receive, baked, tint, 0, level, firstTag, tags);
                    commands_->IASetVertexBuffers(2, 1, &instanceView);
                    commands_->DrawIndexedInstanced(indexCount, count, range.firstIndex, 0, 0);
                }
                record(local ? 0 : count, static_cast<std::uint64_t>(indexCount / 3) * count, local ? 0 : level);
            };
            const auto drawBatch = [&](std::size_t level, UINT count, std::size_t offset, bool full = false) {
                drawIndexed(level ? &lod->levels[level] : nullptr, static_cast<UINT>(level), {}, count, offset, full, false);
            };
            const auto copyBatch = [&](std::span<const MeshInstance> batch, std::span<const LodTag> tags) {
                const auto offset = sprayInstanceOffset_;
                std::memcpy(mappedSprayInstances_ + offset, batch.data(), batch.size_bytes());
                if (groupTint)
                    std::memcpy(mappedSprayInstanceTags_ + offset, tags.data(), tags.size_bytes());
                sprayInstanceOffset_ += batch.size();
                return offset;
            };

            if (scene.sprayTraversal == SprayTraversal::Flat)
            {
                for (auto& batch : sprayBatches_) batch.clear();
                if (groupTint)
                    for (auto& tags : sprayTagBatches_) tags.clear();
                for (std::size_t i = 0; i < instances.size(); ++i)
                {
                    if (const auto level = classify(instances[i], cull))
                    {
                        sprayBatches_[*level].push_back(instances[i]);
                        if (groupTint) sprayTagBatches_[*level].push_back(sprayGroupTags_[i]);
                    }
                }
                for (std::size_t level = 0; level < levelCount; ++level)
                {
                    const auto& batch = sprayBatches_[level];
                    if (!batch.empty())
                        drawBatch(level, static_cast<UINT>(batch.size()), copyBatch(batch, sprayTagBatches_[level]));
                }
                continue;
            }

            XMFLOAT4X4 objectMatrix;
            XMStoreFloat4x4(&objectMatrix, objectToWorld);
            const auto objectToClip = objectToWorld * viewProjection;
            const auto depthAxis = XMMatrixTranspose(objectToWorld * view).r[2];
            const float windDepth = 0.5f * XMVectorGetZ(XMVector3TransformNormal(XMVectorSet(wind.x, 0, wind.y, 0), view));
            const float focalPixels = projection_._22 * viewport_.Height * 0.5f;
            const float shadowScale = localLod && shadowPass ? projectShadowTexels(1.0f, objectToClip) : 0.0f;
            const auto coarsest = levelCount - 1;
            for (auto& batch : sprayGroupBatches_) batch.clear();
            if (localLod)
            {
                if (sprayLocalBatches_.size() < lod->localCuts.size()) sprayLocalBatches_.resize(lod->localCuts.size());
                for (auto& batch : std::span(sprayLocalBatches_).first(lod->localCuts.size())) batch.clear();
            }
            const std::size_t sprayPart = leaves ? 0 : 1;
            const bool projectGroups = localLod && leaves && !shadowPass && scene.leafGroupBounds &&
                                       lod->leafGroups.size() == lod->localGroupCount && lod->localGroupCount;
            const auto enqueue = [&](std::size_t selected, UINT first, UINT count) {
                if (!localLod)
                {
                    sprayGroupBatches_[selected].add(first, count);
                    return;
                }
                const auto& plan = lod->cutPlans[selected];
                if (plan.localCuts.empty())
                {
                    sprayGroupBatches_[plan.wholeLevel].add(first, count);
                    return;
                }
                stats.sprayLocalPlacements[sprayPart] += count;
                if (leaves) stats.visibleSprayPlacements += count;
                for (auto cut : plan.localCuts) sprayLocalBatches_[cut].add(first, count);
            };
            const auto enqueuePlacement = [&](UINT i, bool testFrustum) {
                const auto& instance = groupedInstances[i];
                const auto selected = classify(instance, testFrustum);
                if (!selected) return;
                if (projectGroups)
                {
                    const auto& plan = lod->cutPlans[*selected];
                    std::size_t oldCount = 0;
                    for (auto cut : plan.localCuts) oldCount += lod->localCuts[cut].range.indexCount;
                    if (plan.localCuts.empty()) oldCount = lod->levels[plan.wholeLevel].indexCount;
                    XMFLOAT4X4 world;
                    XMStoreFloat4x4(&world, instance.matrix() * model);
                    std::array<std::size_t, maxSprayLodGroups> chosen;
                    std::size_t count = 0, indices = 0;
                    for (const auto& group : lod->leafGroups)
                    {
                        const float scale = projectLodError(1.0f, group.bounds, world, frameView_, wind);
                        const auto cuts = std::span(lod->localCuts).subspan(group.firstCut, group.cutCount);
                        const auto cut = std::ranges::find_if(cuts, [&](const auto& c) {
                            return c.sourceError == 0 || c.sourceError * scale <= limit;
                        });
                        if (cut == cuts.end()) break;
                        chosen[count++] = group.firstCut + (cut - cuts.begin());
                        indices += cut->range.indexCount;
                    }
                    if (count == lod->leafGroups.size() && indices < oldCount)
                    {
                        ++stats.sprayLocalPlacements[sprayPart];
                        ++stats.visibleSprayPlacements;
                        for (auto cut : std::span(chosen).first(count)) sprayLocalBatches_[cut].add(i, 1);
                        return;
                    }
                }
                enqueue(*selected, i, 1);
            };
            const auto visit = [&](auto&& self, std::size_t index, bool inside) -> void {
                ++stats.sprayGroupTests;
                const auto& group = lod->groups[index];
                if (!inside)
                {
                    const auto box = worldWindBounds(group.bounds, object, frameWind_);
                    if (!intersectsFrustum(box, planes)) return;
                    inside = insideFrustum(box, planes);
                }
                if (inside && !projectGroups)
                {
                    if (localLod)
                    {
                        // Orthographic shadows need only scale bounds. The perspective camera
                        // also needs bounds on the nearest and farthest depth of the group.
                        const float upper = group.maxScale * (shadowPass ? shadowScale :
                            projectLodError(1.0f, group.bounds, objectMatrix, frameView_, wind));
                        const auto nearCut = lod->selectCut(limit * 0.9999f / upper);
                        if (nearCut == lod->cutPlans.size() - 1)
                        {
                            enqueue(nearCut, group.first, group.count);
                            return;
                        }
                        float lower = group.minScale * shadowScale;
                        if (!shadowPass)
                        {
                            const auto center = XMVectorSetW(XMLoadFloat3(&group.bounds.center), 1.0f);
                            const auto half = XMLoadFloat3(&group.bounds.halfExtent);
                            const float farDepth = -XMVectorGetX(XMVector4Dot(center, depthAxis)) - windDepth +
                                XMVectorGetX(XMVector3Dot(XMVectorAbs(depthAxis), half)) + std::abs(windDepth);
                            if (farDepth > cameraNearPlane_)
                                lower = focalPixels * group.minScale * std::abs(object.placement.w) / farDepth;
                        }
                        if (lower > 0 && nearCut == lod->selectCut(limit * 1.0001f / lower))
                        {
                            enqueue(nearCut, group.first, group.count);
                            return;
                        }
                    }
                    else
                    {
                        const float error = group.maxScale * lod->errors[coarsest];
                        const float projected = !automatic ? 0.0f : shadowPass ? projectShadowTexels(error, objectToClip)
                            : projectLodError(error, group.bounds, objectMatrix, frameView_, wind);
                        if (projected <= limit * 0.9999f)
                        {
                            enqueue(automatic ? coarsest : forced, group.first, group.count);
                            return;
                        }
                    }
                }
                if (group.rightChild)
                {
                    self(self, index + 1, inside);
                    self(self, group.rightChild, inside);
                    return;
                }
                for (auto i = group.first; i < group.first + group.count; ++i)
                    enqueuePlacement(i, !inside);
            };
            bool useBough = false;
            if (automatic && scene.sprayTraversal == SprayTraversal::BoughProxy && scene.sprayBough)
            {
                const auto box = worldWindBounds(sprayBoughBounds_, object, frameWind_);
                const float error = scene.sprayBough->error;
                const float projected = shadowPass ? projectShadowTexels(error, objectToClip)
                    : projectLodError(error, sprayBoughBounds_, objectMatrix, frameView_, wind);
                useBough = (projected <= limit || scene.captureForceBough) && (!cull || intersectsFrustum(box, planes));
                if (useBough)
                {
                    const auto part = meshIndex == scene.sprayLods[0]->meshIndex ? 0u : 1u;
                    const auto proxyTint = tint == FixedLodTint::Shaded ? tint : FixedLodTint::BoughProxy;
                    const auto triangles = drawMesh(sprayBoughMeshes_[part], material, scene.foliage, receive, 1, 0,
                                                    nullptr, proxyTint);
                    record(0, triangles);
                    stats.boughProxyTriangles += triangles;
                    if (part == 0) stats.boughProxyPlacements += lod->groups[1].count;
                }
            }
            if (treePart)
            {
                selectTreeCut(*treePart, scene, object, viewProjection, shadowPass, stats);
                if (std::ranges::any_of(treeSelection_, &TreeSelection::proxy) || (!shadowPass && !liveCut_.regions.empty()))
                {
                    groupedInstances = treePart->instances;
                    groupedTags = treePart->tags;
                    for (const auto& selected : treeSelection_)
                    {
                        const auto& node = treePart->bake->nodes[selected.node];
                        if (selected.proxy)
                        {
                            const bool repeated = treePart->orderedInstances && treePart->bake->instancedNodes[selected.node];
                            const UINT count = repeated ? node.placementCount : 1u;
                            auto stream = identityMeshInstanceView_;
                            if (repeated)
                                stream = {treePart->orderedInstances->GetGPUVirtualAddress() + node.firstPlacement * sizeof(MeshInstance),
                                          count * static_cast<UINT>(sizeof(MeshInstance)), sizeof(MeshInstance)};
                            auto& draws = repeated ? treeInstancedProxyDraws_ : treeProxyDraws_;
                            const auto adjacent = repeated && !draws.empty() &&
                                draws.back().indexed.StartIndexLocation == node.geometry.firstIndex &&
                                draws.back().indexed.IndexCountPerInstance == node.geometry.indexCount &&
                                draws.back().instances.BufferLocation + draws.back().instances.SizeInBytes == stream.BufferLocation;
                            if (adjacent)
                            {
                                draws.back().instances.SizeInBytes += stream.SizeInBytes;
                                draws.back().indexed.InstanceCount += count;
                            }
                            else draws.push_back(treeDraw(treePart->geometry, nullptr, node.geometry, count, 0, stream));
                            const auto triangles = static_cast<std::uint64_t>(node.geometry.indexCount / 3) * count;
                            record(0, triangles, 0, !adjacent);
                            ++stats.treeNodesDrawn;
                            stats.treeCanopyTriangles[sprayPart] += triangles;
                            stats.treeProxySprays[sprayPart] += node.placementCount;
                            if (leaves) stats.visibleSprayPlacements += node.placementCount;
                            continue;
                        }
                        if (selected.fullyVisible)
                        {
                            const float error = treePart->maxScale[selected.node] *
                                (localLod ? lod->cutPlans.back().error : lod->errors[coarsest]);
                            const float projected = shadowPass ? projectShadowTexels(error, objectToClip)
                                : projectLodError(error, node.bounds, objectMatrix, frameView_, wind);
                            if (projected <= limit * 0.9999f)
                            {
                                enqueue(localLod ? lod->cutPlans.size() - 1 : coarsest,
                                        node.firstPlacement, node.placementCount);
                                continue;
                            }
                        }
                        for (auto i = node.firstPlacement; i < node.firstPlacement + node.placementCount; ++i)
                            enqueuePlacement(i, !selected.fullyVisible);
                    }
                    const auto parentTint = tint == FixedLodTint::Shaded ? tint : FixedLodTint::BoughProxy;
                    submitTreeDraws(scene, treePart->geometry, receive, parentTint, treeProxyDraws_, stats);
                    submitTreeDraws(scene, mesh, receive, parentTint, treeInstancedProxyDraws_, stats);
                }
                else if (!treeSelection_.empty())
                {
                    visit(visit, 0, !cull);
                }
            }
            else
            {
                // The legacy bough replaces the left root child.
                visit(visit, useBough ? lod->groups.front().rightChild : 0, !cull);
            }
            const auto copySparse = [&](const PassVisibility& batch) {
                const auto offset = sprayInstanceOffset_;
                for (const auto part : batch.ranges)
                    copyBatch(std::span(groupedInstances).subspan(part.first, part.count),
                              std::span(groupedTags).subspan(part.first, part.count));
                return offset;
            };
            for (std::size_t level = 0; level < levelCount; ++level)
            {
                const auto& batch = sprayGroupBatches_[level];
                if (batch.ranges.empty()) continue;
                const bool full = batch.count == mesh.meshInstanceCount;
                drawBatch(level, batch.count, full ? 0 : copySparse(batch), full);
            }
            if (localLod)
            {
                for (std::size_t i = 0; i < lod->localCuts.size(); ++i)
                {
                    const auto& batch = sprayLocalBatches_[i];
                    if (batch.ranges.empty()) continue;
                    const auto& cut = lod->localCuts[i];
                    const bool full = batch.count == mesh.meshInstanceCount;
                    drawIndexed(&lod->localLevels[cut.fittedVertices], cut.tintLevel, cut.range, batch.count,
                                full ? 0 : copySparse(batch), full, true);
                    stats.sprayLocalGroups[sprayPart] += batch.count;
                    if (cut.tintLevel >= fixedLodLevelCount) stats.sprayIntermediateGroups[sprayPart] += batch.count;
                }
            }
            if (treePart) submitTreeDraws(scene, mesh, receive, tint, treeSourceDraws_, stats);
        }
    }
}

D3D12Renderer::PassStats D3D12Renderer::drawMeshes(const ViewerScene& scene, bool shadowPass, bool omitFoliage,
                                                const CascadeCasterDraws* cascade, std::optional<std::size_t> shadowSlice)
{
    commands_->IASetVertexBuffers(1, 1, &instanceView_);
    const auto& visibility = cascade ? cascade->instances : (shadowPass ? shadowVisibility_ : cameraVisibility_);
    const auto& lodDraws = cascade ? cascade->bakedFoliage : (shadowPass && !shareBranchDraws_ ? shadowBranchDraws_ : cameraBranchDraws_);
    const bool automaticVoxels = automaticBranchVoxels(scene.branchVoxelView);
    const bool experiment = scene.branchVoxelView != BranchVoxelView::Off && !automaticVoxels;
    const auto triangleLod = static_cast<int>(fixedLodTwoTriangleLevel);
    const int cameraLod = experiment ? (scene.branchVoxelView == BranchVoxelView::Source ? sourceBranchLod : triangleLod)
                                     : (automaticVoxels ? automaticBranchLod : scene.branchLod);
    const int shadowLod = experiment ? triangleLod : scene.branchShadowLod;
    const bool offCameraOnly = !experiment && scene.shadowLodOffCameraOnly;
    PassStats stats;
    stats.visibleInstances = visibility.count;
    if (cascade)
    {
        stats.foliageGroupsConsidered = cascade->groupsConsidered;
        stats.foliageGroupsVisible = cascade->groupsVisible;
    }
    if (!shadowPass)
    {
        stats.foliageGroupsConsidered = cameraGroupsConsidered_;
        stats.foliageGroupsVisible = cameraGroupsVisible_;
    }
    const bool treeLod = scene.sprayTraversal == SprayTraversal::Hierarchy && scene.branchLod == automaticBranchLod;
    const bool materialCull = !shadowPass && scene.cullSprayWoodBackfaces && scene.sprayLods[1];
    const bool live = !shadowPass && visibility.count && treeLod && !liveCut_.nodes.empty();
    if (!shadowPass && timestampQueries_)
        commands_->EndQuery(timestampQueries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,AssetStart);
    if (live)
    {
        if (oakRaster_ != OakRaster::Live) liveGpu_.used = true;
        else if (scene.liveCutGpu) dispatchLiveGpu(scene,stats);
        else {
            liveCut_.offset=liveCut_.indexOffset=0;
            for (const auto range : visibility.ranges)
                for (UINT id=range.first; id<range.first+range.count; ++id) selectLiveCut(scene,id,stats);
        }
    }
    if (!shadowPass && timestampQueries_)
        commands_->EndQuery(timestampQueries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,LiveCutEnd);
    auto& cameraMode = msaaEnabled_ ? multisample_ : singleSample_;
    if (materialCull && !cameraMode.oneSidedMesh.linear)
    {
        auto description = meshPipelineDescription_;
        description.VS = reloadedShaders_.cameraVertex.empty() ? D3D12_SHADER_BYTECODE{meshVS,sizeof(meshVS)} : bytecode(reloadedShaders_.cameraVertex);
        description.PS = reloadedShaders_.cameraPixel.empty() ? D3D12_SHADER_BYTECODE{meshPS,sizeof(meshPS)} : bytecode(reloadedShaders_.cameraPixel);
        description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        if (msaaEnabled_) { description.SampleDesc = msaaSamples; description.RasterizerState.MultisampleEnable = TRUE; }
        cameraMode.oneSidedMesh = createCameraPipeline(description,"Create one-sided spray camera pipelines");
    }
    const bool batchedTrees = !materialCull && treeLod && scene.treeSubmission == TreeSubmission::GpuBatched &&
                              drawBatchedTrees(scene, visibility, shadowPass, shadowSlice, stats);
    const auto woodIndex = !shadowPass && !materialCull && treeLod && scene.treeSubmission == TreeSubmission::GpuPerPart &&
                          scene.treeOcclusion && scene.sprayLods[1] &&
                          treeParts_[scene.sprayLods[1]->meshIndex].gpuNodes
        ? static_cast<std::size_t>(scene.sprayLods[1]->meshIndex) : meshes_.size();
    for (std::size_t slot = 0; slot < meshes_.size(); ++slot)
    {
        std::size_t i = slot;
        if (woodIndex < meshes_.size() && slot >= woodIndex)
        {
            i = slot + 1 < meshes_.size() ? slot + 1 : woodIndex;
            // Resolve the mask of real camera depth left by the leaves and trunk before testing wood.
            if (i == woodIndex) buildTreeDepth();
        }
        const auto& flags = scene.meshShadows[i];
        if (materialCull || (live && scene.liveCutGpu))
            commands_->SetPipelineState((materialCull && i == scene.sprayLods[1]->meshIndex ? cameraMode.oneSidedMesh : cameraMode.mesh).get(encodedCamera_));
        if (shadowPass && !flags.cast) continue;
        if (batchedTrees && treeParts_[i].bake) continue;

        const auto& mesh = meshes_[i];
        const auto& material = scene.asset->materials[mesh.material];
        if (!shadowPass && oakRaster_ != OakRaster::Live)
        {
            if (material.alphaCutoff >= 0 || !live || !treeParts_[i].bake)
                throw std::runtime_error("Oak raster replay requires a fully replaced, opaque oak.");
            if (oakRaster_ >= OakRaster::Depth)
            {
                const bool cull = cullMeshBackfaces_ || (materialCull && i == scene.sprayLods[1]->meshIndex);
                commands_->SetPipelineState(oakRasterPipelines_[int(oakRaster_) - int(OakRaster::Depth)][cull].Get());
            }
        }
        if (live && treeParts_[i].bake)
        {
            const UINT part = i == scene.sprayLods[0]->meshIndex ? 0 : i == scene.sprayLods[1]->meshIndex ? 1 : 2;
            for (const auto range : visibility.ranges)
                for (UINT id=range.first; id<range.first+range.count; ++id)
                {
                    if (scene.liveCutGpu) drawLiveGpu(scene,i,part,id,stats);
                    else drawLiveCut(scene,i,part,id,stats);
                }
            commands_->IASetVertexBuffers(1,1,&instanceView_);
            if (liveCut_.replace & (1u<<part)) continue;
        }
        if (mesh.meshInstanceCount)
        {
            drawInstancedMesh(scene, i, visibility, shadowPass, shadowSlice, stats);
            commands_->IASetVertexBuffers(1, 1, &instanceView_);
            continue;
        }
        const bool foliage = scene.fixedLod && i == scene.fixedLod->meshIndex;
        const auto record = [&](std::uint64_t triangles) {
            ++stats.draws;
            stats.triangles += triangles;
            if (foliage)
            {
                ++stats.foliageDraws;
                ++stats.foliageCalls;
                stats.foliageTriangles += triangles;
            }
        };
        if (!foliage)
        {
            const auto& part = treeParts_[i];
            if (treeLod && part.bake)
            {
                const auto viewProjection = shadowPass
                    ? XMLoadFloat4x4(shadowSlice ? &cascadeViewProjections_[*shadowSlice] : &lightViewProjection_)
                    : XMLoadFloat4x4(&frameView_) * XMLoadFloat4x4(&projection_);
                for (const auto range : visibility.ranges)
                {
                    for (auto placement = range.first; placement < range.first + range.count; ++placement)
                    {
                        if (scene.treeSubmission == TreeSubmission::GpuPerPart && part.gpuNodes)
                        {
                            const D3D12_VERTEX_BUFFER_VIEW placementView{
                                instanceView_.BufferLocation + (placement + 1ULL) * sizeof(XMFLOAT4), sizeof(XMFLOAT4), 0};
                            commands_->IASetVertexBuffers(1, 1, &placementView);
                            const auto tint = !shadowPass ? scene.lodTint : FixedLodTint::Shaded;
                            drawGpuTreePart(scene, i, scene.objects[placement], viewProjection, shadowPass, shadowSlice, tint, stats);
                            commands_->IASetVertexBuffers(1, 1, &instanceView_);
                            continue;
                        }
                        selectTreeCut(part, scene, scene.objects[placement], viewProjection, shadowPass, stats);
                        for (const auto& selected : treeSelection_)
                        {
                            const auto& node = part.bake->nodes[selected.node];
                            const auto& geometry = selected.proxy ? part.geometry : mesh;
                            auto& draws = selected.proxy ? treeProxyDraws_ : treeSourceDraws_;
                            const auto indices = selected.proxy ? node.geometry : FixedLodIndexRange{};
                            const auto draw = treeDraw(geometry, nullptr, indices, 1, placement + 1, identityMeshInstanceView_);
                            draws.push_back(draw);
                            const auto triangles = draw.indexed.IndexCountPerInstance / 3;
                            record(triangles);
                            stats.treeTrunkTriangles += triangles;
                            if (selected.proxy) ++stats.treeNodesDrawn;
                        }
                    }
                }
                const auto tint = !shadowPass && scene.lodTint != FixedLodTint::Shaded
                    ? FixedLodTint::BoughProxy : FixedLodTint::Shaded;
                submitTreeDraws(scene, part.geometry, flags.receive, tint, treeProxyDraws_, stats);
                submitTreeDraws(scene, mesh, flags.receive, FixedLodTint::Shaded, treeSourceDraws_, stats);
                continue;
            }
            for (const auto range : visibility.ranges)
            {
                record(drawMesh(mesh, material, scene.foliage, flags.receive, range.count, range.first + 1));
            }
            continue;
        }

        if (omitFoliage || (!shadowPass && branchVoxelIndex(scene.branchVoxelView))) continue;
        const auto tint = shadowPass || scene.branchVoxelView != BranchVoxelView::Off ? FixedLodTint::Shaded : scene.lodTint;
        const UINT highlightedGroup = !shadowPass && scene.branchVoxelView == BranchVoxelView::Off && scene.highlightLodGroup
            ? static_cast<UINT>(scene.inspectedLodGroup) + 1 : 0;
        const auto drawLod = [&](std::size_t level, FixedLodIndexRange range, UINT instances, UINT firstInstance) {
            const auto triangles = drawMesh(mesh, material, scene.foliage, flags.receive, instances, firstInstance,
                                            &fixedLodLevels_[level], tint, highlightedGroup, static_cast<UINT>(level), range);
            record(triangles);
            if (level == fixedLodTwoTriangleLevel) stats.proxyFoliageTriangles += triangles;
        };
        if (cascade)
        {
            for (const auto range : cascade->sourceFoliage.ranges)
                record(drawMesh(mesh, material, scene.foliage, flags.receive, range.count, range.first + 1));
        }
        else
        {
            const PassVisibility* matching = &visibility;
            if (shadowPass && shadowLod >= 0)
            {
                const auto& forced = offCameraOnly ? shadowOverrideVisibility_ : visibility;
                stats.shadowLodOverrides = forced.count;
                for (const auto range : forced.ranges)
                    drawLod(static_cast<std::size_t>(shadowLod), {}, range.count, range.first + 1);
                if (!offCameraOnly) continue;
                matching = &shadowMatchVisibility_;
            }
            if (cameraLod == sourceBranchLod)
            {
                for (const auto range : matching->ranges)
                    record(drawMesh(mesh, material, scene.foliage, flags.receive, range.count, range.first + 1));
                continue;
            }
            if (cameraLod >= 0 && (shadowPass || cameraGroupVisibility_.empty()))
            {
                for (const auto range : matching->ranges)
                    drawLod(static_cast<std::size_t>(cameraLod), {}, range.count, range.first + 1);
                continue;
            }
        }

        UINT64 argumentOffset = lodDraws.argumentOffset.value_or(0);
        for (std::size_t level = 0; level < lodDraws.byLevel.size(); ++level)
        {
            const auto& draws = lodDraws.byLevel[level];
            if (draws.empty()) continue;
            if (lodDraws.argumentOffset)
            {
                bindMesh(mesh, material, scene.foliage, flags.receive, &fixedLodLevels_[level], tint,
                         highlightedGroup, static_cast<UINT>(level), 0);
                const auto drawCount = static_cast<UINT>(draws.size());
                commands_->ExecuteIndirect(branchCommandSignature_.Get(), drawCount, branchArgumentBuffer_.Get(),
                                           argumentOffset, nullptr, 0);
                argumentOffset += draws.size() * sizeof(BranchDraw);
                stats.draws += drawCount;
                stats.foliageDraws += drawCount;
                ++stats.foliageCalls;
                stats.triangles += lodDraws.triangles[level];
                stats.foliageTriangles += lodDraws.triangles[level];
                if (level == fixedLodTwoTriangleLevel) stats.proxyFoliageTriangles += lodDraws.triangles[level];
                continue;
            }
            for (const auto& draw : draws)
            {
                const auto& args = draw.indexed;
                drawLod(level, {args.StartIndexLocation, args.IndexCountPerInstance},
                        args.InstanceCount, args.StartInstanceLocation);
            }
        }
    }
    if (treeDepthReady_)
    {
        transitionResource(commands_.Get(), treeGpuDepth_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        treeDepthReady_ = false;
    }
    if (materialCull) commands_->SetPipelineState(cameraMode.mesh.get(encodedCamera_));
    if (!shadowPass && timestampQueries_)
        commands_->EndQuery(timestampQueries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,AssetEnd);
    if (!shadowPass && oakRaster_ != OakRaster::Live)
    {
        if (scene.cameraPipelineStats)
            commands_->EndQuery(cameraPipelineQuery_.Get(),D3D12_QUERY_TYPE_PIPELINE_STATISTICS,0);
        if (validateOakRaster_)
        {
            copyFrameColor(cameraMode.depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                           D3D12_RESOURCE_STATE_DEPTH_WRITE, oakDepthReadback_);
            if (oakRaster_ == OakRaster::Visibility)
                copyFrameColor(oakIds_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_RENDER_TARGET, oakIdReadback_);
        }
        commands_->OMSetRenderTargets(1, &sceneTarget_, FALSE, &cameraMode.depthTarget);
        commands_->SetPipelineState(cameraMode.mesh.linear.Get());
    }
    if (shadowPass ? scene.groundShadows.cast : !captureGeometryMask_)
    {
        stats.triangles += drawMesh(ground_, scene.groundMaterial, false, scene.groundShadows.receive, 1, 0);
        ++stats.draws;
    }
    return stats;
}

std::uint64_t D3D12Renderer::drawMesh(const GpuMesh& mesh, const MaterialData& material, bool wind,
                             bool receiveShadows, UINT instanceCount, UINT firstInstance,
                             const GpuFixedLodLevel* lod, FixedLodTint tint, UINT highlightedGroup,
                             UINT lodLevel, FixedLodIndexRange range, D3D12_GPU_VIRTUAL_ADDRESS tags)
{
    bindMesh(mesh, material, wind, receiveShadows, lod, tint, highlightedGroup, lodLevel, range.firstIndex / 3, tags);
    const UINT indexCount = range.indexCount ? range.indexCount : (lod ? lod->indexCount : mesh.indexCount);
    commands_->DrawIndexedInstanced(indexCount, instanceCount, range.firstIndex, 0, firstInstance);
    return static_cast<std::uint64_t>(indexCount / 3) * instanceCount;
}

void D3D12Renderer::bindMesh(const GpuMesh& mesh, const MaterialData& material, bool wind, bool receiveShadows,
                            const GpuFixedLodLevel* lod, FixedLodTint tint, UINT highlightedGroup,
                            UINT lodLevel, UINT firstTag, D3D12_GPU_VIRTUAL_ADDRESS tags)
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
    constants.lodTint = static_cast<UINT>(tint);
    constants.highlightedLodGroup = highlightedGroup;
    constants.firstLodTag = firstTag;
    constants.lodLevel = lodLevel;
    constants.usesMeshInstances = mesh.meshInstanceCount != 0;
    if (boughDepth_) constants.shadowView = 9;
    else if (boughMaterials_) constants.shadowView = material.diffuseTransmission ? 10 : 11;
    else if (captureGeometryMask_)
        constants.shadowView = static_cast<UINT>(BranchAppearance::Representation) + 2u;
    else if (&mesh == diagnosticFoliage_)
        constants.shadowView = static_cast<UINT>(branchAppearance_) + 2u;
    else if (showShadowFactor_)
        constants.shadowView = &mesh == &ground_ ? 1u : 2u;
    if (material.twoSidedTransmission)
    {
        constants.twoSidedNormalSign = mesh.mirrored ? -1 : 1;
    }
    const auto textureHandle = [&](std::optional<std::size_t> textureIndex) {
        auto handle = textureHeap_->GetGPUDescriptorHandleForHeapStart();
        if (textureIndex) handle.ptr += (*textureIndex + 1) * textureDescriptorSize_;
        return handle;
    };
    std::optional<std::size_t> transmissionColor, transmissionStrength;
    if (useLeafResponse_ && material.diffuseTransmission)
    {
        const auto& leaf = *material.diffuseTransmission;
        constants.transmissionColor = leaf.color;
        constants.transmissionFlags = 1;
        if (leaf.colorTexture)
        {
            transmissionColor = leaf.colorTexture->index;
            constants.transmissionFlags |= (leaf.colorTexture->texcoord + 1) << 1;
        }
        if (leaf.strengthTexture)
        {
            transmissionStrength = leaf.strengthTexture->index;
            constants.transmissionFlags |= (leaf.strengthTexture->texcoord + 1) << 3;
        }
    }
    commands_->SetGraphicsRoot32BitConstants(0, drawConstantCount, &constants, 0);
    commands_->SetGraphicsRootDescriptorTable(1, textureHandle(material.colorTexture));
    commands_->SetGraphicsRootDescriptorTable(6, textureHandle(transmissionColor));
    commands_->SetGraphicsRootDescriptorTable(7, textureHandle(transmissionStrength));
    if (!tags)
    {
        auto* buffer = lod && lod->triangleTags && (tint != FixedLodTint::Shaded || highlightedGroup)
            ? lod->triangleTags.Get() : frameConstants_.Get();
        tags = buffer->GetGPUVirtualAddress();
    }
    commands_->SetGraphicsRootShaderResourceView(4, tags);
    const auto& vertices = lod && lod->proxyVertices.BufferLocation ? lod->proxyVertices : mesh.vertices;
    commands_->IASetVertexBuffers(0, 1, &vertices);
    commands_->IASetVertexBuffers(2, 1, mesh.meshInstanceCount ? &mesh.meshInstanceView : &identityMeshInstanceView_);
    commands_->IASetIndexBuffer(lod ? &lod->indices : &mesh.indices);
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
    queries.Count = TimestampCount;
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
    buffer.Width = TimestampCount * sizeof(UINT64) + sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
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

void D3D12Renderer::readGpuTimes(FrameTiming& timing) const
{
    if (!timestampReadback_) return;
    const D3D12_RANGE readRange{0, TimestampCount * sizeof(UINT64)};
    void* mapped = nullptr;
    if (FAILED(timestampReadback_->Map(0, &readRange, &mapped))) return;
    const auto* ticks = static_cast<const UINT64*>(mapped);
    const auto elapsed = [&](FrameTimestamp start, FrameTimestamp end) {
        return 1000.0 * static_cast<double>(ticks[end] - ticks[start]) / static_cast<double>(timestampFrequency_);
    };
    timing.gpuMilliseconds = elapsed(FrameStart, FrameEnd);
    timing.gpuProbeMilliseconds = elapsed(FrameStart, ProbeEnd);
    timing.gpuShadowMilliseconds = elapsed(ShadowStart, ShadowEnd);
    timing.gpuCameraMilliseconds = elapsed(ShadowEnd, CameraEnd);
    timing.gpuAssetMilliseconds = elapsed(AssetStart, AssetEnd);
    if (timing.camera.voxelBricks && !boughVoxelPath_.empty())
        timing.gpuBoughBrickMilliseconds = elapsed(BoughBrickStart, BoughBrickEnd);
    timing.gpuLiveCutMilliseconds = elapsed(AssetStart, LiveCutEnd);
    if (timing.camera.treeOcclusionActive)
        timing.camera.treePyramidGpuMilliseconds += elapsed(TreeDepthStart, TreeDepthEnd);
    const D3D12_RANGE noWrites{0, 0};
    timestampReadback_->Unmap(0, &noWrites);
}

void D3D12Renderer::createDisplayPipeline()
{
    D3D12_DESCRIPTOR_RANGE sceneTexture{};
    sceneTexture.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sceneTexture.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &sceneTexture};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = static_cast<UINT>(std::size(parameters));
    root.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
          "Serialize display root signature");
    check(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&displayRoot_)), "Create display root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = displayRoot_.Get();
    pipeline.VS = {displayVS, sizeof(displayVS)};
    pipeline.PS = {displayPS, sizeof(displayPS)};
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.SampleDesc.Count = 1;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = colorFormat;
    displayPipeline_ = createPipeline(device_.Get(), pipeline, "Create display pipeline");
}

void D3D12Renderer::drawDisplay(float exposureEv)
{
    const float exposureScale = std::exp2(exposureEv);
    commands_->SetGraphicsRootSignature(displayRoot_.Get());
    commands_->SetPipelineState(displayPipeline_.Get());
    commands_->SetGraphicsRootDescriptorTable(0, sceneColorView_);
    commands_->SetGraphicsRoot32BitConstant(1, std::bit_cast<UINT>(exposureScale), 0);
    commands_->DrawInstanced(3, 1, 0, 0);
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
