#include "D3D12Renderer.h"
#include "MeshletPacking.h"
#include "LiveCutClearCS.h"
#include "LiveCutTraverseCS.h"
#include "LiveCutEncodeCS.h"
#include "LiveCutPackCS.h"
#include "TreeBatchClipCS.h"
#include "TreeBatchCullHistoryCS.h"
#include "TreeBatchDepthCS.h"
#include "TreeBatchEncodeCS.h"
#include "TreeBatchPrepareOcclusionCS.h"
#include "TreeBatchRetestOcclusionCS.h"
#include "TreeBatchScatterCS.h"
#include "TreeBatchSelectCS.h"
#include "TreeBatchTraverseCS.h"
#include "TreeDepthCS.h"
#include "TreeSelectCS.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <DirectXCollision.h>
#include <format>
#include <limits>
#include <stdexcept>

using namespace DirectX;

namespace
{
struct Selection
{
    XMFLOAT4X4 treeToWorld, worldToClip, treeToView, treeToClip;
    XMFLOAT4 wind, viewWind, sprayCenter, sprayExtent, projection;
    XMUINT4 sizes, options;
};

struct Result
{
    UINT parents, fallbackDraws, triangles, proxyPlacements, terminals, visiblePlacements;
    UINT nodesTested, occluded, proxyTriangles, hash;
};

static_assert(sizeof(Selection) == 368 && sizeof(Result) == 40);
constexpr std::size_t selectionStride = (sizeof(Selection) + 255) & ~std::size_t{255};
constexpr UINT maxNodes = 2048, maxBuckets = 1024;

void checkTree(HRESULT result, const char* operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::format("{} failed (0x{:08X}).", operation, static_cast<unsigned long>(result)));
}

Microsoft::WRL::ComPtr<ID3D12Resource> buffer(ID3D12Device* device, std::size_t bytes, D3D12_HEAP_TYPE heapType)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const bool uav = heapType == D3D12_HEAP_TYPE_DEFAULT;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    checkTree(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&result)), "Allocate GPU tree buffer");
    return result;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* buffer, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    return barrier;
}
}

void D3D12Renderer::createTreeGpuPipelines()
{
    const auto create = [&](std::span<const D3D12_ROOT_PARAMETER> params, D3D12_SHADER_BYTECODE shader,
                            ComPtr<ID3D12RootSignature>& root, ComPtr<ID3D12PipelineState>& pipeline) {
        if (!root)
        {
            const D3D12_ROOT_SIGNATURE_DESC layout{static_cast<UINT>(params.size()), params.data(), 0, nullptr,
                                                   D3D12_ROOT_SIGNATURE_FLAG_NONE};
            ComPtr<ID3DBlob> serialized;
            checkTree(D3D12SerializeRootSignature(&layout, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr),
                      "Serialize tree compute layout");
            checkTree(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(&root)), "Create tree compute layout");
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.CS = shader;
        checkTree(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)), "Create tree compute pipeline");
    };
    std::array<D3D12_ROOT_PARAMETER,14> live{};
    live[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    live[0].Constants={0,0,9};
    live[11].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    live[11].Descriptor.ShaderRegister=5;
    for (UINT i=12;i<14;++i) {
        live[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        live[i].Descriptor.ShaderRegister=i-7;
    }
    for (UINT i=0;i<5;++i)
    {
        live[1+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
        live[6+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        live[1+i].Descriptor.ShaderRegister=live[6+i].Descriptor.ShaderRegister=i;
    }
    constexpr D3D12_SHADER_BYTECODE liveStages[]{{liveCutClearCS,sizeof(liveCutClearCS)},
        {liveCutTraverseCS,sizeof(liveCutTraverseCS)}, {liveCutEncodeCS,sizeof(liveCutEncodeCS)},{liveCutPackCS,sizeof(liveCutPackCS)}};
    for (size_t i=0;i<liveGpuPipelines_.size();++i) create(live,liveStages[i],liveGpuRoot_,liveGpuPipelines_[i]);
    const D3D12_INDIRECT_ARGUMENT_DESC indexed{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
    const D3D12_COMMAND_SIGNATURE_DESC signature{sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),1,&indexed,0};
    checkTree(device_->CreateCommandSignature(&signature,nullptr,IID_PPV_ARGS(&liveGpuSignature_)), "Create live draw signature");

    std::array<D3D12_ROOT_PARAMETER, 11> selection{};
    selection[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (UINT i = 0; i < 6; ++i)
    {
        selection[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        selection[i + 1].Descriptor.ShaderRegister = i;
    }
    for (UINT i = 0; i < 4; ++i)
    {
        selection[i + 7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        selection[i + 7].Descriptor.ShaderRegister = i;
    }
    create(selection, {treeSelectCS, sizeof(treeSelectCS)}, treeGpuRoot_, treeGpuPipeline_);

    const D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    std::array<D3D12_ROOT_PARAMETER, 3> depth{};
    depth[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    depth[0].Constants.Num32BitValues = 4;
    depth[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    depth[1].DescriptorTable = {1, &range};
    depth[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    create(depth, {treeDepthCS, sizeof(treeDepthCS)}, treeDepthRoot_, treeDepthPipeline_);
    create(depth, {treeBatchDepthCS, sizeof(treeBatchDepthCS)}, treeDepthRoot_, treeBatchDepthPipeline_);

    std::array<D3D12_ROOT_PARAMETER, 18> batch{};
    batch[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (UINT i = 0; i < 9; ++i)
    {
        batch[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        batch[i + 1].Descriptor.ShaderRegister = i;
    }
    for (UINT i = 0; i < 7; ++i)
    {
        batch[i + 10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        batch[i + 10].Descriptor.ShaderRegister = i;
    }
    batch.back().ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    batch.back().Constants = {1, 0, 1};
    constexpr D3D12_SHADER_BYTECODE shaders[]{{treeBatchTraverseCS, sizeof(treeBatchTraverseCS)},
        {treeBatchSelectCS, sizeof(treeBatchSelectCS)}, {treeBatchClipCS, sizeof(treeBatchClipCS)},
        {treeBatchEncodeCS, sizeof(treeBatchEncodeCS)},
        {treeBatchScatterCS, sizeof(treeBatchScatterCS)},
        {treeBatchPrepareOcclusionCS, sizeof(treeBatchPrepareOcclusionCS)},
        {treeBatchCullHistoryCS, sizeof(treeBatchCullHistoryCS)},
        {treeBatchRetestOcclusionCS, sizeof(treeBatchRetestOcclusionCS)}};
    for (std::size_t i = 0; i < treeBatchPipelines_.size(); ++i)
        create(batch, shaders[i], treeBatchRoot_, treeBatchPipelines_[i]);
}

void D3D12Renderer::prepareLiveGpu()
{
    auto& gpu=liveGpu_;
    const auto& cut=liveCut_;
    struct Geometry { LiveCut::Ranges ranges; UINT first, assembly; };
    static_assert(sizeof(Geometry)==32 && sizeof(LiveGpu::View)==272);
    std::vector<UINT> stack, depth(cut.nodes.size()), parent(cut.nodes.size()), remap(cut.nodes.size());
    std::vector<UINT> capacities(cut.geometry.size());
    std::vector<bool> assembly(cut.geometry.size(),cut.packAssembly);
    UINT sprays=0;
    // The disk cut is preorder. Repack by depth so a dispatch only consumes completed parents.
    for (UINT i=0;i<cut.nodes.size();++i)
    {
        while (!stack.empty() && i>=cut.nodes[stack.back()].end) stack.pop_back();
        const auto& n=cut.nodes[i];
        if (!stack.empty() && n.end>cut.nodes[stack.back()].end)
            throw std::runtime_error("Overlapping live cut subtrees.");
        depth[i]=static_cast<UINT>(stack.size());
        if (depth[i]==gpu.levels.size()) gpu.levels.push_back({});
        ++gpu.levels[depth[i]][1];
        parent[i]=stack.empty()? UINT_MAX : stack.back();
        if (n.kind<2) stack.push_back(i);
        else if (n.kind==2) {
            ++capacities[n.geometry];
            if (n.instance!=cut.placements.size()-1) assembly[n.geometry]=false;
        }
        else ++sprays;
    }
    UINT end=0;
    std::vector<UINT> cursor;
    for (auto& level:gpu.levels) { level[0]=end; cursor.push_back(end); end+=level[1]; }
    std::vector<LiveCut::Node> nodes(cut.nodes.size());
    for (UINT i=0;i<nodes.size();++i)
    {
        auto& n=nodes[remap[i]=cursor[depth[i]]++];
        n=cut.nodes[i];
        n.end=parent[i]==UINT_MAX ? UINT_MAX : remap[parent[i]];
    }
    // Each spray can choose any reachable tier. Each native cluster has one fixed bin.
    std::vector<bool> sprayBin(capacities.size());
    for (auto tier:cut.tiers) sprayBin[tier.geometry]=true;
    std::vector<Geometry> geometry;
    for (UINT i=0;i<capacities.size();++i)
    {
        const bool gather=assembly[i] && capacities[i] && !sprayBin[i];
        geometry.push_back({cut.geometry[i],gpu.capacity,gather});
        if (gather) for (UINT slot=0;slot<3;++slot) gpu.packSizes[slot]+=cut.geometry[i][slot].indexCount;
        gpu.capacity+=capacities[i]+(sprayBin[i]?sprays:0);
    }
    if (!gpu.capacity || uint64_t(gpu.capacity)*sizeof(MeshInstance)>UINT_MAX)
        throw std::runtime_error("Live cut exceeds the D3D instance view limit.");
    const LiveCut::Tier emptyTier{};
    gpu.inputs[0]=uploadBuffer(nodes.data(),nodes.size()*sizeof(nodes[0]));
    gpu.inputs[1]=uploadBuffer(geometry.data(),geometry.size()*sizeof(Geometry));
    gpu.inputs[2]=cut.tiers.empty()?uploadBuffer(&emptyTier,sizeof(emptyTier)):uploadBuffer(cut.tiers.data(),cut.tiers.size()*sizeof(emptyTier));
    gpu.inputs[3]=uploadBuffer(cut.placements.data(),cut.placements.size()*sizeof(MeshInstance));
    const size_t trees=cut.treeCount;
    auto scratch=[&](size_t bytes){return buffer(device_.Get(),bytes*trees,D3D12_HEAP_TYPE_DEFAULT);};
    gpu.views=createUploadBuffer(trees*sizeof(LiveGpu::View));
    gpu.active=scratch(nodes.size()*sizeof(UINT));
    gpu.bins=scratch(geometry.size()*sizeof(UINT));
    gpu.instances=scratch(size_t(gpu.capacity)*sizeof(MeshInstance));
    gpu.arguments=scratch(3*geometry.size()*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    gpu.counts=scratch(6*sizeof(UINT));
    const uint64_t packedIndices=uint64_t(gpu.packSizes[0])+gpu.packSizes[1]+gpu.packSizes[2];
    if (packedIndices*sizeof(UINT)>UINT_MAX || packedIndices*trees>UINT_MAX)
        throw std::runtime_error("Live assembly exceeds the D3D index buffer limit.");
    gpu.packStride=static_cast<UINT>(packedIndices);
    size_t readbackBytes=trees*6*sizeof(UINT);
    if (gpu.packStride) {
        gpu.packedIndices=scratch(gpu.packStride*sizeof(UINT));
        gpu.packedDraws=scratch(3*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        readbackBytes+=trees*3*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    }
    gpu.readback=buffer(device_.Get(),readbackBytes,D3D12_HEAP_TYPE_READBACK);
}

void D3D12Renderer::dispatchLiveGpu(const ViewerScene& scene, PassStats& stats)
{
    auto& gpu=liveGpu_;
    const auto& cut=liveCut_;
    const UINT trees=cut.treeCount, bins=static_cast<UINT>(cut.geometry.size());
    const auto view=XMLoadFloat4x4(&frameView_);
    const auto cols=XMMatrixTranspose(view*XMLoadFloat4x4(&projection_));
    const auto w=cols.r[3];
    const XMVECTOR planes[]={w+cols.r[0],w-cols.r[0],w+cols.r[1],w-cols.r[1],cols.r[2],w-cols.r[2]};
    const D3D12_RANGE noReads{};
    void* data;
    checkTree(gpu.views->Map(0,&noReads,&data),"Map live camera inputs");
    for (UINT i=0;i<trees;++i)
    {
        LiveGpu::View v{};
        const auto& obj=scene.objects[i];
        const auto world=obj.placementMatrix();
        XMStoreFloat4x4(&v.world,world);
        XMStoreFloat4x4(&v.view,world*view);
        for (UINT p=0;p<6;++p) XMStoreFloat4(&v.planes[p],planes[p]);
        v.scale=obj.placement.w;
        const auto wind=XMVectorSet(frameWind_.x,0,frameWind_.y,0)*(.5f*v.scale);
        XMStoreFloat4(&v.windFocal,wind);
        XMStoreFloat4(&v.viewWindNear,XMVector3TransformNormal(wind,view));
        v.windFocal.w=projection_._22*viewport_.Height*.5f*v.scale/scene.liveCutPixelError;
        v.viewWindNear.w=cameraNearPlane_;
        v.visible=cameraVisibility_.contains(i);
        v.frustum=scene.sprayFrustumCulling;
        static_cast<LiveGpu::View*>(data)[i]=v;
    }
    gpu.views->Unmap(0,nullptr);
    constexpr auto uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto countState=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT|D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto change=[&](bool output){
        D3D12_RESOURCE_BARRIER b[]{transition(gpu.instances.Get(),uav,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            transition(gpu.arguments.Get(),uav,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),transition(gpu.counts.Get(),uav,countState)};
        if (!output) for (auto& barrier:b) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        commands_->ResourceBarrier(3,b);
        if (gpu.packStride) {
            D3D12_RESOURCE_BARRIER packed[]{transition(gpu.packedIndices.Get(),uav,D3D12_RESOURCE_STATE_INDEX_BUFFER),
                                            transition(gpu.packedDraws.Get(),uav,countState)};
            if (!output) for (auto& barrier:packed) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            commands_->ResourceBarrier(2,packed);
        }
    };
    if (gpu.readable) change(false);
    // Global UAV barriers order bin clears, parent visibility and instance writes across dispatches.
    D3D12_RESOURCE_BARRIER sync{};
    sync.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    commands_->SetComputeRootSignature(liveGpuRoot_.Get());
    for (UINT i=0;i<gpu.inputs.size();++i)
        commands_->SetComputeRootShaderResourceView(1+i,gpu.inputs[i]->GetGPUVirtualAddress());
    commands_->SetComputeRootShaderResourceView(5,gpu.views->GetGPUVirtualAddress());
    ID3D12Resource* outputs[]{gpu.active.Get(),gpu.bins.Get(),gpu.instances.Get(),gpu.arguments.Get(),gpu.counts.Get()};
    for (UINT i=0;i<5;++i) commands_->SetComputeRootUnorderedAccessView(6+i,outputs[i]->GetGPUVirtualAddress());
    commands_->SetComputeRootShaderResourceView(11,cut.mesh.indexBuffer->GetGPUVirtualAddress());
    commands_->SetComputeRootUnorderedAccessView(12,(gpu.packStride?gpu.packedIndices:gpu.bins)->GetGPUVirtualAddress());
    commands_->SetComputeRootUnorderedAccessView(13,(gpu.packStride?gpu.packedDraws:gpu.arguments)->GetGPUVirtualAddress());
    const auto run=[&](UINT stage, UINT first, UINT count) {
        const UINT args[]{first,count,static_cast<UINT>(cut.nodes.size()),bins,gpu.capacity,static_cast<UINT>(cut.tiers.size()),
                          gpu.packSizes[0],gpu.packSizes[0]+gpu.packSizes[1],gpu.packStride};
        commands_->SetComputeRoot32BitConstants(0,9,args,0);
        commands_->SetPipelineState(liveGpuPipelines_[stage].Get());
        if (stage==3) commands_->Dispatch(std::min(count,65535u),trees,(count+65534)/65535);
        else commands_->Dispatch((count+63)/64,trees,1);
        ++stats.gpuTreeDispatches;
        commands_->ResourceBarrier(1,&sync);
    };
    run(0,0,std::max(6u,bins));
    for (auto level:gpu.levels) run(1,level[0],level[1]);
    run(gpu.packStride ? 3 : 2,0,bins);
    change(true);
    commands_->CopyBufferRegion(gpu.readback.Get(),0,gpu.counts.Get(),0,trees*6*sizeof(UINT));
    if (gpu.packStride) commands_->CopyBufferRegion(gpu.readback.Get(),trees*6*sizeof(UINT),gpu.packedDraws.Get(),0,
                                                     trees*3*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    gpu.used=gpu.readable=true;
}

void D3D12Renderer::drawLiveGpu(const ViewerScene& scene, size_t meshIndex, UINT slot, UINT object, PassStats& stats)
{
    auto& gpu=liveGpu_;
    auto& mesh=liveCut_.mesh;
    mesh.material=meshes_[meshIndex].material;
    const bool visibilityIds = oakRaster_ == OakRaster::Visibility;
    const UINT keyBase = visibilityIds ? (object * 3 + slot) * gpu.capacity : 0;
    bindMesh(mesh,scene.asset->materials[mesh.material],scene.foliage,scene.meshShadows[meshIndex].receive,
             nullptr,FixedLodTint::Shaded,0,0,keyBase);
    if (visibilityIds)
    {
        const D3D12_VERTEX_BUFFER_VIEW keys{oakPlacementKeys_->GetGPUVirtualAddress(), gpu.capacity * UINT(sizeof(UINT)), sizeof(UINT)};
        commands_->IASetVertexBuffers(3, 1, &keys);
    }
    const UINT bytes=gpu.capacity*sizeof(MeshInstance);
    const D3D12_VERTEX_BUFFER_VIEW streams[]{
        {instanceView_.BufferLocation+(object+1)*sizeof(XMFLOAT4),sizeof(XMFLOAT4),0},
        {gpu.instances->GetGPUVirtualAddress()+uint64_t(object)*bytes,bytes,sizeof(MeshInstance)}};
    commands_->IASetVertexBuffers(1,2,streams);
    const UINT bins=static_cast<UINT>(liveCut_.geometry.size());
    commands_->ExecuteIndirect(liveGpuSignature_.Get(),bins,gpu.arguments.Get(),
        size_t(object*3+slot)*bins*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),gpu.counts.Get(),(object*6+slot)*sizeof(UINT));
    ++stats.treeSubmissionCalls;
    if (gpu.packSizes[slot]) {
        const UINT indexBytes=gpu.packStride*sizeof(UINT);
        const D3D12_INDEX_BUFFER_VIEW indices{gpu.packedIndices->GetGPUVirtualAddress()+uint64_t(object)*indexBytes,
                                               indexBytes,DXGI_FORMAT_R32_UINT};
        commands_->IASetIndexBuffer(&indices);
        commands_->IASetVertexBuffers(2,1,&identityMeshInstanceView_);
        commands_->ExecuteIndirect(liveGpuSignature_.Get(),1,gpu.packedDraws.Get(),
                                   size_t(object*3+slot)*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),nullptr,0);
        ++stats.treeSubmissionCalls;
    }
}

void D3D12Renderer::readLiveGpuStats(PassStats& stats)
{
    auto& gpu=liveGpu_;
    if (!gpu.used) return;
    gpu.used=false;
    const D3D12_RANGE range{0,static_cast<size_t>(gpu.readback->GetDesc().Width)};
    void* data;
    checkTree(gpu.readback->Map(0,&range,&data),"Read live draw counters");
    for (UINT i=0;i<liveCut_.treeCount;++i) {
        const auto* r=static_cast<const UINT*>(data)+6*i;
        UINT draws=r[0]+r[1]+r[2];
        if (gpu.packStride) {
            const auto* packed=reinterpret_cast<const D3D12_DRAW_INDEXED_ARGUMENTS*>(
                static_cast<const UINT*>(data)+liveCut_.treeCount*6)+i*3;
            for (UINT slot=0;slot<3;++slot) draws+=packed[slot].IndexCountPerInstance!=0;
        }
        stats.draws+=draws;
        stats.treeDraws+=draws;
        stats.triangles+=uint64_t(r[3])+r[4]+r[5];
        stats.foliageTriangles+=r[3]; stats.treeCanopyTriangles[0]+=r[3];
        stats.sprayWoodTriangles+=r[4]; stats.treeCanopyTriangles[1]+=r[4];
        stats.treeTrunkTriangles+=r[5];
    }
    const D3D12_RANGE noWrites{};
    gpu.readback->Unmap(0,&noWrites);
}

void D3D12Renderer::writeTreeDepthViews(D3D12_CPU_DESCRIPTOR_HANDLE first)
{
    const auto cpu = textureHeap_->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = textureHeap_->GetGPUDescriptorHandleForHeapStart();
    auto address = first;
    for (UINT mode = 0; mode < treeDepthViews_.size(); ++mode)
    {
        treeDepthViews_[mode].ptr = gpu.ptr + (address.ptr - cpu.ptr);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(mode == 0 ? singleSample_.depthBuffer.Get() : nullptr, &view, address);
        address.ptr += textureDescriptorSize_;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        device_->CreateShaderResourceView(mode == 1 ? multisample_.depthBuffer.Get() : nullptr, &view, address);
        address.ptr += textureDescriptorSize_;
    }
}

void D3D12Renderer::prepareTreeGpu(const ViewerScene& scene)
{
    treeBatch_ = {};
    if (!scene.treeHierarchy || std::ranges::any_of(scene.treeHierarchy->parts,
            [](const auto& part) { return !part.instancedNodes.empty(); }))
    {
        treeGpuArguments_.Reset();
        treeGpuResults_.Reset();
        treeGpuInstances_.Reset();
        treeGpuTags_.Reset();
        treeGpuDepth_.Reset();
        treeGpuConstants_.Reset();
        treeGpuReadbacks_.Reset();
        mappedTreeGpuConstants_ = nullptr;
        return;
    }
    static_assert(sizeof(TreeGpuNode) == 48 && sizeof(TreeGpuPlacement) == 120 && sizeof(TreeGpuPlan) == 12);
    static_assert(sizeof(MeshInstance) == 40);
    std::size_t maxCommands = 0;
    std::size_t maxInstances = 1;
    std::vector<TreeGpuTable> tables;
    for (std::size_t index = 0; index < treeParts_.size(); ++index)
    {
        auto& part = treeParts_[index];
        if (!part.bake) continue;
        const auto& mesh = meshes_[index];
        const auto* lod = mesh.sprayLod.get();
        part.placementCount = static_cast<UINT>(part.instances.size());
        part.bucketCount = lod ? static_cast<UINT>(lod->levels.size() + lod->localCuts.size()) : 1;
        const auto count = part.bake->nodes.size();
        if (count > maxNodes || part.bucketCount > maxBuckets ||
            (mesh.meshInstanceCount && !lod) ||
            static_cast<std::uint64_t>(mesh.indexCount / 3) * std::max(1u, part.placementCount) > UINT_MAX ||
            static_cast<std::uint64_t>(part.placementCount) * part.bucketCount * sizeof(MeshInstance) > UINT_MAX)
        {
            part.gpuNodes.Reset();
            continue;
        }
        maxCommands = std::max(maxCommands, count + part.bucketCount);
        maxInstances = std::max(maxInstances, static_cast<std::size_t>(part.placementCount) * part.bucketCount);
    }
    if (!maxCommands) return;
    treeGpuArguments_ = buffer(device_.Get(), maxCommands * sizeof(TreeDraw), D3D12_HEAP_TYPE_DEFAULT);
    treeGpuResults_ = buffer(device_.Get(), sizeof(Result), D3D12_HEAP_TYPE_DEFAULT);
    treeGpuInstances_ = buffer(device_.Get(), maxInstances * sizeof(MeshInstance), D3D12_HEAP_TYPE_DEFAULT);
    treeGpuTags_ = buffer(device_.Get(), maxInstances * sizeof(LodTag), D3D12_HEAP_TYPE_DEFAULT);
    const UINT x = (static_cast<UINT>(viewport_.Width) + 7) / 8, y = (static_cast<UINT>(viewport_.Height) + 7) / 8;
    treeGpuDepth_ = buffer(device_.Get(), (x * y + ((x + 7) / 8) * ((y + 7) / 8)) * sizeof(UINT),
                           D3D12_HEAP_TYPE_DEFAULT);
    const auto passes = std::max<std::size_t>(1, scene.objects.size() * scene.treeHierarchy->parts.size() *
                                                  (1 + sunCascadeSplits.size()));
    treeGpuConstants_ = createUploadBuffer(passes * selectionStride);
    const D3D12_RANGE noRead{0, 0};
    void* mapped = nullptr;
    checkTree(treeGpuConstants_->Map(0, &noRead, &mapped), "Map tree pass constants");
    mappedTreeGpuConstants_ = static_cast<std::byte*>(mapped);
    treeGpuReadbacks_ = buffer(device_.Get(), passes * sizeof(Result), D3D12_HEAP_TYPE_READBACK);

    for (std::size_t index = 0; index < treeParts_.size(); ++index)
    {
        auto& part = treeParts_[index];
        if (!part.bake || part.bake->nodes.size() > maxNodes || part.bucketCount > maxBuckets ||
            part.bake->nodes.size() + part.bucketCount > maxCommands) continue;
        const auto& mesh = meshes_[index];
        const auto* lod = mesh.sprayLod.get();
        if ((mesh.meshInstanceCount && !lod) ||
            static_cast<std::uint64_t>(mesh.indexCount / 3) * std::max(1u, part.placementCount) > UINT_MAX ||
            static_cast<std::size_t>(part.placementCount) * part.bucketCount > maxInstances) continue;
        const auto& source = part.bake->nodes;
        auto& table = tables.emplace_back();
        table.meshIndex = index;
        auto& [meshIndex, nodes, placements, plans, buckets, templates] = table;
        placements.resize(part.placementCount);
        templates.resize(source.size() + part.bucketCount);
        const auto model = XMLoadFloat4x4(&mesh.localToWorld);
        for (UINT i = 0; i < source.size(); ++i)
        {
            const auto& n = source[i];
            nodes.push_back({n.bounds, n.firstChild, n.childCount, n.geometry.indexCount, n.placementCount,
                             n.error, n.firstPlacement});
            if (n.geometry.indexCount)
                templates[i] = treeDraw(part.geometry, nullptr, n.geometry, 1, 0, identityMeshInstanceView_);
            if (n.childCount || !lod) continue;
            for (UINT p = n.firstPlacement; p < n.firstPlacement + n.placementCount; ++p)
            {
                auto& record = placements[p];
                record.instance = part.instances[p];
                XMStoreFloat4x4(&record.toAsset, record.instance.matrix() * model);
                record.tag = part.tags.empty() ? LodTag{} : part.tags[p];
                record.leaf = i;
            }
        }
        const auto addTemplate = [&](UINT b, const GpuFixedLodLevel* level, FixedLodIndexRange range, UINT tint) {
            const auto offset = b * part.placementCount;
            auto view = identityMeshInstanceView_;
            if (lod)
            {
                view = {treeGpuInstances_->GetGPUVirtualAddress() + offset * sizeof(MeshInstance),
                        part.placementCount * static_cast<UINT>(sizeof(MeshInstance)), sizeof(MeshInstance)};
            }
            templates[source.size() + b] = treeDraw(mesh, level, range, 1, 0, view, tint, offset,
                                                       treeGpuTags_->GetGPUVirtualAddress());
        };
        if (lod)
        {
            for (UINT i = 0; i < lod->levels.size(); ++i) addTemplate(i, i ? &lod->levels[i] : nullptr, {}, i);
            for (UINT i = 0; i < lod->localCuts.size(); ++i)
            {
                const auto& cut = lod->localCuts[i];
                addTemplate(i + static_cast<UINT>(lod->levels.size()), &lod->localLevels[cut.fittedVertices],
                            cut.range, cut.tintLevel);
            }
            for (const auto& p : lod->cutPlans)
            {
                const auto first = static_cast<UINT>(buckets.size());
                if (p.localCuts.empty()) buckets.push_back(static_cast<UINT>(p.wholeLevel));
                else
                    for (const auto cut : p.localCuts) buckets.push_back(static_cast<UINT>(lod->levels.size() + cut));
                plans.push_back({p.error, first, static_cast<UINT>(buckets.size()) - first});
            }
            part.localPlanCount = static_cast<UINT>(plans.size());
            const UINT minimum = scene.sprayLods[0] && index == scene.sprayLods[0]->meshIndex ? fixedLodRimLevel : 0;
            for (UINT i = minimum; i < lod->levels.size(); ++i)
            {
                plans.push_back({i == minimum ? 0 : lod->errors[i], static_cast<UINT>(buckets.size()), 1});
                buckets.push_back(i);
            }
            part.wholePlanCount = static_cast<UINT>(plans.size()) - part.localPlanCount;
        }
        else
        {
            addTemplate(0, nullptr, {}, 0);
        }
        part.gpuNodes = uploadBuffer(nodes.data(), nodes.size() * sizeof(TreeGpuNode));
        const TreeGpuPlacement emptyPlacement{};
        const TreeGpuPlan emptyPlan{};
        const UINT emptyBucket = 0;
        part.gpuPlacements = placements.empty() ? uploadBuffer(&emptyPlacement, sizeof(emptyPlacement))
            : uploadBuffer(placements.data(), placements.size() * sizeof(TreeGpuPlacement));
        part.gpuPlans = plans.empty() ? uploadBuffer(&emptyPlan, sizeof(emptyPlan))
            : uploadBuffer(plans.data(), plans.size() * sizeof(TreeGpuPlan));
        part.gpuBuckets = buckets.empty() ? uploadBuffer(&emptyBucket, sizeof(emptyBucket))
            : uploadBuffer(buckets.data(), buckets.size() * sizeof(UINT));
        part.gpuDraws = uploadBuffer(templates.data(), templates.size() * sizeof(TreeDraw));
    }
    prepareTreeBatch(scene, tables);
}

void D3D12Renderer::buildTreeDepth()
{
    auto& mode = msaaEnabled_ ? multisample_ : singleSample_;
    const auto depth = mode.depthBuffer.Get();
    constexpr auto shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const std::array toCompute{transition(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, shaderRead)};
    commands_->ResourceBarrier(static_cast<UINT>(toCompute.size()), toCompute.data());
    commands_->SetComputeRootSignature(treeDepthRoot_.Get());
    commands_->SetPipelineState(treeDepthPipeline_.Get());
    commands_->SetComputeRootDescriptorTable(1, treeDepthViews_[msaaEnabled_ ? 1 : 0]);
    commands_->SetComputeRootUnorderedAccessView(2, treeGpuDepth_->GetGPUVirtualAddress());
    std::array constants{static_cast<UINT>(viewport_.Width), static_cast<UINT>(viewport_.Height),
                         depth->GetDesc().SampleDesc.Count, 0u};
    const UINT x = (constants[0] + 7) / 8, y = (constants[1] + 7) / 8;
    commands_->SetComputeRoot32BitConstants(0, 4, constants.data(), 0);
    commands_->Dispatch(x, y, 1);
    D3D12_RESOURCE_BARRIER order{};
    order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    order.UAV.pResource = treeGpuDepth_.Get();
    commands_->ResourceBarrier(1, &order);
    constants[3] = 1;
    commands_->SetComputeRoot32BitConstants(0, 4, constants.data(), 0);
    commands_->Dispatch((x + 7) / 8, (y + 7) / 8, 1);
    const std::array toRaster{transition(depth, shaderRead, D3D12_RESOURCE_STATE_DEPTH_WRITE),
                              transition(treeGpuDepth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, shaderRead)};
    commands_->ResourceBarrier(static_cast<UINT>(toRaster.size()), toRaster.data());
    commands_->SetPipelineState(mode.mesh.get(encodedCamera_));
    treeDepthReady_ = true;
}

void D3D12Renderer::drawGpuTreePart(const ViewerScene& scene, std::size_t meshIndex, const SceneObject& object,
                                  FXMMATRIX viewProjection, bool shadowPass, std::optional<std::size_t> shadowSlice,
                                  FixedLodTint tint, PassStats& stats)
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto& part = treeParts_[meshIndex];
    const auto& mesh = meshes_[meshIndex];
    const auto receive = scene.meshShadows[meshIndex].receive;
    const auto ordinal = treeGpuReadbackParts_.size();
    if ((ordinal + 1) * selectionStride > treeGpuConstants_->GetDesc().Width)
        throw std::runtime_error("Tree compute pass buffer capacity exceeded.");
    treeGpuReadbackParts_.push_back({shadowPass, shadowSlice, meshIndex});
    Selection selection{};
    const auto treeToWorld = object.placementMatrix();
    const auto worldToView = XMLoadFloat4x4(&frameView_);
    XMStoreFloat4x4(&selection.treeToWorld, treeToWorld);
    XMStoreFloat4x4(&selection.worldToClip, viewProjection);
    XMStoreFloat4x4(&selection.treeToView, treeToWorld * worldToView);
    XMStoreFloat4x4(&selection.treeToClip, treeToWorld * viewProjection);
    selection.wind = {frameWind_.x * object.placement.w * 0.5f, 0, frameWind_.y * object.placement.w * 0.5f, 0};
    XMStoreFloat4(&selection.viewWind, XMVector3TransformNormal(XMLoadFloat4(&selection.wind), worldToView));
    if (const auto* lod = mesh.sprayLod.get())
    {
        const auto& b = lod->bounds;
        selection.sprayCenter = {b.center.x, b.center.y, b.center.z, 0};
        selection.sprayExtent = {b.halfExtent.x, b.halfExtent.y, b.halfExtent.z, 0};
    }
    selection.projection = {shadowPass ? 0.75f : scene.sprayPixelLimit, projection_._22 * viewport_.Height * 0.5f,
                            cameraNearPlane_, static_cast<float>(shadowMap_->GetDesc().Width) * 0.5f};
    selection.sizes = {static_cast<UINT>(part.bake->nodes.size()), part.placementCount, part.bucketCount, part.localPlanCount};
    selection.options = {(scene.sprayFrustumCulling ? 1u : 0u) | (shadowPass ? 2u : 0u) |
                         (treeDepthReady_ && !shadowPass ? 4u : 0u) |
                         (scene.sprayLocalLod && part.localPlanCount ? 8u : 0u) |
                         (tint == FixedLodTint::SprayGroup ? 16u : 0u), part.wholePlanCount,
                         static_cast<UINT>(viewport_.Width), static_cast<UINT>(viewport_.Height)};
    std::memcpy(mappedTreeGpuConstants_ + ordinal * selectionStride, &selection, sizeof(selection));

    commands_->SetComputeRootSignature(treeGpuRoot_.Get());
    commands_->SetPipelineState(treeGpuPipeline_.Get());
    commands_->SetComputeRootConstantBufferView(0, treeGpuConstants_->GetGPUVirtualAddress() + ordinal * selectionStride);
    const std::array inputs{part.gpuNodes.Get(), part.gpuPlacements.Get(), part.gpuPlans.Get(), part.gpuDraws.Get(),
                            part.gpuBuckets.Get(), treeDepthReady_ ? treeGpuDepth_.Get() : part.gpuBuckets.Get()};
    const std::array outputs{treeGpuArguments_.Get(), treeGpuResults_.Get(), treeGpuInstances_.Get(), treeGpuTags_.Get()};
    for (UINT i = 0; i < inputs.size(); ++i) commands_->SetComputeRootShaderResourceView(i + 1, inputs[i]->GetGPUVirtualAddress());
    for (UINT i = 0; i < outputs.size(); ++i) commands_->SetComputeRootUnorderedAccessView(i + 7, outputs[i]->GetGPUVirtualAddress());
    commands_->Dispatch(1, 1, 1);
    constexpr std::array states{D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    std::array<D3D12_RESOURCE_BARRIER, outputs.size()> barriers;
    for (std::size_t i = 0; i < outputs.size(); ++i)
        barriers[i] = transition(outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, states[i]);
    commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    const auto& mode = msaaEnabled_ ? multisample_ : singleSample_;
    commands_->SetPipelineState(shadowPass ? shadowPipeline_.Get() : mode.mesh.get(encodedCamera_));
    const auto execute = [&](const GpuMesh& m, FixedLodTint color, UINT cap, UINT64 offset, UINT64 countOffset) {
        bindMesh(m, scene.asset->materials[m.material], scene.foliage, receive, nullptr, color, 0, 0, 0);
        commands_->ExecuteIndirect(treeCommandSignature_.Get(), cap, treeGpuArguments_.Get(), offset,
                                   treeGpuResults_.Get(), countOffset);
        ++stats.treeSubmissionCalls;
    };
    if (part.geometry.indexCount)
        execute(part.geometry, tint == FixedLodTint::Shaded ? tint : FixedLodTint::BoughProxy, selection.sizes.x, 0, 0);
    execute(mesh, tint, part.bucketCount, selection.sizes.x * sizeof(TreeDraw), sizeof(UINT));
    commands_->CopyBufferRegion(treeGpuReadbacks_.Get(), ordinal * sizeof(Result), treeGpuResults_.Get(), 0, sizeof(Result));
    for (std::size_t i = 0; i < outputs.size(); ++i)
        barriers[i] = transition(outputs[i], states[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    ++stats.gpuTreeDispatches;
    stats.treeSubmissionMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void D3D12Renderer::readTreeGpuStats(const ViewerScene& scene, PassStats& camera, PassStats& shadow,
                                    std::array<PassStats, sunCascadeSplits.size()>& cascades)
{
    if (treeGpuReadbackParts_.empty()) return;
    D3D12_RANGE range{0, treeGpuReadbackParts_.size() * sizeof(Result)};
    void* mapped = nullptr;
    checkTree(treeGpuReadbacks_->Map(0, &range, &mapped), "Read tree compute counts");
    const auto* results = static_cast<const Result*>(mapped);
    for (std::size_t i = 0; i < treeGpuReadbackParts_.size(); ++i)
    {
        const auto& item = treeGpuReadbackParts_[i];
        const auto& result = results[i];
        const auto apply = [&](PassStats& stats) {
            const auto draws = result.parents + result.fallbackDraws;
            stats.draws += draws;
            stats.triangles += result.triangles;
            stats.treeDraws += draws;
            stats.treeNodesDrawn += result.parents;
            stats.treeNodeTests += result.nodesTested;
            stats.treeDepthRejects += result.occluded;
            stats.treeCutHash = (stats.treeCutHash ^ result.hash) * 1099511628211ull;
            if (scene.sprayLods[0] && item.part == scene.sprayLods[0]->meshIndex)
            {
                stats.foliageTriangles += result.triangles;
                stats.foliageDraws += draws;
                stats.visibleSprayPlacements += result.visiblePlacements + result.proxyPlacements;
                stats.treeProxySprays[0] += result.proxyPlacements;
                stats.treeCanopyTriangles[0] += result.proxyTriangles;
            }
            else if (scene.sprayLods[1] && item.part == scene.sprayLods[1]->meshIndex)
            {
                stats.sprayWoodTriangles += result.triangles;
                stats.treeProxySprays[1] += result.proxyPlacements;
                stats.treeCanopyTriangles[1] += result.proxyTriangles;
            }
            else
            {
                stats.treeTrunkTriangles += result.triangles;
            }
        };
        apply(item.shadow ? shadow : camera);
        if (item.slice) apply(cascades[*item.slice]);
    }
    range.End = 0;
    treeGpuReadbacks_->Unmap(0, &range);
}

namespace
{
constexpr UINT batchViewCapacity = 1 + static_cast<UINT>(sunCascadeSplits.size());
constexpr UINT batchPassCapacity = batchViewCapacity + 1;
constexpr UINT batchViewStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr UINT batchSelectThreads = 64;
constexpr UINT batchScatterThreads = 128;
enum BatchTimestamp : UINT { BatchStart, CoarseEnd, SelectionEnd, ClipEnd, HistoryEnd, PackEnd, BatchEnd, batchTimestamps };
constexpr UINT batchStateHeader = 16;
constexpr UINT pyramidLevels = 4;
constexpr UINT noCluster = std::numeric_limits<UINT>::max();
constexpr UINT batchArenaStride = 68; // Scene placement, spray placement, and optional group tag.

struct BatchPart
{
    XMFLOAT4 center, extent;
    UINT nodeFirst, nodeCount, placementFirst, placementCount;
    UINT planFirst, localPlans, wholePlans, firstBin;
};

struct BatchPose
{
    XMFLOAT4 placement;
    XMFLOAT4X4 toWorld, toView, toClip;
    XMFLOAT4 wind, viewWind, clipWind;
};

struct BatchView
{
    XMFLOAT4X4 worldToClip;
    XMFLOAT4 projection;
    XMUINT4 sizes, options, instanceAddress;
    XMFLOAT4X4 previousClip;
    XMFLOAT4 previousWind;
    XMUINT4 viewport;
};

struct BatchCluster
{
    Bounds bounds;
    UINT next, bin;
};

static_assert(sizeof(BatchPart) == 64 && sizeof(BatchPose) == 256 && sizeof(BatchView) == 224);
static_assert(sizeof(BatchCluster) == 32);

Bounds mergeClusterBounds(const Bounds& a, const Bounds& b)
{
    BoundingBox merged;
    BoundingBox::CreateMerged(merged, BoundingBox(a.center, a.halfExtent), BoundingBox(b.center, b.halfExtent));
    return {merged.Center, merged.Extents};
}

Bounds clusterBounds(std::span<const Vertex> vertices, std::span<const UINT> indices, FXMMATRIX toAsset)
{
    auto lower = XMLoadFloat3(&vertices[indices.front()].position);
    auto upper = lower;
    for (const auto index : indices)
    {
        const auto point = XMLoadFloat3(&vertices[index].position);
        lower = XMVectorMin(lower, point);
        upper = XMVectorMax(upper, point);
    }
    BoundingBox box;
    XMStoreFloat3(&box.Center, (lower + upper) * 0.5f);
    XMStoreFloat3(&box.Extents, (upper - lower) * 0.5f);
    BoundingBox transformed;
    box.Transform(transformed, toAsset);
    return {transformed.Center, transformed.Extents};
}

bool treeCrossesFrustum(const Bounds& bounds, FXMMATRIX toClip, FXMVECTOR clipWind)
{
    const auto columns = XMMatrixTranspose(toClip);
    const auto w = columns.r[3];
    const std::array planes{w + columns.r[0], w - columns.r[0], w + columns.r[1], w - columns.r[1],
                            columns.r[2], w - columns.r[2]};
    const float x = XMVectorGetX(clipWind), y = XMVectorGetY(clipWind);
    const float z = XMVectorGetZ(clipWind), cw = XMVectorGetW(clipWind);
    const std::array shifts{cw + x, cw - x, cw + y, cw - y, z, cw - z};
    const auto position = XMVectorSetW(XMLoadFloat3(&bounds.center), 1);
    const auto extent = XMLoadFloat3(&bounds.halfExtent);
    bool crossing = false;
    for (std::size_t i = 0; i < planes.size(); ++i)
    {
        const auto absPlane = XMVectorAbs(planes[i]);
        const float distance = XMVectorGetX(XMVector4Dot(position, planes[i]));
        const float radius = XMVectorGetX(XMVector3Dot(extent, absPlane));
        const float magnitude = XMVectorGetX(XMVector4Dot(XMVectorAbs(position), absPlane)) + radius + std::abs(shifts[i]);
        const float tolerance = 32 * std::numeric_limits<float>::epsilon() * magnitude;
        if (distance + radius + std::max(0.0f, shifts[i]) < -tolerance) return false;
        crossing |= distance - radius + std::min(0.0f, shifts[i]) < tolerance;
    }
    return crossing;
}

D3D12_RESOURCE_BARRIER orderTreeBuffer(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}
}

void D3D12Renderer::prepareTreeBatch(const ViewerScene& scene, std::span<const TreeGpuTable> tables)
{
    if (tables.empty() || tables.size() != scene.treeHierarchy->parts.size() || tables.size() > 32 ||
        scene.objects.empty() || scene.objects.size() > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION)
        return;

    // A parent replaces one or more terminal ranges. The all-terminal cut bounds both queues.
    std::size_t jobsPerTree = 0, recordsPerTree = 0;
    for (const auto& table : tables)
    {
        for (const auto& node : table.nodes)
            if (!node.childCount) jobsPerTree += (node.placements + batchSelectThreads - 1) / batchSelectThreads;
        const auto slots = table.plans.empty() ? 1u : std::ranges::max(table.plans, {}, &TreeGpuPlan::count).count;
        recordsPerTree += std::max<std::size_t>(1, table.placements.size() * slots);
    }
    const auto maxJobs = jobsPerTree * scene.objects.size();
    const auto maxRecords = recordsPerTree * scene.objects.size();
    const auto recordLimit = std::min<std::size_t>(UINT_MAX / batchArenaStride,
        D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * batchScatterThreads);
    if (maxJobs > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION ||
        maxRecords > recordLimit)
        return;
    // Clipped cuts reuse the unused tail of the view arena, plus one tree's worth of headroom.
    const auto recordCapacity = std::min(recordLimit, maxRecords + recordsPerTree);

    struct DrawTemplate
    {
        TreeBatchDraw command;
        UINT batch, firstCommand = 0, clusterRoot = noCluster;
        UINT padding = 0;
    };
    static_assert(sizeof(DrawTemplate) == 120 && offsetof(DrawTemplate, batch) == 104 &&
                  offsetof(DrawTemplate, clusterRoot) == 112);

    TreeBatchGpu gpu;
    std::vector<BatchPart> parts;
    std::vector<TreeGpuNode> nodes;
    std::vector<TreeGpuPlacement> placements;
    std::vector<TreeGpuPlan> plans;
    std::vector<UINT> planBins;
    std::vector<DrawTemplate> draws;
    std::vector<BatchCluster> clusters;
    gpu.bounds = scene.assetBounds;
    for (const auto& table : tables)
    {
        const auto meshIndex = table.meshIndex;
        const auto& part = treeParts_[meshIndex];
        const auto* lod = meshes_[meshIndex].sprayLod.get();
        const auto first = static_cast<UINT>(draws.size());
        const auto nodeCount = static_cast<UINT>(table.nodes.size());
        BatchPart info{};
        if (lod)
        {
            const auto& bounds = lod->bounds;
            info.center = {bounds.center.x, bounds.center.y, bounds.center.z, 0};
            info.extent = {bounds.halfExtent.x, bounds.halfExtent.y, bounds.halfExtent.z, 0};
        }
        info.nodeFirst = static_cast<UINT>(nodes.size());
        info.nodeCount = nodeCount;
        info.placementFirst = static_cast<UINT>(placements.size());
        info.placementCount = part.placementCount;
        info.planFirst = static_cast<UINT>(plans.size());
        info.localPlans = part.localPlanCount;
        info.wholePlans = part.wholePlanCount;
        info.firstBin = first;
        const auto partIndex = static_cast<UINT>(parts.size());
        parts.push_back(info);
        gpu.bounds = mergeClusterBounds(gpu.bounds, part.bake->nodes.front().bounds);
        gpu.meshIndices.push_back(meshIndex);
        nodes.insert(nodes.end(), table.nodes.begin(), table.nodes.end());
        placements.insert(placements.end(), table.placements.begin(), table.placements.end());
        for (auto plan : table.plans)
        {
            plan.first += static_cast<UINT>(planBins.size());
            plans.push_back(plan);
        }
        planBins.insert(planBins.end(), table.planBins.begin(), table.planBins.end());

        const auto proxyBatch = static_cast<UINT>(gpu.batches.size());
        if (part.geometry.indexCount)
            gpu.batches.push_back({meshIndex, first, nodeCount, 0, nodeCount, partIndex, true});
        const auto sourceBatch = static_cast<UINT>(gpu.batches.size());
        gpu.batches.push_back({meshIndex, first + nodeCount, part.bucketCount, 0, part.bucketCount, partIndex, false});
        for (UINT i = 0; i < table.templates.size(); ++i)
        {
            const auto& d = table.templates[i];
            const bool proxy = i < nodeCount;
            const auto batch = proxy ? proxyBatch : sourceBatch;
            draws.push_back({{d.vertices, {}, d.indices, 0, d.lodLevel, 0, {}, d.indexed}, batch});
            gpu.bins.push_back({d.indexed.IndexCountPerInstance / 3, proxy ? table.nodes[i].placements : 0, batch});
        }
    }
    gpu.baseBins = static_cast<UINT>(draws.size());
    for (UINT bin = 0; bin < gpu.baseBins; ++bin)
    {
        const auto source = draws[bin];
        const auto& draw = source.command.indexed;
        if (!draw.IndexCountPerInstance) continue;
        auto& batch = gpu.batches[source.batch];
        const auto& part = treeParts_[batch.meshIndex];
        const auto& mesh = batch.proxy ? part.bake->geometry : scene.asset->meshes[batch.meshIndex];
        const auto* lod = batch.proxy ? nullptr : meshes_[batch.meshIndex].sprayLod.get();
        const auto baked = !lod ? nullptr :
            scene.sprayLods[0] && scene.sprayLods[0]->meshIndex == batch.meshIndex ? scene.sprayLods[0].get() :
            scene.sprayLods[1].get();
        std::span<const Vertex> vertices = mesh.vertices;
        std::span<const UINT> indices = mesh.indices;
        std::span<const FixedLodIndexRange> bakedClusters;
        const auto level = bin - batch.firstBin;
        if (baked && level)
        {
            if (level < lod->levels.size())
            {
                indices = baked->levels[level].indices;
                bakedClusters = baked->levels[level].clusters;
                if (baked->usesProxyVertices(level)) vertices = baked->proxyVertices;
            }
            else
            {
                indices = baked->sprayGroupIndices;
                if (lod->localCuts[level - lod->levels.size()].fittedVertices) vertices = baked->proxyVertices;
            }
        }
        const auto toAsset = lod ? XMMatrixIdentity() : XMLoadFloat4x4(&mesh.localToWorld);
        struct Range { Bounds bounds; UINT first, count; };
        std::vector<Range> ranges;
        const auto add = [&](UINT first, UINT count) {
            ranges.push_back({clusterBounds(vertices, indices.subspan(first, count), toAsset), first, count});
        };
        if (bakedClusters.empty())
        {
            auto offset = draw.StartIndexLocation;
            const auto cut = indices.subspan(offset, draw.IndexCountPerInstance);
            for (const auto count : scanMeshletIndexCounts(cut, vertices.size(), baked ? baked->triangleCap : 128))
            {
                add(offset, count);
                offset += count;
            }
        }
        else
            for (const auto& range : bakedClusters) add(range.firstIndex, range.indexCount);

        // Group contiguous meshlets so a fully visible range can be drawn in a single packet.
        const auto append = [&](auto&& self, std::span<const Range> group, UINT templateBin) -> UINT {
            const auto root = static_cast<UINT>(clusters.size());
            draws[templateBin].clusterRoot = root;
            clusters.emplace_back();
            Bounds bounds = group.front().bounds;
            if (group.size() > 1)
            {
                const auto stride = (group.size() + 3) / 4;
                for (std::size_t first = 0; first < group.size(); first += stride)
                {
                    const auto children = group.subspan(first, std::min(stride, group.size() - first));
                    auto command = source;
                    command.command.indexed.StartIndexLocation = children.front().first;
                    command.command.indexed.IndexCountPerInstance =
                        children.back().first + children.back().count - children.front().first;
                    const auto childBin = static_cast<UINT>(draws.size());
                    draws.push_back(command);
                    gpu.bins.push_back({command.command.indexed.IndexCountPerInstance / 3, 0, source.batch});
                    ++batch.commandCapacity;
                    const auto childRoot = self(self, children, childBin);
                    bounds = mergeClusterBounds(bounds, clusters[childRoot].bounds);
                }
            }
            clusters[root] = {bounds, static_cast<UINT>(clusters.size()), templateBin};
            return root;
        };
        append(append, ranges, bin);
    }
    UINT firstCommand = 0;
    for (auto& batch : gpu.batches)
    {
        batch.firstCommand = firstCommand;
        firstCommand += batch.commandCapacity;
    }
    for (auto& draw : draws)
    {
        const auto& batch = gpu.batches[draw.batch];
        draw.batch = batch.firstBin;
        draw.firstCommand = batch.firstCommand;
    }
    if (placements.empty()) placements.emplace_back();
    if (plans.empty()) plans.emplace_back();
    if (planBins.empty()) planBins.push_back(0);
    const std::array uploads{uploadBuffer(parts.data(), parts.size() * sizeof(BatchPart)),
                             uploadBuffer(nodes.data(), nodes.size() * sizeof(TreeGpuNode)),
                             uploadBuffer(placements.data(), placements.size() * sizeof(TreeGpuPlacement)),
                             uploadBuffer(plans.data(), plans.size() * sizeof(TreeGpuPlan)),
                             uploadBuffer(planBins.data(), planBins.size() * sizeof(UINT)),
                             uploadBuffer(draws.data(), draws.size() * sizeof(DrawTemplate)),
                             uploadBuffer(clusters.data(), clusters.size() * sizeof(BatchCluster))};
    // Keep the hierarchy tables local to the GPU for the dependent cluster-bound reads.
    checkTree(allocator_->Reset(), "Reset tree table upload allocator");
    checkTree(commands_->Reset(allocator_.Get(), nullptr), "Begin tree table upload");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    for (std::size_t i = 0; i < uploads.size(); ++i)
    {
        const auto description = uploads[i]->GetDesc();
        checkTree(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu.tables[i])), "Allocate resident tree table");
        commands_->CopyBufferRegion(gpu.tables[i].Get(), 0, uploads[i].Get(), 0, description.Width);
        const auto ready = transition(gpu.tables[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commands_->ResourceBarrier(1, &ready);
    }
    checkTree(commands_->Close(), "End tree table upload");
    ID3D12CommandList* const lists[]{commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitForGpu();
    gpu.partCount = static_cast<UINT>(parts.size());
    gpu.rootCapacity = static_cast<UINT>(scene.objects.size());
    gpu.recordCapacity = static_cast<UINT>(recordCapacity);
    gpu.stateBytes = static_cast<UINT>((batchStateHeader + 4 * gpu.bins.size() + 4 * parts.size()) * sizeof(UINT));
    gpu.poseStride = gpu.rootCapacity * sizeof(BatchPose);
    const std::array resetDispatch{0u, 1u, 1u, 0u, 1u, 1u};
    std::vector<UINT> reset(gpu.stateBytes / sizeof(UINT) + resetDispatch.size());
    std::ranges::copy(resetDispatch, reset.begin() + gpu.stateBytes / sizeof(UINT));
    gpu.reset = uploadBuffer(reset.data(), reset.size() * sizeof(UINT));
    gpu.state = buffer(device_.Get(), gpu.stateBytes, D3D12_HEAP_TYPE_DEFAULT);
    gpu.dispatch = buffer(device_.Get(), sizeof(resetDispatch), D3D12_HEAP_TYPE_DEFAULT);
    gpu.jobs = buffer(device_.Get(), maxJobs * 5 * sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT);
    gpu.records = buffer(device_.Get(), recordCapacity * 4 * sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT);
    gpu.arguments = buffer(device_.Get(), firstCommand * sizeof(TreeBatchDraw), D3D12_HEAP_TYPE_DEFAULT);
    gpu.instances = buffer(device_.Get(), recordCapacity * batchArenaStride, D3D12_HEAP_TYPE_DEFAULT);
    gpu.readback = buffer(device_.Get(), static_cast<std::size_t>(gpu.stateBytes) * batchPassCapacity, D3D12_HEAP_TYPE_READBACK);
    gpu.constants = createUploadBuffer(batchViewCapacity * batchViewStride);
    gpu.poses = createUploadBuffer(batchViewCapacity * gpu.poseStride);
    const D3D12_RANGE noRead{0, 0};
    void* mapped = nullptr;
    checkTree(gpu.constants->Map(0, &noRead, &mapped), "Map batched tree view constants");
    gpu.mappedConstants = static_cast<std::byte*>(mapped);
    checkTree(gpu.poses->Map(0, &noRead, &mapped), "Map batched tree transforms");
    gpu.mappedPoses = static_cast<std::byte*>(mapped);
    if (timestampFrequency_)
    {
        const D3D12_QUERY_HEAP_DESC timestamps{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, batchPassCapacity * batchTimestamps, 0};
        checkTree(device_->CreateQueryHeap(&timestamps, IID_PPV_ARGS(&gpu.timestamps)), "Create batched tree timestamps");
        gpu.timesReadback = buffer(device_.Get(), timestamps.Count * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
    }
    gpu.passes.reserve(batchPassCapacity);
    treeBatch_ = std::move(gpu);
}

void D3D12Renderer::buildTreeBatchDepth()
{
    auto& gpu = treeBatch_;
    auto* depth = (msaaEnabled_ ? multisample_ : singleSample_).depthBuffer.Get();
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto depthWrite = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    const std::array begin{transition(depth, depthWrite, read), transition(gpu.pyramid.Get(), read, write)};
    commands_->ResourceBarrier(gpu.pyramidReadable ? 2 : 1, begin.data());
    commands_->SetComputeRootSignature(treeDepthRoot_.Get());
    commands_->SetPipelineState(treeBatchDepthPipeline_.Get());
    commands_->SetComputeRootDescriptorTable(1, treeDepthViews_[msaaEnabled_ ? 1 : 0]);
    commands_->SetComputeRootUnorderedAccessView(2, gpu.pyramid->GetGPUVirtualAddress());
    std::array constants{static_cast<UINT>(viewport_.Width), static_cast<UINT>(viewport_.Height),
                         depth->GetDesc().SampleDesc.Count, 0u};
    for (UINT level = 0; level < pyramidLevels; ++level)
    {
        const UINT tile = 1u << (2 * (level + 1));
        const UINT x = (constants[0] + tile - 1) / tile, y = (constants[1] + tile - 1) / tile;
        constants[3] = level;
        commands_->SetComputeRoot32BitConstants(0, 4, constants.data(), 0);
        commands_->Dispatch((x + 7) / 8, (y + 7) / 8, 1);
        if (level + 1 < pyramidLevels)
        {
            const auto order = orderTreeBuffer(gpu.pyramid.Get());
            commands_->ResourceBarrier(1, &order);
        }
    }
    const std::array end{transition(depth, read, depthWrite), transition(gpu.pyramid.Get(), write, read)};
    commands_->ResourceBarrier(static_cast<UINT>(end.size()), end.data());
    gpu.pyramidReadable = true;
}

void D3D12Renderer::finishTreeBatchDepth(FXMMATRIX viewProjection)
{
    auto& gpu = treeBatch_;
    gpu.historyValid = gpu.depthUsed;
    if (!gpu.depthUsed) return;
    buildTreeBatchDepth();
    XMStoreFloat4x4(&gpu.previousClip, viewProjection);
    gpu.previousWind = frameWind_;
}

bool D3D12Renderer::drawBatchedTrees(const ViewerScene& scene, const PassVisibility& visibility, bool shadowPass,
                                    std::optional<std::size_t> shadowSlice, PassStats& stats)
{
    auto& gpu = treeBatch_;
    if (!gpu.state || visibility.count > gpu.rootCapacity || gpu.passes.size() >= batchViewCapacity) return false;
    if (!visibility.count) return true;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto ordinal = static_cast<UINT>(gpu.passes.size());
    const bool occlusion = !shadowPass && scene.treeBatchOcclusion;
    const bool history = occlusion && gpu.historyValid;
    if (occlusion && !gpu.pyramid)
    {
        std::size_t tiles = 0;
        for (UINT level = 0; level < pyramidLevels; ++level)
        {
            const UINT tile = 1u << (2 * (level + 1));
            const UINT x = (static_cast<UINT>(viewport_.Width) + tile - 1) / tile;
            const UINT y = (static_cast<UINT>(viewport_.Height) + tile - 1) / tile;
            tiles += static_cast<std::size_t>(x) * y;
        }
        gpu.pyramid = buffer(device_.Get(), tiles * sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT);
        gpu.deferred = buffer(device_.Get(), static_cast<std::size_t>(gpu.recordCapacity) * sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT);
    }
    if (occlusion) gpu.depthUsed = true;
    stats.treeOcclusionActive = occlusion;
    stats.treeOcclusionHistory = history;
    const bool testClusters = !shadowPass && scene.treeClusterCulling && scene.sprayFrustumCulling;
    bool clustered = false;
    const auto worldToView = XMLoadFloat4x4(&frameView_);
    const auto worldToClip = shadowPass
        ? XMLoadFloat4x4(shadowSlice ? &cascadeViewProjections_[*shadowSlice] : &lightViewProjection_)
        : worldToView * XMLoadFloat4x4(&projection_);
    auto* poseAddress = gpu.mappedPoses + ordinal * gpu.poseStride;
    for (const auto range : visibility.ranges)
    {
        for (UINT index = range.first; index < range.first + range.count; ++index)
        {
            const auto& object = scene.objects[index];
            BatchPose pose{};
            pose.placement = object.placement;
            const auto toWorld = object.placementMatrix();
            XMStoreFloat4x4(&pose.toWorld, toWorld);
            XMStoreFloat4x4(&pose.toView, toWorld * worldToView);
            XMStoreFloat4x4(&pose.toClip, toWorld * worldToClip);
            pose.wind = {frameWind_.x * object.placement.w * 0.5f, 0, frameWind_.y * object.placement.w * 0.5f, 0};
            const auto wind = XMLoadFloat4(&pose.wind);
            XMStoreFloat4(&pose.viewWind, XMVector3TransformNormal(wind, worldToView));
            const auto clipWind = XMVector4Transform(wind * 2.0f, worldToClip);
            XMStoreFloat4(&pose.clipWind, clipWind);
            if (testClusters && treeCrossesFrustum(gpu.bounds, XMLoadFloat4x4(&pose.toClip), clipWind))
            {
                pose.wind.w = 1.0f;
                clustered = true;
            }
            std::memcpy(poseAddress, &pose, sizeof(pose));
            poseAddress += sizeof(pose);
        }
    }
    const auto tint = shadowPass ? FixedLodTint::Shaded : scene.lodTint;
    UINT enabledParts = 0;
    for (UINT part = 0; part < gpu.partCount; ++part)
        if (!shadowPass || scene.meshShadows[gpu.meshIndices[part]].cast) enabledParts |= 1u << part;

    BatchView view{};
    XMStoreFloat4x4(&view.worldToClip, worldToClip);
    view.projection = {shadowPass ? 0.75f : scene.sprayPixelLimit, projection_._22 * viewport_.Height * 0.5f,
                       cameraNearPlane_, static_cast<float>(shadowMap_->GetDesc().Width) * 0.5f};
    const bool sparse = clustered || history;
    view.sizes = {sparse ? static_cast<UINT>(gpu.bins.size()) : gpu.baseBins,
                  gpu.partCount, visibility.count, gpu.recordCapacity};
    view.options = {(scene.sprayFrustumCulling ? 1u : 0u) | (shadowPass ? 2u : 0u) |
                    (scene.sprayLocalLod ? 4u : 0u) | (tint == FixedLodTint::SprayGroup ? 8u : 0u) |
                    (sparse ? 16u : 0u) | (history ? 32u : 0u), enabledParts, 0, 0};
    const auto address = gpu.instances->GetGPUVirtualAddress();
    view.instanceAddress = {static_cast<UINT>(address), static_cast<UINT>(address >> 32), 0, 0};
    view.previousClip = gpu.previousClip;
    view.previousWind = {gpu.previousWind.x, 0, gpu.previousWind.y, 0};
    view.viewport = {static_cast<UINT>(viewport_.Width), static_cast<UINT>(viewport_.Height), 0, 0};
    std::memcpy(gpu.mappedConstants + ordinal * batchViewStride, &view, sizeof(view));
    const auto stateBytes = sparse ? gpu.stateBytes :
        static_cast<UINT>((batchStateHeader + 3 * gpu.baseBins + 4 * gpu.partCount) * sizeof(UINT));
    gpu.passes.push_back({shadowPass, shadowSlice, view.sizes.x, sparse, clustered, history, false});

    const auto timestamp = [&](UINT slot, BatchTimestamp phase) {
        if (gpu.timestamps)
            commands_->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * batchTimestamps + phase);
    };
    const auto barriers = [&](std::span<const D3D12_RESOURCE_BARRIER> barriers) {
        commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    };
    constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto indirect = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    constexpr auto readState = indirect | D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr auto readInstances = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto bindCompute = [&](bool recovery) {
        commands_->SetComputeRootSignature(treeBatchRoot_.Get());
        commands_->SetComputeRootConstantBufferView(0, gpu.constants->GetGPUVirtualAddress() + ordinal * batchViewStride);
        for (UINT i = 0; i < gpu.tables.size() - 1; ++i)
            commands_->SetComputeRootShaderResourceView(i + 1, gpu.tables[i]->GetGPUVirtualAddress());
        commands_->SetComputeRootShaderResourceView(7, gpu.poses->GetGPUVirtualAddress() + ordinal * gpu.poseStride);
        commands_->SetComputeRootShaderResourceView(8, gpu.tables.back()->GetGPUVirtualAddress());
        commands_->SetComputeRootShaderResourceView(9, (history ? gpu.pyramid : gpu.tables.back())->GetGPUVirtualAddress());
        const std::array outputs{gpu.state.Get(), gpu.dispatch.Get(), gpu.jobs.Get(), gpu.records.Get(),
                                 gpu.arguments.Get(), gpu.instances.Get(), history ? gpu.deferred.Get() : gpu.records.Get()};
        for (UINT i = 0; i < outputs.size(); ++i)
            commands_->SetComputeRootUnorderedAccessView(i + 10, outputs[i]->GetGPUVirtualAddress());
        commands_->SetComputeRoot32BitConstant(17, recovery ? 1u : 0u, 0);
    };
    const auto dispatchOcclusion = [&](UINT pipeline) {
        commands_->SetPipelineState(treeBatchPipelines_[5].Get());
        commands_->Dispatch(1, 1, 1);
        const std::array toCull{transition(gpu.dispatch.Get(), write, indirect), orderTreeBuffer(gpu.state.Get())};
        barriers(toCull);
        commands_->SetPipelineState(treeBatchPipelines_[pipeline].Get());
        commands_->ExecuteIndirect(treeBatchDispatchSignature_.Get(), 1, gpu.dispatch.Get(), sizeof(D3D12_DISPATCH_ARGUMENTS),
                                   nullptr, 0);
        const std::array afterCull{transition(gpu.dispatch.Get(), indirect, write), orderTreeBuffer(gpu.records.Get()),
                                   orderTreeBuffer(gpu.state.Get()), orderTreeBuffer(gpu.deferred.Get())};
        barriers(afterCull);
    };
    const auto drawCut = [&](UINT slot) {
        commands_->SetPipelineState(treeBatchPipelines_[3].Get());
        commands_->Dispatch(1, 1, 1);
        const std::array toScatter{transition(gpu.dispatch.Get(), write, indirect), orderTreeBuffer(gpu.state.Get())};
        barriers(toScatter);
        commands_->SetPipelineState(treeBatchPipelines_[4].Get());
        commands_->ExecuteIndirect(treeBatchDispatchSignature_.Get(), 1, gpu.dispatch.Get(), sizeof(D3D12_DISPATCH_ARGUMENTS),
                                   nullptr, 0);
        const std::array toDraw{transition(gpu.dispatch.Get(), indirect, write), transition(gpu.arguments.Get(), write, indirect),
                                transition(gpu.state.Get(), write, readState), transition(gpu.instances.Get(), write, readInstances)};
        barriers(toDraw);
        timestamp(slot, PackEnd);
        const auto& mode = msaaEnabled_ ? multisample_ : singleSample_;
        commands_->SetPipelineState(shadowPass ? shadowPipeline_.Get() : mode.mesh.get(encodedCamera_));
        for (const auto& batch : gpu.batches)
        {
            const auto& flags = scene.meshShadows[batch.meshIndex];
            if (shadowPass && !flags.cast) continue;
            const auto& mesh = batch.proxy ? treeParts_[batch.meshIndex].geometry : meshes_[batch.meshIndex];
            auto drawTint = tint;
            if (batch.proxy && tint != FixedLodTint::Shaded) drawTint = FixedLodTint::BoughProxy;
            if (!batch.proxy && !mesh.meshInstanceCount) drawTint = FixedLodTint::Shaded;
            bindMesh(mesh, scene.asset->materials[mesh.material], scene.foliage, flags.receive, nullptr, drawTint, 0, 0, 0);
            const auto countOffset = (batchStateHeader + 2 * view.sizes.x + batch.firstBin) * sizeof(UINT);
            commands_->ExecuteIndirect(treeBatchDrawSignature_.Get(), sparse ? batch.commandCapacity : batch.binCount,
                                       gpu.arguments.Get(), batch.firstCommand * sizeof(TreeBatchDraw), gpu.state.Get(), countOffset);
            ++stats.treeSubmissionCalls;
        }
        timestamp(slot, BatchEnd);
        commands_->IASetVertexBuffers(1, 1, &instanceView_);
        commands_->CopyBufferRegion(gpu.readback.Get(), static_cast<UINT64>(slot) * gpu.stateBytes, gpu.state.Get(), 0, stateBytes);
        const std::array afterDraw{transition(gpu.arguments.Get(), indirect, write), transition(gpu.state.Get(), readState, write),
                                   transition(gpu.instances.Get(), readInstances, write)};
        barriers(afterDraw);
    };

    timestamp(ordinal, BatchStart);
    const std::array toReset{transition(gpu.state.Get(), write, D3D12_RESOURCE_STATE_COPY_DEST),
                             transition(gpu.dispatch.Get(), write, D3D12_RESOURCE_STATE_COPY_DEST),
                             transition(gpu.arguments.Get(), write, D3D12_RESOURCE_STATE_COPY_DEST)};
    barriers(std::span(toReset).first(clustered ? 3 : 2));
    commands_->CopyBufferRegion(gpu.state.Get(), 0, gpu.reset.Get(), 0, stateBytes);
    commands_->CopyBufferRegion(gpu.dispatch.Get(), 0, gpu.reset.Get(), gpu.stateBytes, 2 * sizeof(D3D12_DISPATCH_ARGUMENTS));
    if (clustered)
        commands_->CopyBufferRegion(gpu.arguments.Get(), 0, gpu.reset.Get(), gpu.stateBytes, sizeof(D3D12_DISPATCH_ARGUMENTS));
    const std::array afterReset{transition(gpu.state.Get(), D3D12_RESOURCE_STATE_COPY_DEST, write),
                                transition(gpu.dispatch.Get(), D3D12_RESOURCE_STATE_COPY_DEST, write),
                                transition(gpu.arguments.Get(), D3D12_RESOURCE_STATE_COPY_DEST, write)};
    barriers(std::span(afterReset).first(clustered ? 3 : 2));
    bindCompute(false);

    commands_->SetPipelineState(treeBatchPipelines_[0].Get());
    commands_->Dispatch(visibility.count, gpu.partCount, 1);
    const std::array toSelect{transition(gpu.dispatch.Get(), write, indirect), orderTreeBuffer(gpu.jobs.Get()),
                              orderTreeBuffer(gpu.state.Get())};
    barriers(toSelect);
    timestamp(ordinal, CoarseEnd);
    commands_->SetPipelineState(treeBatchPipelines_[1].Get());
    commands_->ExecuteIndirect(treeBatchDispatchSignature_.Get(), 1, gpu.dispatch.Get(), 0, nullptr, 0);
    const std::array toEncode{transition(gpu.dispatch.Get(), indirect, write), orderTreeBuffer(gpu.state.Get()),
                              orderTreeBuffer(gpu.records.Get()), transition(gpu.arguments.Get(), write, indirect),
                              orderTreeBuffer(gpu.instances.Get())};
    barriers(std::span(toEncode).first(clustered ? 5 : 3));
    timestamp(ordinal, SelectionEnd);
    if (clustered)
    {
        commands_->SetPipelineState(treeBatchPipelines_[2].Get());
        commands_->ExecuteIndirect(treeBatchDispatchSignature_.Get(), 1, gpu.arguments.Get(), 0, nullptr, 0);
        const std::array afterClip{transition(gpu.arguments.Get(), indirect, write),
                                   orderTreeBuffer(gpu.state.Get()), orderTreeBuffer(gpu.records.Get()),
                                   orderTreeBuffer(gpu.instances.Get())};
        barriers(afterClip);
    }
    timestamp(ordinal, ClipEnd);
    if (history) dispatchOcclusion(6);
    timestamp(ordinal, HistoryEnd);
    drawCut(ordinal);
    stats.gpuTreeDispatches += clustered ? 5 : 4;
    if (history)
    {
        const UINT late = ordinal + 1;
        gpu.passes.push_back({false, std::nullopt, view.sizes.x, true, false, false, true});
        timestamp(late, BatchStart);
        buildTreeBatchDepth();
        timestamp(late, CoarseEnd);
        // Keep the compact deferred queue and reset only the fields that build a draw cut.
        constexpr UINT activeCountOffset = 16;
        const auto toClear = transition(gpu.state.Get(), write, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers(std::span(&toClear, 1));
        commands_->CopyBufferRegion(gpu.state.Get(), activeCountOffset, gpu.reset.Get(), activeCountOffset, sizeof(UINT));
        commands_->CopyBufferRegion(gpu.state.Get(), batchStateHeader * sizeof(UINT), gpu.reset.Get(),
                                    batchStateHeader * sizeof(UINT), 3ull * view.sizes.x * sizeof(UINT));
        const auto afterClear = transition(gpu.state.Get(), D3D12_RESOURCE_STATE_COPY_DEST, write);
        barriers(std::span(&afterClear, 1));
        bindCompute(true);
        dispatchOcclusion(7);
        timestamp(late, SelectionEnd);
        timestamp(late, ClipEnd);
        timestamp(late, HistoryEnd);
        drawCut(late);
        stats.gpuTreeDispatches += 6 + pyramidLevels; // History, retest, late packing, and the mid-frame build.
    }
    if (occlusion) stats.gpuTreeDispatches += pyramidLevels;
    stats.treeSubmissionMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (scene.sprayLods[0] && (!shadowPass || scene.meshShadows[scene.sprayLods[0]->meshIndex].cast))
        stats.sprayPlacements += visibility.count * meshes_[scene.sprayLods[0]->meshIndex].meshInstanceCount;
    return true;
}

void D3D12Renderer::resolveTreeBatchTimes()
{
    const auto& gpu = treeBatch_;
    if (gpu.timestamps && !gpu.passes.empty())
        commands_->ResolveQueryData(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                    static_cast<UINT>(gpu.passes.size()) * batchTimestamps, gpu.timesReadback.Get(), 0);
}

void D3D12Renderer::readTreeBatchStats(const ViewerScene& scene, PassStats& camera, PassStats& shadow,
                                     std::array<PassStats, sunCascadeSplits.size()>& cascades)
{
    const auto& gpu = treeBatch_;
    if (gpu.passes.empty()) return;
    D3D12_RANGE range{0, gpu.passes.size() * gpu.stateBytes};
    void* mapped = nullptr;
    checkTree(gpu.readback->Map(0, &range, &mapped), "Read batched tree counts");
    const auto* results = static_cast<const std::byte*>(mapped);
    const UINT64* ticks = nullptr;
    if (gpu.timesReadback)
    {
        const D3D12_RANGE timeRange{0, gpu.passes.size() * batchTimestamps * sizeof(UINT64)};
        checkTree(gpu.timesReadback->Map(0, &timeRange, &mapped), "Read batched tree GPU times");
        ticks = static_cast<const UINT64*>(mapped);
    }
    for (std::size_t i = 0; i < gpu.passes.size(); ++i)
    {
        const auto& pass = gpu.passes[i];
        const auto* state = reinterpret_cast<const UINT*>(results + i * gpu.stateBytes);
        const auto instances = std::span(state + batchStateHeader, pass.binCount);
        const auto* nodes = state + batchStateHeader + 3 * pass.binCount;
        const auto* visibleSprays = nodes + gpu.partCount;
        const auto* parents = visibleSprays + gpu.partCount;
        const auto* parentSprays = parents + gpu.partCount;
        const auto* activeBins = parentSprays + gpu.partCount;
        const auto apply = [&](PassStats& stats) {
            if (pass.recovery)
            {
                stats.treeOcclusionRejected += state[12];
                stats.treeOcclusionRecovered += state[13];
            }
            else
            {
                stats.treeBatchJobs += state[0];
                stats.treeClusterTests += state[5];
                stats.treeClusterRejects += state[6];
                stats.treeClusterSplits += state[7];
                stats.treeClusterFallbacks += state[8];
                stats.treeOcclusionDeferred += state[10];
                stats.treeOcclusionTests += state[11];
            }
            if (ticks)
            {
                const auto* t = ticks + i * batchTimestamps;
                const auto elapsed = [&](BatchTimestamp begin, BatchTimestamp end) {
                    return 1000.0 * static_cast<double>(t[end] - t[begin]) / static_cast<double>(timestampFrequency_);
                };
                if (pass.recovery)
                {
                    stats.treePyramidGpuMilliseconds += elapsed(BatchStart, CoarseEnd);
                    stats.treeOcclusionRetestMilliseconds += elapsed(CoarseEnd, BatchEnd);
                }
                else
                {
                    stats.treeBatchGpuMilliseconds[0] += elapsed(BatchStart, CoarseEnd);
                    stats.treeBatchGpuMilliseconds[1] += elapsed(CoarseEnd, SelectionEnd);
                    stats.treeBatchGpuMilliseconds[2] += elapsed(HistoryEnd, PackEnd);
                    stats.treeBatchGpuMilliseconds[3] += elapsed(PackEnd, BatchEnd);
                    if (pass.clusters) stats.treeClusterGpuMilliseconds += elapsed(SelectionEnd, ClipEnd);
                    if (pass.history) stats.treeOcclusionGpuMilliseconds += elapsed(ClipEnd, HistoryEnd);
                }
            }
            for (UINT p = 0; !pass.recovery && p < gpu.partCount; ++p)
            {
                stats.treeNodeTests += nodes[p];
                const auto mesh = gpu.meshIndices[p];
                const bool leaves = scene.sprayLods[0] && mesh == scene.sprayLods[0]->meshIndex;
                const bool wood = scene.sprayLods[1] && mesh == scene.sprayLods[1]->meshIndex;
                if (leaves) stats.visibleSprayPlacements += visibleSprays[p];
                if (pass.sparse)
                {
                    stats.treeNodesDrawn += parents[p];
                    if (leaves || wood) stats.treeProxySprays[leaves ? 0 : 1] += parentSprays[p];
                    if (leaves) stats.visibleSprayPlacements += parentSprays[p];
                }
            }
            const auto drawnBins = pass.sparse ? state[4] : pass.binCount;
            for (UINT index = 0; index < drawnBins; ++index)
            {
                const auto bin = pass.sparse ? activeBins[index] : index;
                const auto count = instances[bin];
                if (!count) continue;
                const auto& baked = gpu.bins[bin];
                const auto& batch = gpu.batches[baked.batch];
                const bool leaves = scene.sprayLods[0] && batch.meshIndex == scene.sprayLods[0]->meshIndex;
                const bool wood = scene.sprayLods[1] && batch.meshIndex == scene.sprayLods[1]->meshIndex;
                const auto triangles = static_cast<std::uint64_t>(baked.triangles) * count;
                ++stats.draws;
                ++stats.treeDraws;
                stats.triangles += triangles;
                stats.treeBatchInstances += count;
                stats.treeCutHash ^= ((static_cast<std::uint64_t>(bin) << 32) | count) * 1099511628211ull;
                if (batch.proxy && !pass.sparse)
                {
                    stats.treeNodesDrawn += count;
                    if (leaves || wood) stats.treeProxySprays[leaves ? 0 : 1] += baked.sprays * count;
                    if (leaves) stats.visibleSprayPlacements += baked.sprays * count;
                }
                if (leaves)
                {
                    stats.foliageTriangles += triangles;
                    ++stats.foliageDraws;
                    if (batch.proxy) stats.treeCanopyTriangles[0] += triangles;
                }
                else if (wood)
                {
                    stats.sprayWoodTriangles += triangles;
                    if (batch.proxy) stats.treeCanopyTriangles[1] += triangles;
                }
                else stats.treeTrunkTriangles += triangles;
            }
        };
        apply(pass.shadow ? shadow : camera);
        if (pass.slice) apply(cascades[*pass.slice]);
    }
    range.End = 0;
    if (ticks) gpu.timesReadback->Unmap(0, &range);
    gpu.readback->Unmap(0, &range);
}
