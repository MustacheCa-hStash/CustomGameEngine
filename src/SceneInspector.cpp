#include "SceneInspector.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
void showVector(const char* label, const DirectX::XMFLOAT3& value)
{
    ImGui::Text("%s  %.2f, %.2f, %.2f", label, value.x, value.y, value.z);
}

bool inspectShadowBias(ShadowBiasSettings& bias, const D3D12Renderer& renderer)
{
    constexpr auto sliderFlags = ImGuiSliderFlags_AlwaysClamp;
    // Stack the labels so they fit even when the inspector is narrow.
    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::TextUnformatted("Constant depth bias (draw)");
    bool changed = ImGui::SliderInt("##constantDepthBias", &bias.constant, 0, ShadowBiasSettings::maxConstant,
                                    "%d", sliderFlags | ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Integer multiplier of a D32 float depth step.\nThe step depends on the primitive's maximum depth.");
    ImGui::TextUnformatted("Slope-scaled depth bias (draw)");
    changed |= ImGui::SliderFloat("##slopeDepthBias", &bias.slopeScale, 0.0f, ShadowBiasSettings::maxSlopeScale,
                                  "%.2f x", sliderFlags);
    ImGui::SetItemTooltip("Multiplier of the primitive's maximum depth slope per shadow-map pixel.");
    ImGui::TextUnformatted("Shadow sampling bias (receive)");
    changed |= ImGui::SliderFloat("##samplingBias", &bias.samplingTexels, 0.0f,
                                  ShadowBiasSettings::maxReceiverTexels, "%.2f texels", sliderFlags);
    ImGui::SetItemTooltip("Converted to normalized light depth and subtracted before the shadow comparison.");
    ImGui::TextUnformatted("Normal bias (receive)");
    changed |= ImGui::SliderFloat("##normalBias", &bias.normalTexels, 0.0f,
                                  ShadowBiasSettings::maxReceiverTexels, "%.2f texels", sliderFlags);
    ImGui::SetItemTooltip("Offsets the shadow lookup along the sun-facing world normal.\n"
                          "Zero head-on, full offset at grazing incidence.");
    ImGui::PopItemWidth();
    ImGui::Text("Sampling offset %.6f in light depth", bias.samplingTexels * renderer.shadowDepthPerTexel());
    ImGui::Text("Max normal offset %.4f world units", bias.normalTexels * renderer.shadowWorldUnitsPerTexel());
    ImGui::TextWrapped("Draw bias cap %.3f in light depth, after adding both draw terms.",
                       ShadowBiasSettings::drawClamp);
    if (ImGui::Button("Restore default bias"))
    {
        bias = {};
        changed = true;
    }
    return changed;
}
}

SceneInspector::SceneInspector(HWND window, const D3D12Renderer& renderer)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap.NumDescriptors = previewSlot + 1;
    if (FAILED(renderer.device()->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors_))))
    {
        throw std::runtime_error("Could not create inspector descriptors.");
    }
    descriptorSize_ = renderer.device()->GetDescriptorHandleIncrementSize(heap.Type);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    const float scale = std::clamp(static_cast<float>(GetDpiForWindow(window)) / 96.0f, 1.0f, 2.0f);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
    ImGui::GetStyle().FontSizeBase = 17.0f;
    wchar_t windowsDirectory[MAX_PATH];
    const auto length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
    {
        const auto fontPath = std::filesystem::path(windowsDirectory) / L"Fonts" / L"segoeui.ttf";
        std::error_code error;
        if (std::filesystem::is_regular_file(fontPath, error))
        {
            const auto utf8 = fontPath.u8string();
            io.FontDefault = io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(utf8.c_str()));
        }
    }
    ImGui::GetStyle().WindowRounding = 6.0f * scale;
    ImGui::GetStyle().FrameRounding = 4.0f * scale;

    if (!ImGui_ImplWin32_Init(window))
    {
        ImGui::DestroyContext();
        throw std::runtime_error("Could not initialize Win32 inspector input.");
    }
    ImGui_ImplDX12_InitInfo info;
    info.Device = renderer.device();
    info.CommandQueue = renderer.queue();
    info.NumFramesInFlight = 1;
    info.RTVFormat = renderer.backBufferFormat();
    info.SrvDescriptorHeap = descriptors_.Get();
    info.UserData = this;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                  D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        auto& inspector = *static_cast<SceneInspector*>(info->UserData);
        *cpu = inspector.descriptors_->GetCPUDescriptorHandleForHeapStart();
        *gpu = inspector.descriptors_->GetGPUDescriptorHandleForHeapStart();
        for (unsigned i = 0; i < previewSlot; ++i)
        {
            if (!inspector.fontSlots_.test(i))
            {
                inspector.fontSlots_.set(i);
                cpu->ptr += i * inspector.descriptorSize_;
                gpu->ptr += i * inspector.descriptorSize_;
                return;
            }
        }
        throw std::runtime_error("Inspector texture descriptors exhausted.");
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE) {
        auto& inspector = *static_cast<SceneInspector*>(info->UserData);
        const auto start = inspector.descriptors_->GetCPUDescriptorHandleForHeapStart();
        inspector.fontSlots_.reset((cpu.ptr - start.ptr) / inspector.descriptorSize_);
    };
    if (!ImGui_ImplDX12_Init(&info))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("Could not initialize the D3D12 inspector.");
    }
}

SceneInspector::~SceneInspector()
{
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

bool SceneInspector::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!ImGui::GetCurrentContext())
    {
        return false;
    }
    const auto handled = ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
    const auto& io = ImGui::GetIO();
    return handled != 0 || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && io.WantCaptureMouse);
}

bool SceneInspector::wantsKeyboard()
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

void SceneInspector::resetSelection()
{
    page_ = Page::Object;
    meshIndex_ = 0;
    objectIndex_ = 0;
    ground_ = false;
}

InspectorActions SceneInspector::frame(ViewerScene& scene, const D3D12Renderer& renderer,
                                       const D3D12Renderer::FrameTiming& timing,
                                       const std::string& sceneError, bool hasImportedScene)
{
    InspectorActions actions;
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (visible_)
    {
        const auto& io = ImGui::GetIO();
        const float width = std::min(440.0f * ImGui::GetStyle().FontScaleDpi, io.DisplaySize.x - 24.0f);
        ImGui::SetNextWindowPos({12.0f, 12.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({width, std::min(900.0f, io.DisplaySize.y - 24.0f)}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scene Inspector", &visible_))
        {
            if (ImGui::BeginCombo("Scene", scene.name()))
            {
                constexpr const char* names[] = {"Cube", "Oak", "Grove", "Imported GLB"};
                for (int i = 0; i < (hasImportedScene ? 4 : 3); ++i)
                {
                    const auto kind = static_cast<SceneKind>(i);
                    if (ImGui::Selectable(names[i], scene.kind == kind))
                    {
                        actions.scene = kind;
                    }
                }
                ImGui::EndCombo();
            }
            if (!sceneError.empty())
            {
                ImGui::TextWrapped("%s", sceneError.c_str());
            }
            if (ImGui::CollapsingHeader("Scene controls", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Button("Overview")) actions.camera = CameraPreset::Overview;
                ImGui::SameLine();
                if (ImGui::Button("Close-up")) actions.camera = CameraPreset::CloseUp;
                if (scene.foliage)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Under canopy")) actions.camera = CameraPreset::UnderCanopy;
                }
                actions.sunChanged = ImGui::SliderFloat("Sun azimuth", &scene.sunAzimuth, -180, 180, "%.0f deg",
                                                        ImGuiSliderFlags_AlwaysClamp);
                actions.sunChanged |= ImGui::SliderFloat("Sun elevation", &scene.sunElevation, 25, 85, "%.0f deg",
                                                         ImGuiSliderFlags_AlwaysClamp);
                if (scene.foliage)
                {
                    actions.settingsChanged |= ImGui::SliderFloat("Wind strength", &scene.windStrength,
                        0.0f, maxWindStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                    bool paused = scene.windPaused;
                    actions.toggleWind = ImGui::Checkbox("Pause wind (P)", &paused);
                }
                if (scene.kind == SceneKind::Grove)
                {
                    constexpr const char* counts[] = {"1", "25", "100", "400"};
                    int index = static_cast<int>(std::find(groveCounts.begin(), groveCounts.end(), scene.treeCount) -
                                                 groveCounts.begin());
                    if (ImGui::Combo("Tree count", &index, counts, static_cast<int>(groveCounts.size())))
                    {
                        scene.treeCount = groveCounts[index];
                        actions.treeCountChanged = true;
                    }
                }
                ImGui::BeginDisabled(!renderer.supportsMsaa());
                bool msaa = renderer.msaaEnabled();
                actions.toggleMsaa = ImGui::Checkbox("4x MSAA (M)", &msaa);
                ImGui::EndDisabled();
            }
            if (ImGui::CollapsingHeader("Shadow diagnostics"))
            {
                actions.settingsChanged |= inspectShadowBias(scene.shadowBias, renderer);
            }
            if (ImGui::CollapsingHeader("Rendering"))
            {
                ImGui::Text("CPU %.2f ms", timing.cpuMilliseconds);
                if (timing.gpuMilliseconds) ImGui::Text("GPU %.2f ms", *timing.gpuMilliseconds);
                else ImGui::TextUnformatted("GPU -- ms");
                ImGui::Text("Draw calls (scene + HUD) %u", timing.drawCalls);
            }
            actions.settingsChanged |= inspect(scene, renderer);
        }
        ImGui::End();
    }
    ImGui::Render();
    return actions;
}

bool SceneInspector::inspect(ViewerScene& scene, const D3D12Renderer& renderer)
{
    if (ImGui::CollapsingHeader("Outliner", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* label = scene.kind == SceneKind::Grove ? "Grove / representative tree" :
                            scene.kind == SceneKind::Imported ? "Imported asset" : scene.name();
        if (ImGui::Selectable(label, !ground_))
        {
            if (ground_) meshIndex_ = 0;
            ground_ = false;
            page_ = Page::Object;
        }
        if (ImGui::Selectable("Ground", ground_))
        {
            ground_ = true;
            page_ = Page::Object;
        }
        if (!ground_ && scene.kind == SceneKind::Grove)
        {
            objectIndex_ = std::clamp(objectIndex_, 0, static_cast<int>(scene.treeCount) - 1);
            if (scene.treeCount > 1)
            {
                int tree = objectIndex_ + 1;
                ImGui::SliderInt("Tree instance", &tree, 1, static_cast<int>(scene.treeCount), "%d",
                                 ImGuiSliderFlags_AlwaysClamp);
                objectIndex_ = tree - 1;
            }
        }
    }
    if (!ImGui::CollapsingHeader("Inspector", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return false;
    }
    const auto& meshes = scene.asset->meshes;
    const auto& mesh = ground_ ? scene.ground : meshes.at(meshIndex_);
    auto& material = ground_ ? scene.groundMaterial : scene.asset->materials.at(mesh.material);
    ImGui::BeginDisabled(page_ == Page::Object);
    if (ImGui::Button("Object")) page_ = Page::Object;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Mesh")) page_ = Page::Mesh;
    ImGui::SameLine();
    if (ImGui::Button("Material")) page_ = Page::Material;
    ImGui::SameLine();
    ImGui::BeginDisabled(!material.colorTexture);
    if (ImGui::Button("Texture")) page_ = Page::Texture;
    ImGui::EndDisabled();

    bool changed = false;
    switch (page_)
    {
    case Page::Object:
    {
        const auto& object = scene.objects[std::min<std::size_t>(objectIndex_,
            scene.kind == SceneKind::Grove ? scene.treeCount - 1 : 0)];
        ImGui::Text("Scene ID  %u", ground_ ? 0u : object.id);
        const auto& placement = object.placement;
        showVector("Placement", ground_ ? DirectX::XMFLOAT3{} : DirectX::XMFLOAT3{placement.x, 0, placement.y});
        ImGui::Text("Yaw  %.1f deg   Scale  %.2f", ground_ ? 0.0f : DirectX::XMConvertToDegrees(placement.z),
                    ground_ ? 1.0f : placement.w);
        if (!ground_)
        {
            const auto file = scene.source.filename().u8string();
            ImGui::TextWrapped("%s", reinterpret_cast<const char*>(file.c_str()));
            if (ImGui::IsItemHovered())
            {
                const auto path = scene.source.u8string();
                ImGui::SetTooltip("%s", reinterpret_cast<const char*>(path.c_str()));
            }
        }
        const auto slots = ground_ ? std::span(&scene.ground, 1) : std::span(scene.asset->meshes);
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto& part = slots[i];
            const auto& slot = ground_ ? scene.groundMaterial : scene.asset->materials[part.material];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(part.name.c_str()))
            {
                if (!ground_) meshIndex_ = i;
                page_ = Page::Mesh;
            }
            ImGui::TextWrapped("Material  %s", slot.name.c_str());
            ImGui::PopID();
        }
        break;
    }
    case Page::Mesh:
    {
        ImGui::TextUnformatted(mesh.name.c_str());
        auto& shadows = ground_ ? scene.groundShadows : scene.meshShadows[meshIndex_];
        changed = ImGui::Checkbox("Cast shadows", &shadows.cast);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Receive shadows", &shadows.receive);
        if (!ground_ && scene.kind == SceneKind::Grove)
        {
            ImGui::TextDisabled("Applies to this mesh on every tree.");
        }
        ImGui::Text("Vertices  %zu   Triangles  %zu", mesh.vertices.size(), mesh.indices.size() / 3);
        ImGui::Text("UV sets  %s", mesh.uvMask == 3 ? "0, 1" : mesh.uvMask == 1 ? "0" : mesh.uvMask == 2 ? "1" : "None");
        const auto bounds = meshBounds(std::span(&mesh, 1), false);
        showVector("Local center", bounds.center);
        showVector("Half extent", bounds.halfExtent);
        showVector("Asset translation", {mesh.localToWorld._41, mesh.localToWorld._42, mesh.localToWorld._43});
        if (ImGui::Button("Inspect assigned material")) page_ = Page::Material;
        ImGui::TextWrapped("%s", material.name.c_str());
        break;
    }
    case Page::Material:
    {
        ImGui::TextWrapped("%s", material.name.c_str());
        ImGui::TextDisabled("%s", ground_ ? "Scene material" : "Shared asset material");
        float color[]{material.color.x, material.color.y, material.color.z};
        if (ImGui::ColorEdit3("Base color", color))
        {
            material.color.x = color[0];
            material.color.y = color[1];
            material.color.z = color[2];
            changed = true;
        }
        if (material.alphaCutoff >= 0.0f)
        {
            changed |= ImGui::SliderFloat("Alpha cutoff", &material.alphaCutoff, 0.0f, 1.0f, "%.2f",
                                          ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::SliderFloat("Backlighting", &material.transmission, 0.0f, 1.0f, "%.2f",
                                          ImGuiSliderFlags_AlwaysClamp);
        }
        if (material.colorTexture)
        {
            if (ImGui::Button("Inspect base-color texture")) page_ = Page::Texture;
            ImGui::TextWrapped("%s (UV %u)", scene.asset->textures[*material.colorTexture].name.c_str(),
                               material.textureCoordinateSet);
        }
        break;
    }
    case Page::Texture:
        if (material.colorTexture)
        {
            showTexture(scene.asset->textures[*material.colorTexture], *material.colorTexture, renderer);
        }
        break;
    }
    return changed;
}

void SceneInspector::showTexture(const TextureAsset& texture, std::size_t index,
                                 const D3D12Renderer& renderer) const
{
    const auto& base = texture.mips.front();
    ImGui::TextUnformatted(texture.name.c_str());
    ImGui::Text("%u x %u  |  sRGB  |  %zu mips", base.width, base.height, texture.mips.size());
    auto cpu = descriptors_->GetCPUDescriptorHandleForHeapStart();
    auto gpu = descriptors_->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += previewSlot * descriptorSize_;
    gpu.ptr += previewSlot * descriptorSize_;
    renderer.writeTextureView(index, cpu);
    const float previewSize = 240.0f * ImGui::GetStyle().FontScaleDpi;
    const float scale = std::min(previewSize / static_cast<float>(std::max(base.width, base.height)),
                                 std::max(1.0f, ImGui::GetContentRegionAvail().x) / base.width);
    const ImVec2 size{base.width * scale, base.height * scale};
    const auto position = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(position, {position.x + size.x, position.y + size.y},
                                             IM_COL32(88, 91, 95, 255));
    ImGui::Image(static_cast<ImTextureID>(gpu.ptr), size);
}

void SceneInspector::draw(ID3D12GraphicsCommandList* commands) const
{
    if (auto* data = ImGui::GetDrawData(); data && data->TotalVtxCount > 0)
    {
        ID3D12DescriptorHeap* heaps[] = {descriptors_.Get()};
        commands->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(data, commands);
    }
}
