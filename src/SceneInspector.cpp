#include "SceneInspector.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
void showVector(const char* label, const DirectX::XMFLOAT3& value)
{
    ImGui::Text("%s  %.2f, %.2f, %.2f", label, value.x, value.y, value.z);
}

void inspectBranchLod(ViewerScene& scene, const D3D12Renderer& renderer, const CameraView& camera, float windPhase)
{
    if (!scene.fixedLod)
    {
        ImGui::TextWrapped("%s", scene.fixedLodStatus.c_str());
        return;
    }
    const auto& lod = *scene.fixedLod;
    const auto& mesh = scene.asset->meshes[lod.meshIndex];
    constexpr const char* levels[] = {"Source GLB", "LOD 0 - Clustered full detail", "LOD 1 - Reduced",
                                      "LOD 2 - Locked rims", "LOD 3 - Rim collapse", "LOD 4 - Forced far",
                                      "LOD 5 - Four triangles per leaf", "LOD 6 - Two triangles per leaf",
                                      "Automatic (LOD 3 / 4 / 6)"};
    constexpr int automaticItem = IM_ARRAYSIZE(levels) - 1;
    static_assert(automaticItem == fixedLodLevelCount + 1);
    const bool hasProxies = lod.levels.size() == fixedLodLevelCount;
    const auto view = camera.matrix();
    auto& selection = scene.branchLodSelection;
    int level = scene.branchLod == automaticBranchLod ? automaticItem : scene.branchLod + 1;
    const int itemCount = static_cast<int>(lod.levels.size()) + (hasProxies ? 2 : 1);
    if (ImGui::Combo("Representation", &level, levels, itemCount, itemCount))
    {
        scene.branchLod = level == automaticItem ? automaticBranchLod : level - 1;
        selection.frozen = false;
        renderer.updateBranchLod(scene, view);
    }
    const bool automatic = scene.branchLod == automaticBranchLod;
    const bool source = scene.branchLod == sourceBranchLod;

    constexpr const char* tints[] = {"Shaded", "Triangle", "Group", "Cluster", "LOD level"};
    int tint = static_cast<int>(scene.lodTint);
    ImGui::BeginDisabled(source);
    if (ImGui::Combo("Color by", &tint, tints, IM_ARRAYSIZE(tints))) scene.lodTint = static_cast<FixedLodTint>(tint);
    ImGui::EndDisabled();

    constexpr std::array shadowLevels{matchCameraShadowLod, static_cast<int>(fixedLodRimLevel),
        static_cast<int>(fixedLodFarLevel), static_cast<int>(fixedLodTwoTriangleLevel)};
    constexpr const char* shadowLabels[] = {"Match camera cut", "LOD 3", "LOD 4", "LOD 6"};
    const auto shadowItem = std::find(shadowLevels.begin(), shadowLevels.end(), scene.branchShadowLod);
    int shadowChoice = static_cast<int>(shadowItem - shadowLevels.begin());
    if (ImGui::Combo("Shadow LOD", &shadowChoice, shadowLabels, hasProxies ? 4 : 1))
    {
        scene.branchShadowLod = shadowLevels[shadowChoice];
    }
    ImGui::BeginDisabled(scene.branchShadowLod == matchCameraShadowLod);
    ImGui::Checkbox("Off-camera casters only", &scene.shadowLodOffCameraOnly);
    ImGui::SetItemTooltip("Override foliage LOD only on branches whose bounds are outside the camera frustum.");
    ImGui::EndDisabled();
    ImGui::Checkbox("Shadow factor view", &scene.showShadowFactor);
    ImGui::SetItemTooltip("Show the received shadow in grayscale. White is unshadowed.");

    const auto instances = scene.instanceCount();
    const bool multiple = instances > 1;
    const auto sourceTriangles = mesh.indices.size() / 3 * instances;
    std::string cutData;
    if (automatic)
    {
        bool update = ImGui::Checkbox("Freeze cut", &selection.frozen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep each group's selected LOD while you move the camera.");
        ImGui::SameLine();
        ImGui::Checkbox("Indirect draws", &selection.indirectDraws);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Batch the same cut into one draw list per active LOD.");
        if (ImGui::TreeNode("Switch thresholds"))
        {
            ImGui::BeginDisabled(selection.frozen);
            constexpr auto clamp = ImGuiSliderFlags_AlwaysClamp;
            update |= ImGui::SliderFloat("LOD 4 limit", &selection.farPixelLimit, 0.10f, 5.0f, "%.2f px", clamp);
            update |= ImGui::SliderFloat("LOD 6 limit", &selection.proxyPixelLimit, 0.10f, 5.0f, "%.2f px", clamp);
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        if (update) renderer.updateBranchLod(scene, view);

        std::array<unsigned, fixedLodLevelCount> groupCounts{};
        std::size_t triangles = 0;
        std::size_t clusters = 0;
        for (std::size_t i = 0; i < selection.groupLevels.size(); ++i)
        {
            const auto selected = selection.groupLevels[i];
            const auto group = i % lod.groups.size();
            ++groupCounts[selected];
            clusters += lod.groups[group].levels[selected].clusterCount;
            triangles += lod.indicesForGroups(selected, group, group).indexCount / 3;
        }
        const auto nearGroups = groupCounts[fixedLodRimLevel];
        const auto farGroups = groupCounts[fixedLodFarLevel];
        const auto proxyGroups = groupCounts[fixedLodTwoTriangleLevel];
        cutData = std::format("Cut {} | Groups LOD 3: {}, LOD 4: {}, LOD 6: {} | {} tris | {} clusters\n"
                              "Switch limits LOD 4 {:.2f} px | LOD 6 {:.2f} px\n",
                              selection.frozen ? "frozen" : "live", nearGroups, farGroups, proxyGroups,
                              triangles, clusters, selection.farPixelLimit, selection.proxyPixelLimit);
        ImGui::Text("%zu foliage triangles, %zu clusters", triangles, clusters);
        ImGui::TextUnformatted("Groups");
        ImGui::SameLine();
        ImGui::TextColored({0.35f, 0.82f, 0.33f, 1.0f}, "LOD 3: %u", nearGroups);
        ImGui::SameLine();
        ImGui::TextColored({0.25f, 0.75f, 0.94f, 1.0f}, "LOD 4: %u", farGroups);
        ImGui::SameLine();
        ImGui::TextColored({0.78f, 0.48f, 0.94f, 1.0f}, "LOD 6: %u", proxyGroups);
    }
    else if (!source)
    {
        const auto index = static_cast<std::size_t>(scene.branchLod);
        const auto& selected = lod.levels[index];
        const auto triangles = selected.indices.size() / 3 * instances;
        const auto reduction = 100.0 * (sourceTriangles - triangles) / sourceTriangles;
        ImGui::Text("%zu foliage triangles (%.1f%% reduction)", triangles, reduction);
        ImGui::Text("%zu clusters in %zu groups", selected.clusters.size() * instances, lod.groups.size() * instances);
        float error = 0;
        for (const auto& group : lod.groups)
        {
            error = std::max(error, group.levels[index].surfaceError());
        }
        ImGui::Text("Max sampled error %.5f mesh units", error);
    }
    else
    {
        ImGui::Text("%zu foliage triangles", sourceTriangles);
    }

    const auto eye = camera.eye();
    const auto yaw = DirectX::XMConvertToDegrees(camera.yaw);
    const auto pitch = DirectX::XMConvertToDegrees(camera.pitch);
    const auto verticalFov = DirectX::XMConvertToDegrees(renderer.cameraVerticalFov);
    const auto viewport = renderer.viewportSize();
    if (ImGui::TreeNode("Camera diagnostics"))
    {
        ImGui::Text("Orbit distance %.3f scene units", camera.distance);
        ImGui::Text("Yaw %.1f deg  Pitch %.1f deg", yaw, pitch);
        showVector("Eye", eye);
        showVector("Focus", camera.focus);
        ImGui::Text("Vertical FOV %.1f deg  Viewport %.0f x %.0f", verticalFov, viewport.x, viewport.y);
        ImGui::TreePop();
    }

    const bool copy = ImGui::Button("Copy view data");
    const bool groupOpen = ImGui::TreeNode("Group diagnostics");
    if (groupOpen)
    {
        if (multiple)
        {
            int instance = scene.inspectedLodInstance + 1;
            if (ImGui::SliderInt("Branch instance", &instance, 1, static_cast<int>(instances),
                                 "%d", ImGuiSliderFlags_AlwaysClamp))
            {
                scene.inspectedLodInstance = instance - 1;
            }
        }
        ImGui::SliderInt("Group ID", &scene.inspectedLodGroup, 0, static_cast<int>(lod.groups.size() - 1),
                         "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::BeginDisabled(source);
        ImGui::Checkbox(multiple ? "Highlight group across copies" : "Highlight group", &scene.highlightLodGroup);
        ImGui::EndDisabled();
    }
    if (groupOpen || copy)
    {
        const auto instanceIndex = static_cast<std::size_t>(scene.inspectedLodInstance);
        const auto& instance = scene.objects[instanceIndex];
        const auto groupIndex = static_cast<std::size_t>(scene.inspectedLodGroup);
        const auto& group = lod.groups[groupIndex];
        DirectX::XMFLOAT4X4 localToWorld;
        DirectX::XMStoreFloat4x4(&localToWorld, DirectX::XMLoadFloat4x4(&mesh.localToWorld) * instance.placementMatrix());
        if (groupOpen)
        {
            ImGui::Text("%u components", group.componentCount);
            ImGui::TextWrapped("Projected from the sampled surface error at rest.");
        }
        std::string groupData;
        if (copy && multiple)
        {
            groupData = std::format("Branch instance {} | X {:.2f}, Z {:.2f}\n",
                                    instance.id, instance.placement.x, instance.placement.y);
        }
        if (automatic)
        {
            const auto activeLevel = selection.groupLevels[instanceIndex * lod.groups.size() + groupIndex];
            const auto errors = renderer.projectBranchLodErrors(group, localToWorld, view);
            const auto line = std::format("Active LOD {} | Selection errors LOD 4 {:.2f} px, LOD 6 {:.2f} px",
                                          activeLevel, errors.farPixels, errors.proxyPixels);
            if (groupOpen) ImGui::TextWrapped("%s", line.c_str());
            if (copy) groupData += line + '\n';
        }
        for (std::size_t i = 0; i < lod.levels.size(); ++i)
        {
            const float error = group.levels[i].surfaceError();
            const auto& bounds = i < fixedLodIndicesOnlyCount ? group.bounds : group.proxyBounds;
            const float pixels = renderer.projectLodError(error, bounds, localToWorld, view);
            const auto screen = std::isfinite(pixels) ? std::format("{:.2f} px", pixels) : "near plane or behind";
            const auto triangles = lod.indicesForGroups(i, groupIndex, groupIndex).indexCount / 3;
            const float lightTexels = renderer.projectShadowError(error, localToWorld);
            const auto reading = std::format("LOD {}   {} tris   {:.5f} units   {}   {:.2f} shadow texels",
                                             i, triangles, error, screen, lightTexels);
            if (groupOpen) ImGui::TextUnformatted(reading.c_str());
            if (copy) groupData += reading + '\n';
        }
        if (groupOpen) ImGui::TextUnformatted("Sampled rim deviation (mesh units)");
        for (std::size_t i = fixedLodRimLevel; i < lod.levels.size(); ++i)
        {
            if (groupOpen)
            {
                if (i != fixedLodRimLevel && i % 2 == 0) ImGui::SameLine();
                ImGui::Text("LOD %zu %.5f", i, group.levels[i].rimDeviation);
            }
            if (copy)
            {
                if (i == fixedLodRimLevel) groupData += "Sampled rim deviation (mesh units)";
                groupData += std::format(" | LOD {} {:.5f}", i, group.levels[i].rimDeviation);
            }
        }
        if (groupOpen) ImGui::TreePop();
        if (copy)
        {
            const auto text = std::format(
                "{} | {} | {}\n"
                "Orbit distance {:.6f} scene units | Yaw {:.4f} deg | Pitch {:.4f} deg\n"
                "Eye ({:.6f}, {:.6f}, {:.6f}) | Focus ({:.6f}, {:.6f}, {:.6f})\n"
                "Vertical FOV {:.1f} deg | Viewport {:.0f} x {:.0f} | {}x MSAA\n"
                "Sun azimuth {:.2f} deg | Elevation {:.2f} deg | Wind strength {:.2f} ({}, phase {:.4f})\n"
                "Shadow LOD {} | {} | Shadow factor view {}\n"
                "{}Group {} ({} components) | Projected error estimated at rest\n{}",
                scene.name(), levels[level], source ? tints[0] : tints[tint],
                camera.distance, yaw, pitch, eye.x, eye.y, eye.z, camera.focus.x, camera.focus.y, camera.focus.z,
                verticalFov, viewport.x, viewport.y, renderer.msaaEnabled() ? 4 : 1,
                scene.sunAzimuth, scene.sunElevation, scene.windStrength, scene.windPaused ? "paused" : "running",
                windPhase, shadowLabels[shadowChoice], scene.shadowLodOffCameraOnly ? "Off-camera only" : "All casters",
                scene.showShadowFactor ? "on" : "off", cutData, scene.inspectedLodGroup, group.componentCount, groupData);
            ImGui::SetClipboardText(text.c_str());
        }
    }
}

void showMeshIcon()
{
    const float lineHeight = ImGui::GetTextLineHeight();
    const float size = lineHeight * 0.72f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 topLeft(cursor.x, cursor.y + (lineHeight - size) * 0.5f);
    const ImVec2 bottomRight(topLeft.x + size, topLeft.y + size);
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_CheckMark);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRect(topLeft, bottomRight, color);
    draw->AddLine(topLeft, bottomRight, color);
    draw->AddLine(ImVec2(bottomRight.x, topLeft.y),
                  ImVec2(topLeft.x + size * 0.5f, topLeft.y + size * 0.5f), color);
    ImGui::Dummy(ImVec2(size, lineHeight));
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
                                       const std::string& sceneError, bool hasImportedScene,
                                       bool reloadingShaders, const std::string& shaderDiagnostics,
                                       const CameraView& camera, float windPhase)
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
                for (const auto kind : sceneKinds)
                {
                    if (kind == SceneKind::Imported && !hasImportedScene) continue;
                    if (ImGui::Selectable(sceneName(kind), scene.kind == kind))
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
            if (isOakBranchScene(scene.kind) &&
                ImGui::CollapsingHeader("Branch LOD", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Frustum culling", &scene.branchFrustumCulling);
                inspectBranchLod(scene, renderer, camera, windPhase);
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
                if (isOakBranchScene(scene.kind))
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Distant")) actions.camera = CameraPreset::Distant;
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
            if (ImGui::CollapsingHeader("Shaders"))
            {
                ImGui::BeginDisabled(reloadingShaders);
                actions.reloadShaders = ImGui::Button("Reload shaders");
                ImGui::EndDisabled();
                if (reloadingShaders)
                {
                    ImGui::SameLine();
                    ImGui::TextUnformatted("Compiling...");
                }
                if (!shaderDiagnostics.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Copy diagnostics")) ImGui::SetClipboardText(shaderDiagnostics.c_str());
                    if (ImGui::BeginChild("Shader diagnostics", {0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f},
                                          ImGuiChildFlags_Borders))
                    {
                        ImGui::TextWrapped("%s", shaderDiagnostics.c_str());
                    }
                    ImGui::EndChild();
                }
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
                if (timing.gpuCameraMilliseconds && timing.gpuShadowMilliseconds)
                {
                    ImGui::Text("GPU camera %.2f ms, shadows %.2f ms", *timing.gpuCameraMilliseconds,
                                *timing.gpuShadowMilliseconds);
                }
                ImGui::Text("Camera %llu tris, %u draws", static_cast<unsigned long long>(timing.camera.triangles),
                            timing.camera.draws);
                ImGui::Text("Shadows %llu tris, %u draws", static_cast<unsigned long long>(timing.shadow.triangles),
                            timing.shadow.draws);
                if (isOakBranchScene(scene.kind))
                {
                    ImGui::Text("Eligible branches %u/%u camera, %u/%u shadows", timing.camera.visibleInstances,
                                scene.instanceCount(), timing.shadow.visibleInstances, scene.instanceCount());
                    ImGui::Text("Foliage API calls %u camera, %u shadows", timing.camera.foliageCalls,
                                timing.shadow.foliageCalls);
                    ImGui::Text("Shadow foliage %llu tris, %u draws",
                                static_cast<unsigned long long>(timing.shadow.foliageTriangles), timing.shadow.foliageDraws);
                    ImGui::Text("Shadow LOD overridden on %u branches", timing.shadow.shadowLodOverrides);
                }
                ImGui::Text("Draws (scene + HUD) %u", timing.drawCalls);
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
        const auto instances = static_cast<int>(scene.instanceCount());
        if (!ground_ && instances > 1)
        {
            int instance = std::clamp(objectIndex_ + 1, 1, instances);
            ImGui::SliderInt(scene.kind == SceneKind::Grove ? "Tree instance" : "Branch instance", &instance,
                             1, instances, "%d", ImGuiSliderFlags_AlwaysClamp);
            objectIndex_ = instance - 1;
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
        const auto& object = scene.objects[std::min<std::size_t>(objectIndex_, scene.instanceCount() - 1)];
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
        ImGui::Spacing();
        ImGui::SeparatorText("Meshes");
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto& part = slots[i];
            const auto& slot = ground_ ? scene.groundMaterial : scene.asset->materials[part.material];
            ImGui::PushID(static_cast<int>(i));
            const float iconX = ImGui::GetCursorPosX();
            showMeshIcon();
            ImGui::SameLine();
            const float nameOffset = ImGui::GetCursorPosX() - iconX;
            if (ImGui::Selectable(part.name.c_str(), ground_ || i == meshIndex_))
            {
                if (!ground_) meshIndex_ = i;
                page_ = Page::Mesh;
            }
            ImGui::Indent(nameOffset);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Material: %s", slot.name.c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent(nameOffset);
            ImGui::Spacing();
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
        if (!ground_ && scene.instanceCount() > 1)
        {
            ImGui::TextDisabled("Applies to this mesh on every instance.");
        }
        ImGui::Text("Vertices  %zu   Triangles  %zu", mesh.vertices.size(), mesh.indices.size() / 3);
        ImGui::Text("UV sets  %s", mesh.uvMask == 3 ? "0, 1" : mesh.uvMask == 1 ? "0" : mesh.uvMask == 2 ? "1" : "None");
        showVector("Local center", mesh.localBounds.center);
        showVector("Half extent", mesh.localBounds.halfExtent);
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
        }
        if (material.hasBacklighting)
        {
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
