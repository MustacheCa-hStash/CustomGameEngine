// Fullscreen passes for the meadow camera. All resources in this file hold linear scene color.
#include "MeadowWeightedPS.h"
#include "MeadowSeedPS.h"
#include "MeadowTemporalPS.h"

const wchar_t* D3D12Renderer::meadowAaName() const
{
    constexpr const wchar_t* names[]{L"1x", L"4x MSAA", L"HDR-weighted 4x", L"1x oaks / 4x grass", L"Native temporal"};
    return names[static_cast<unsigned>(meadowAa_)];
}

void D3D12Renderer::setMeadowAa(MeadowAa mode)
{
    const bool multi = mode == MeadowAa::Msaa || mode == MeadowAa::Weighted || mode == MeadowAa::Split;
    if (multi && !supportsMsaa()) throw std::runtime_error("This meadow mode requires 4x MSAA.");
    meadowAa_ = mode;
    msaaEnabled_ = multi;
    treeBatch_.historyValid = false;
    aaFrame_ = 0;
    aaHistoryValid_ = false;
}

void D3D12Renderer::configureMeadowAa()
{
    wchar_t value[16]{};
    if (!GetEnvironmentVariableW(L"MEADOW_AA_MODE",value,16)) return;
    constexpr std::wstring_view modes[]{L"off",L"msaa",L"hdr",L"split",L"temporal"};
    const auto found = std::ranges::find(modes, std::wstring_view(value));
    if (found == std::end(modes)) throw std::runtime_error("Unknown meadow AA mode.");
    setMeadowAa(static_cast<MeadowAa>(found-modes));
}

void D3D12Renderer::createMeadowAaPipelines()
{
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    D3D12_ROOT_PARAMETER parameters[5]{};
    for (UINT i=0;i<4;++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {1,&ranges[i]};
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[4].Constants = {0,0,40};
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{5,parameters};
    ComPtr<ID3DBlob> bytes;
    check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr),"Serialize meadow AA root");
    check(device_->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&aaRoot_)),"Create meadow AA root");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = aaRoot_.Get();
    p.VS = {displayVS,sizeof(displayVS)};
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.SampleMask = UINT_MAX;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = sceneColorFormat;
    p.SampleDesc.Count = 1;
    p.PS = {meadowTemporalPS,sizeof(meadowTemporalPS)};
    aaTemporal_ = createPipeline(device_.Get(),p,"Create temporal resolve");
    if (!supportsMsaa()) return;
    p.PS = {meadowWeightedPS,sizeof(meadowWeightedPS)};
    aaWeighted_ = createPipeline(device_.Get(),p,"Create weighted resolve");
    p.PS = {meadowSeedPS,sizeof(meadowSeedPS)};
    p.SampleDesc = msaaSamples;
    p.RasterizerState.MultisampleEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.DSVFormat = depthFormat;
    aaSeed_ = createPipeline(device_.Get(),p,"Create meadow color and depth seed");
}

void D3D12Renderer::writeAaView(UINT slot, ID3D12Resource* resource, DXGI_FORMAT format, bool multisampled)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = format;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (!multisampled) v.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(resource,&v,{aaCpu_.ptr+slot*textureDescriptorSize_});
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::aaView(UINT slot) const
{
    return {aaGpu_.ptr+UINT64(slot)*textureDescriptorSize_};
}

void D3D12Renderer::seedGrassSamples(const ViewerScene& scene, PassStats& stats)
{
    commands_->SetPipelineState(singleSample_.mesh.linear.Get());
    stats.triangles += drawMesh(ground_,scene.groundMaterial,false,scene.groundShadows.receive,1,0);
    ++stats.draws;
    if (scene.captureLighting != CaptureLighting::Transmission && scene.captureLighting != CaptureLighting::ShadowVisibility && singleSample_.sky.linear)
    {
        drawSky(singleSample_.sky.linear.Get(),frameView_,scene.captureLighting==CaptureLighting::SkyOnly);
        ++stats.draws;
        ++stats.triangles;
    }
    auto* depth = singleSample_.depthBuffer.Get();
    transitionResource(commands_.Get(),sceneColor_.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transitionResource(commands_.Get(),depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands_->OMSetRenderTargets(1,&multisampleTarget_,FALSE,&multisample_.depthTarget);
    commands_->SetGraphicsRootSignature(aaRoot_.Get());
    commands_->SetPipelineState(aaSeed_.Get());
    commands_->SetGraphicsRootDescriptorTable(0,sceneColorView_);
    commands_->SetGraphicsRootDescriptorTable(1,aaView(1));
    commands_->DrawInstanced(3,1,0,0);
    transitionResource(commands_.Get(),depth,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commands_->SetGraphicsRootSignature(rootSignature_.Get());
    commands_->SetGraphicsRootConstantBufferView(3,frameConstants_->GetGPUVirtualAddress());
    commands_->SetGraphicsRootConstantBufferView(5,skyIrradiance_->GetGPUVirtualAddress());
    commands_->SetGraphicsRootDescriptorTable(2,shadowTexture_);
    commands_->SetGraphicsRootDescriptorTable(8,probeGpuView_);
    commands_->SetPipelineState(multisample_.mesh.linear.Get());
}

void D3D12Renderer::resolveWeighted(float exposure)
{
    const auto read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto target = D3D12_RESOURCE_STATE_RENDER_TARGET;
    transitionResource(commands_.Get(),multisampleColor_.Get(),target,read);
    transitionResource(commands_.Get(),sceneColor_.Get(),read,target);
    commands_->OMSetRenderTargets(1,&sceneTarget_,FALSE,nullptr);
    commands_->SetGraphicsRootSignature(aaRoot_.Get());
    commands_->SetPipelineState(aaWeighted_.Get());
    commands_->SetGraphicsRootDescriptorTable(3,aaView(0));
    commands_->SetGraphicsRoot32BitConstant(4,std::bit_cast<UINT>(std::exp2(exposure)),35);
    commands_->DrawInstanced(3,1,0,0);
    transitionResource(commands_.Get(),multisampleColor_.Get(),read,target);
    transitionResource(commands_.Get(),sceneColor_.Get(),target,read);
}

void D3D12Renderer::resolveTemporal(FXMMATRIX clip)
{
    const auto read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto target = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const UINT dest = aaFrame_%2, previous = 1-dest;
    for (UINT i=0;i<2;++i)
        if (!aaHistory_[i])
        {
            aaHistory_[i] = createCameraColor(sceneColorFormat,{1,0},aaTargets_[i],read);
            writeAaView(2+i,aaHistory_[i].Get(),sceneColorFormat,false);
        }
    struct Constants { XMFLOAT4X4 inverse, previous; XMFLOAT2 windDelta; float valid, exposure; XMFLOAT2 jitterDelta; float still, pad; } c{};
    XMStoreFloat4x4(&c.inverse,XMMatrixInverse(nullptr,clip));
    c.previous = aaPrevious_;
    c.windDelta = {aaWind_.x-frameWind_.x,aaWind_.y-frameWind_.y};
    c.valid = aaHistoryValid_ ? 1.f : 0.f;
    c.jitterDelta = {aaJitter_.x-aaPreviousJitter_.x,aaJitter_.y-aaPreviousJitter_.y};
    c.still = std::memcmp(&frameView_,&aaPreviousView_,sizeof(frameView_))==0 &&
              std::abs(c.windDelta.x)+std::abs(c.windDelta.y)<.00001f;
    transitionResource(commands_.Get(),singleSample_.depthBuffer.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,read);
    transitionResource(commands_.Get(),aaHistory_[dest].Get(),read,target);
    commands_->OMSetRenderTargets(1,&aaTargets_[dest],FALSE,nullptr);
    commands_->SetGraphicsRootSignature(aaRoot_.Get());
    commands_->SetPipelineState(aaTemporal_.Get());
    commands_->SetGraphicsRootDescriptorTable(0,sceneColorView_);
    commands_->SetGraphicsRootDescriptorTable(1,aaView(1));
    commands_->SetGraphicsRootDescriptorTable(2,aaView(2+previous));
    commands_->SetGraphicsRoot32BitConstants(4,40,&c,0);
    commands_->DrawInstanced(3,1,0,0);
    transitionResource(commands_.Get(),aaHistory_[dest].Get(),target,read);
    transitionResource(commands_.Get(),singleSample_.depthBuffer.Get(),read,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    XMStoreFloat4x4(&aaPrevious_,clip);
    aaWind_ = frameWind_;
    aaPreviousView_ = frameView_;
    aaPreviousJitter_ = aaJitter_;
    aaHistoryValid_ = true;
    ++aaFrame_;
}
