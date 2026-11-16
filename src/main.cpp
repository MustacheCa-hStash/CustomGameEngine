#include "BranchBenchmark.h"
#include "D3D12Renderer.h"
#include "SceneInspector.h"
#include "ViewerSettings.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

using namespace DirectX;

namespace
{
constexpr wchar_t windowClassName[] = L"CustomGameEngineWindow";
constexpr DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
using Clock = std::chrono::steady_clock;

class PerformanceHud
{
public:
    std::string_view text() const { return text_; }

    void reset()
    {
        frameStart_ = Clock::now();
        elapsed_ = cpu_ = gpu_ = 0.0;
        frames_ = gpuSamples_ = 0;
        text_ = " --.- fps  cpu  -.-- ms  gpu  -.-- ms";
    }

    void record(D3D12Renderer::FrameTiming timing)
    {
        const auto now = Clock::now();
        elapsed_ += std::chrono::duration<double>(now - frameStart_).count();
        frameStart_ = now;
        ++frames_;
        cpu_ += timing.cpuMilliseconds;
        if (timing.gpuMilliseconds)
        {
            gpu_ += *timing.gpuMilliseconds;
            ++gpuSamples_;
        }
        if (elapsed_ >= 0.5)
        {
            const auto gpuText = gpuSamples_ ? std::format("{:5.2f}", gpu_ / gpuSamples_) : " -.--";
            text_ = std::format("{:5.1f} fps  cpu {:5.2f} ms  gpu {} ms",
                                frames_ / elapsed_, cpu_ / frames_, gpuText);
            elapsed_ = cpu_ = gpu_ = 0.0;
            frames_ = gpuSamples_ = 0;
        }
    }

private:
    Clock::time_point frameStart_ = Clock::now();
    double elapsed_ = 0.0;
    double cpu_ = 0.0;
    double gpu_ = 0.0;
    unsigned frames_ = 0;
    unsigned gpuSamples_ = 0;
    std::string text_ = " --.- fps  cpu  -.-- ms  gpu  -.-- ms";
};

class WindClock
{
public:
    bool paused() const { return paused_; }

    void togglePause()
    {
        const auto now = Clock::now();
        if (!paused_)
        {
            elapsed_ += now - started_;
        }
        started_ = now;
        paused_ = !paused_;
    }

    float phase() const
    {
        const auto time = paused_ ? elapsed_ : elapsed_ + (Clock::now() - started_);
        const double seconds = std::chrono::duration<double>(time).count();
        constexpr double cycleSeconds = 6.0;
        return static_cast<float>(std::fmod(seconds, cycleSeconds) * (XM_2PI / cycleSeconds));
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point started_ = Clock::now();
    Clock::duration elapsed_{};
    bool paused_ = false;
};

struct OrbitCamera
{
    static constexpr float defaultYaw = -0.28f;
    static constexpr float defaultPitch = 0.4f;
    static constexpr float defaultDistance = 5.4f;

    float yaw = defaultYaw;
    float pitch = defaultPitch;
    float distance = defaultDistance;
    float homeDistance = defaultDistance;
    float zoomLimit = 45.0f;
    XMFLOAT3 focus{0.0f, 0.75f, 0.0f};
    POINT lastMouse{};
    bool dragging = false;

    void useView(const CameraView& view, float overviewDistance, float distantDistance)
    {
        focus = view.focus;
        yaw = view.yaw;
        pitch = view.pitch;
        distance = view.distance;
        homeDistance = overviewDistance;
        zoomLimit = std::max(45.0f * homeDistance / defaultDistance, distantDistance);
    }

    void setDistance(float requested)
    {
        const float scale = homeDistance / defaultDistance;
        if (std::isfinite(requested)) distance = std::clamp(requested, 1.5f * scale, zoomLimit);
    }

    void zoom(float steps) { setDistance(distance * std::pow(0.85f, steps)); }

    CameraView currentView() const
    {
        return {focus, yaw, pitch, distance};
    }
};

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }

    auto* camera = reinterpret_cast<OrbitCamera*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    // The ImGui Win32 backend acquires and releases capture for the shared HWND,
    // including viewport presses. WantCaptureMouse only routes the gesture.
    const bool uiHandled = SceneInspector::handleMessage(window, message, wParam, lParam);
    if (camera)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
            camera->dragging = !uiHandled && GetCapture() == window;
            if (camera->dragging)
            {
                camera->lastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            camera->dragging = false;
            return 0;
        case WM_MOUSEMOVE:
            if (camera->dragging && GetCapture() == window && (wParam & MK_LBUTTON))
            {
                const POINT mouse{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const float dx = static_cast<float>(mouse.x - camera->lastMouse.x);
                const float dy = static_cast<float>(mouse.y - camera->lastMouse.y);
                camera->yaw = std::remainder(camera->yaw - dx * 0.008f, XM_2PI);
                camera->pitch = std::clamp(camera->pitch + dy * 0.008f, -1.45f, 1.45f);
                camera->lastMouse = mouse;
            }
            return 0;
        case WM_MOUSEWHEEL:
        {
            if (!uiHandled || camera->dragging)
            {
                const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
                camera->zoom(steps);
            }
            return 0;
        }
        }
    }

    if (uiHandled)
    {
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND createWindow(HINSTANCE instance, OrbitCamera& camera)
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = windowClassName;
    if (!RegisterClassW(&windowClass))
    {
        throw std::runtime_error("Could not register the viewer window class.");
    }

    RECT bounds{0, 0, 3200, 1800};
    if (!AdjustWindowRect(&bounds, windowStyle, FALSE))
    {
        throw std::runtime_error("Could not calculate the viewer window size.");
    }
    HWND window = CreateWindowExW(0, windowClassName, L"Custom Game Engine", windowStyle,
                                  CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top, nullptr, nullptr, instance, &camera);
    if (!window)
    {
        throw std::runtime_error("Could not create the viewer window.");
    }
    return window;
}

void updateWindowTitle(HWND window, const D3D12Renderer& renderer, const ViewerScene& scene,
                       bool routeActive)
{
    std::wstring title = L"Custom Game Engine | MSAA 4x unavailable";
    if (renderer.supportsMsaa())
    {
        title = renderer.msaaEnabled() ? L"Custom Game Engine | MSAA 4x (M)"
                                       : L"Custom Game Engine | MSAA off (M)";
    }
    if (scene.foliage)
    {
        title += scene.windPaused ? L" | Wind paused (P)" : L" | Wind running (P)";
    }
    if (scene.kind == SceneKind::Grove)
    {
        title += std::format(L" | {} trees (1-4) | Route {} (B)", scene.treeCount,
                             routeActive ? L"on" : L"off");
    }
    title += L" | Inspector (F1)";
    SetWindowTextW(window, title.c_str());
}

enum class BranchBenchmarkKind { Triangle, Shadow, Voxel, VoxelNormals, VoxelBounds, Lighting, Transition, Region, OakWalk, OakWalkCull,
                                 SunCascades, SunCascadeStability, SunCascadeTransition, SunCascadeCasters };

struct LaunchOptions
{
    SceneKind kind = SceneKind::Cube;
    std::filesystem::path imported;
    bool resetSettings = false;
    std::filesystem::path benchmarkReport;
    BranchBenchmarkKind benchmark = BranchBenchmarkKind::Triangle;
    std::array<std::filesystem::path, 2> farVoxelBakes;
    VoxelShadingComparison voxelShading = VoxelShadingComparison::None;
    float voxelEntryPixels = branchVoxelDefaultEntryPixels;
    bool compareVoxelShadowMidpoint = false;
    bool compareVoxelShadowCasters = false;
    unsigned pilotRegion = 0;
    std::filesystem::path regionCandidate;
    bool stabilizedCascadeBenchmark = false;
};

LaunchOptions launchOptions()
{
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &count), LocalFree);
    if (!arguments)
    {
        throw std::runtime_error("Could not read the command line.");
    }
    if (count > 1)
    {
        const std::wstring_view option = arguments.get()[1];
        if (option == L"--sun-cascade-caster-benchmark")
        {
            if (count != 3 || !arguments.get()[2][0]) throw std::runtime_error("Supply a CSV output path.");
            return {SceneKind::ModeledOakWalk, {}, false, arguments.get()[2], BranchBenchmarkKind::SunCascadeCasters};
        }
        if (option == L"--sun-cascade-stability-benchmark" || option == L"--sun-cascade-transition-benchmark")
        {
            if (count != 3 || !arguments.get()[2][0]) throw std::runtime_error("Supply a CSV output path.");
            const auto kind = option == L"--sun-cascade-stability-benchmark" ?
                BranchBenchmarkKind::SunCascadeStability : BranchBenchmarkKind::SunCascadeTransition;
            return {SceneKind::ModeledOakWalk, {}, false, arguments.get()[2], kind};
        }
        if (option == L"--sun-cascade-benchmark")
        {
            const bool stabilized = count == 4 && std::wstring_view(arguments.get()[3]) == L"--stable";
            if ((count != 3 && !stabilized) || !arguments.get()[2][0])
                throw std::runtime_error("Supply a CSV output path and optional --stable.");
            LaunchOptions launch{SceneKind::ModeledOakWalk, {}, false, arguments.get()[2], BranchBenchmarkKind::SunCascades};
            launch.stabilizedCascadeBenchmark = stabilized;
            return launch;
        }
        if (option == L"--oak-walk-benchmark" || option == L"--oak-walk-cull-benchmark")
        {
            if (count != 3 || !arguments.get()[2][0]) throw std::runtime_error("Supply a CSV output path.");
            const auto kind = option == L"--oak-walk-benchmark" ? BranchBenchmarkKind::OakWalk : BranchBenchmarkKind::OakWalkCull;
            return {SceneKind::ModeledOakWalk, {}, false, arguments.get()[2], kind};
        }
        if (option == L"--branch-voxel-bounds-benchmark")
        {
            if (count != 3 || !arguments.get()[2][0]) throw std::runtime_error("Supply a CSV output path.");
            return {SceneKind::OakBranch, {}, false, arguments.get()[2], BranchBenchmarkKind::VoxelBounds};
        }
        if (option == L"--branch-transition-benchmark")
        {
            if ((count != 3 && count != 4) || !arguments.get()[2][0])
                throw std::runtime_error("Supply a CSV output path and optional voxel entry limit in pixels.");
            LaunchOptions launch{SceneKind::OakBranchTransition, {}, false, arguments.get()[2], BranchBenchmarkKind::Transition};
            if (count == 4)
            {
                const std::wstring value = arguments.get()[3];
                std::size_t consumed = 0;
                launch.voxelEntryPixels = std::stof(value, &consumed);
                if (consumed != value.size() || !(launch.voxelEntryPixels >= 0.1f && launch.voxelEntryPixels <= 1.0f))
                    throw std::runtime_error("Voxel entry limit must be 0.1 to 1.0 pixels.");
            }
            return launch;
        }
        if (option == L"--branch-region-benchmark")
        {
            if ((count != 4 && count != 5) || !arguments.get()[2][0])
                throw std::runtime_error("Supply a CSV output path, region index, and optional region voxel bake.");
            const std::wstring_view region = arguments.get()[3];
            if (region.size() != 1 || region.front() < L'0' ||
                region.front() >= L'0' + static_cast<wchar_t>(voxelBranchRegionCount))
                throw std::runtime_error("The region index is out of range.");
            LaunchOptions launch{SceneKind::OakBranchTransition, {}, false, arguments.get()[2], BranchBenchmarkKind::Region};
            launch.pilotRegion = static_cast<unsigned>(region.front() - L'0');
            if (count == 5) launch.regionCandidate = arguments.get()[4];
            return launch;
        }
        if (option == L"--branch-voxel-normal-benchmark" || option == L"--branch-lighting-diagnostics")
        {
            const bool directional = count == 4 && std::wstring_view(arguments.get()[3]) == L"--directional";
            const auto kind = option == L"--branch-lighting-diagnostics" ? BranchBenchmarkKind::Lighting
                                                                       : BranchBenchmarkKind::VoxelNormals;
            const bool shadowReceiver = kind == BranchBenchmarkKind::VoxelNormals && count == 4 &&
                                        std::wstring_view(arguments.get()[3]) == L"--shadow-receiver";
            const bool midpoint = kind == BranchBenchmarkKind::Lighting && count == 4 &&
                                  std::wstring_view(arguments.get()[3]) == L"--midpoint";
            const bool casterMatrix = kind == BranchBenchmarkKind::Lighting && count == 4 &&
                                      std::wstring_view(arguments.get()[3]) == L"--caster-matrix";
            if ((count != 3 && !directional && !shadowReceiver && !midpoint && !casterMatrix) || !arguments.get()[2][0])
                throw std::runtime_error(kind == BranchBenchmarkKind::Lighting
                    ? "Supply a CSV output path, optionally followed by --directional, --midpoint, or --caster-matrix."
                    : "Supply a CSV output path, optionally followed by --directional or --shadow-receiver.");
            const auto shading = shadowReceiver ? VoxelShadingComparison::ShadowReceiver :
                (directional ? VoxelShadingComparison::DirectionalNormals : VoxelShadingComparison::PooledNormals);
            LaunchOptions launch{SceneKind::OakBranch, {}, false, arguments.get()[2], kind, {}, shading};
            launch.compareVoxelShadowMidpoint = midpoint || casterMatrix;
            launch.compareVoxelShadowCasters = casterMatrix;
            return launch;
        }
        if (option == L"--branch-benchmark" || option == L"--branch-shadow-benchmark" || option == L"--branch-voxel-benchmark")
        {
            const bool farVoxelRoute = option == L"--branch-voxel-benchmark" && count == 5;
            if ((count != 3 && !farVoxelRoute) || arguments.get()[2][0] == L'\0')
            {
                throw std::runtime_error("Supply a CSV output path. The voxel benchmark also accepts two voxel bakes for the far route.");
            }
            const auto benchmark = option == L"--branch-shadow-benchmark" ? BranchBenchmarkKind::Shadow :
                (option == L"--branch-voxel-benchmark" ? BranchBenchmarkKind::Voxel : BranchBenchmarkKind::Triangle);
            LaunchOptions launch{benchmark == BranchBenchmarkKind::Shadow ? SceneKind::OakBranchGrid : SceneKind::OakBranch,
                                 {}, false, arguments.get()[2], benchmark};
            if (farVoxelRoute)
            {
                if (!arguments.get()[3][0] || !arguments.get()[4][0])
                    throw std::runtime_error("Both far voxel bake paths must be nonempty.");
                launch.farVoxelBakes = {arguments.get()[3], arguments.get()[4]};
            }
            return launch;
        }
    }
    if (count > 2)
    {
        throw std::runtime_error("Usage: CustomGameEngine.exe [--oak | --modeled-oak | --grove | --branch | --branch-grid | --branch-orbit | --branch-benchmark report.csv | --branch-shadow-benchmark report.csv | --branch-voxel-benchmark report.csv | path-to-mesh.glb | --reset-settings]");
    }
    if (count == 2)
    {
        const std::wstring_view option = arguments.get()[1];
        if (option == L"--grove") return {SceneKind::Grove, {}};
        if (option == L"--oak") return {SceneKind::Oak, {}};
        if (option == L"--modeled-oak") return {SceneKind::ModeledOak, {}};
        if (option == L"--oak-walk") return {SceneKind::ModeledOakWalk, {}};
        if (option == L"--branch") return {SceneKind::OakBranch, {}};
        if (option == L"--branch-grid") return {SceneKind::OakBranchGrid, {}};
        if (option == L"--branch-orbit") return {SceneKind::OakBranchOrbit, {}};
        if (option == L"--branch-transition") return {SceneKind::OakBranchTransition, {}};
        if (option == L"--reset-settings") return {SceneKind::Cube, {}, true};
        return {SceneKind::Imported, arguments.get()[1]};
    }
    return {};
}

std::filesystem::path assetDirectory()
{
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
    {
        throw std::runtime_error("Could not locate the viewer's bundled assets.");
    }
    executable.resize(length);
    return std::filesystem::path(executable).parent_path() / L"assets";
}

void showError(const char* message, const std::filesystem::path& assetPath)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
    std::wstring text = L"The viewer could not continue.";
    if (length > 0)
    {
        text.resize(static_cast<std::size_t>(length));
        MultiByteToWideChar(CP_UTF8, 0, message, -1, text.data(), length);
        text.pop_back();
    }
    if (!assetPath.empty())
    {
        text += L"\n\nAsset: " + assetPath.native();
    }
    MessageBoxW(nullptr, text.c_str(), L"Custom Game Engine - Error", MB_OK | MB_ICONERROR);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    std::filesystem::path assetPath;
    OrbitCamera camera;
    HWND window = nullptr;
    try
    {
        const auto launch = launchOptions();
        ViewerSettings settings(std::filesystem::path(VIEWER_PROJECT_DIRECTORY) / L"viewer-settings.ini");
        if (launch.resetSettings)
        {
            settings.reset();
            if (!settings.save()) throw std::runtime_error(settings.error());
        }
        const auto directory = assetDirectory();
        ViewerScenes scenes(directory);
        assetPath = scenes.sourcePath(launch.kind, launch.imported);
        auto* scene = &scenes.open(launch.kind, launch.imported);
        if (!launch.benchmarkReport.empty())
        {
            window = createWindow(instance, camera);
            D3D12Renderer renderer(window);
            if (renderer.msaaEnabled()) renderer.toggleMsaa();
            ShowWindow(window, showCommand);
            if (launch.benchmark == BranchBenchmarkKind::Shadow) return runBranchShadowBenchmark(window, *scene, renderer, launch.benchmarkReport);
            if (launch.benchmark == BranchBenchmarkKind::Lighting)
                return runBranchLightingDiagnostics(*scene, renderer, launch.benchmarkReport,
                                                    launch.voxelShading == VoxelShadingComparison::DirectionalNormals,
                                                    launch.compareVoxelShadowMidpoint, launch.compareVoxelShadowCasters);
            if (launch.benchmark == BranchBenchmarkKind::Transition)
                return runBranchTransitionBenchmark(*scene, renderer, launch.benchmarkReport, launch.voxelEntryPixels);
            if (launch.benchmark == BranchBenchmarkKind::Region)
                return runBranchRegionBenchmark(*scene, renderer, launch.benchmarkReport, launch.pilotRegion, launch.regionCandidate);
            if (launch.benchmark == BranchBenchmarkKind::OakWalk)
                return runOakWalkBenchmark(*scene, renderer, launch.benchmarkReport);
            if (launch.benchmark == BranchBenchmarkKind::OakWalkCull)
                return runOakWalkCullBenchmark(*scene, renderer, launch.benchmarkReport);
            if (launch.benchmark == BranchBenchmarkKind::SunCascades)
                return runSunCascadeBenchmark(*scene, renderer, launch.benchmarkReport, launch.stabilizedCascadeBenchmark);
            if (launch.benchmark == BranchBenchmarkKind::SunCascadeStability)
                return runSunCascadeStabilityBenchmark(*scene, renderer, launch.benchmarkReport);
            if (launch.benchmark == BranchBenchmarkKind::SunCascadeTransition)
                return runSunCascadeTransitionBenchmark(*scene, renderer, launch.benchmarkReport);
            if (launch.benchmark == BranchBenchmarkKind::SunCascadeCasters)
                return runSunCascadeCasterBenchmark(*scene, renderer, launch.benchmarkReport);
            if (launch.benchmark == BranchBenchmarkKind::Voxel || launch.benchmark == BranchBenchmarkKind::VoxelNormals ||
                launch.benchmark == BranchBenchmarkKind::VoxelBounds)
                return runBranchVoxelBenchmark(window, *scene, renderer, launch.benchmarkReport, launch.farVoxelBakes,
                                               launch.voxelShading, launch.benchmark == BranchBenchmarkKind::VoxelBounds);
            return runBranchBenchmark(window, *scene, renderer, launch.benchmarkReport);
        }
        settings.apply(*scene);
        window = createWindow(instance, camera);
        D3D12Renderer renderer(window);
        if (!settings.msaaEnabled()) renderer.toggleMsaa();
        renderer.setScene(*scene);
        SceneInspector inspector(window, renderer);
        PerformanceHud hud;
        WindClock wind;
        if (scene->windPaused) wind.togglePause();
        D3D12Renderer::FrameTiming timing{};
        std::optional<Clock::time_point> routeStart;
        std::optional<Clock::time_point> branchOrbitStart;
        float orbitSeconds = 0.0f;
        std::string sceneError;
        std::future<ShaderCompilation> shaderReload;
        std::string shaderDiagnostics;

        const auto useCameraView = [&](const CameraView& view) {
            routeStart.reset();
            branchOrbitStart.reset();
            orbitSeconds = 0.0f;
            const float distant = isOakBranchScene(scene->kind) ? branchVoxelFarDistances.back() : 0.0f;
            camera.useView(view, scene->cameraView(CameraPreset::Overview).distance, distant);
        };
        const auto applyView = [&](CameraPreset preset) {
            if (scene->kind == SceneKind::ModeledOakWalk) useCameraView(oakWalkView(scene->oakWalkRoute, 0.0f));
            else useCameraView(isBranchOrbitScene(scene->kind)
                ? scene->branchOrbitView(0.0f, branchOrbitDistance) : scene->cameraView(preset));
        };
        const auto orbitDuration = [&] {
            return scene->kind == SceneKind::ModeledOakWalk ? oakWalkDuration(scene->oakWalkRoute) : branchOrbitDuration;
        };
        const auto toggleOrbit = [&] {
            if (branchOrbitStart)
            {
                branchOrbitStart.reset();
                return;
            }
            if (orbitSeconds >= orbitDuration()) orbitSeconds = 0.0f;
            branchOrbitStart = Clock::now() - std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<float>(orbitSeconds));
        };
        const auto refreshTitle = [&] {
            updateWindowTitle(window, renderer, *scene, routeStart.has_value());
        };
        const auto toggleWind = [&] {
            wind.togglePause();
            scene->windPaused = wind.paused();
            settings.recordScene(*scene);
            refreshTitle();
        };
        const auto treeCountChanged = [&] {
            settings.recordScene(*scene);
            hud.reset();
            if (routeStart) routeStart = Clock::now();
            refreshTitle();
        };
        const auto toggleMsaa = [&] {
            if (!renderer.supportsMsaa()) return;
            renderer.toggleMsaa();
            settings.recordMsaa(renderer.msaaEnabled());
            hud.reset();
            refreshTitle();
        };

        applyView(CameraPreset::Overview);
        refreshTitle();
        ShowWindow(window, showCommand);

        for (;;)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    if (!settings.save())
                    {
                        showError(settings.error().c_str(), {});
                        return 1;
                    }
                    return static_cast<int>(message.wParam);
                }
                // Bit 30 is set on auto-repeat. Toggle once per physical press.
                const bool initialKeyDown = message.hwnd == window && message.message == WM_KEYDOWN &&
                                            (message.lParam & (1L << 30)) == 0;
                if (initialKeyDown && message.wParam == VK_F1)
                {
                    inspector.toggle();
                    continue;
                }
                if (initialKeyDown && !SceneInspector::wantsKeyboard())
                {
                    const bool grove = scene->kind == SceneKind::Grove;
                    if ((isBranchOrbitScene(scene->kind) || scene->kind == SceneKind::ModeledOakWalk) && message.wParam == 'B')
                    {
                        toggleOrbit();
                        continue;
                    }
                    if (message.wParam == 'M')
                    {
                        toggleMsaa();
                        continue;
                    }
                    if (message.wParam == 'P' && scene->foliage)
                    {
                        toggleWind();
                        continue;
                    }
                    if (grove && message.wParam >= '1' && message.wParam <= '4')
                    {
                        scene->treeCount = groveCounts[message.wParam - '1'];
                        treeCountChanged();
                        continue;
                    }
                    if (message.wParam == 'R' || (grove && message.wParam == 'B'))
                    {
                        const bool startRoute = message.wParam == 'B' && !routeStart;
                        applyView(CameraPreset::Overview);
                        if (startRoute) routeStart = Clock::now();
                        hud.reset();
                        refreshTitle();
                        continue;
                    }
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if (IsIconic(window))
            {
                branchOrbitStart.reset();
                settings.save();
                WaitMessage();
                hud.reset();
                continue;
            }

            if (shaderReload.valid() && shaderReload.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                std::string compilerLog;
                try
                {
                    auto result = shaderReload.get();
                    compilerLog = std::move(result.diagnostics);
                    if (result.shaders)
                    {
                        renderer.reloadMeshShaders(std::move(*result.shaders), scene->shadowBias);
                        shaderDiagnostics = renderer.supportsMsaa()
                            ? "Reloaded camera (single-sample and 4x MSAA) and shadow pipelines.\n\n"
                            : "Reloaded single-sample camera and shadow pipelines.\n\n";
                        shaderDiagnostics += compilerLog;
                    }
                    else
                    {
                        shaderDiagnostics = "Compilation failed. Continuing with the active shaders.\n\n" + compilerLog;
                    }
                }
                catch (const std::exception& error)
                {
                    shaderDiagnostics = std::format("Reload failed. Continuing with the active shaders.\n{}\n\n{}",
                                                    error.what(), compilerLog);
                }
            }

            auto view = camera.currentView();
            if (isBranchOrbitScene(scene->kind) || scene->kind == SceneKind::ModeledOakWalk)
            {
                if (branchOrbitStart)
                {
                    const auto elapsed = std::chrono::duration<float>(Clock::now() - *branchOrbitStart).count();
                    orbitSeconds = std::min(elapsed, orbitDuration());
                    if (orbitSeconds == orbitDuration()) branchOrbitStart.reset();
                }
                view = scene->kind == SceneKind::ModeledOakWalk ? oakWalkView(scene->oakWalkRoute, orbitSeconds) :
                                                               scene->branchOrbitView(orbitSeconds, camera.distance);
            }
            else if (routeStart)
            {
                constexpr double routeSeconds = 24.0;
                const double elapsed = std::chrono::duration<double>(Clock::now() - *routeStart).count();
                const float angle = static_cast<float>(std::fmod(elapsed, routeSeconds) *
                                                       (XM_2PI / routeSeconds));
                view.yaw = OrbitCamera::defaultYaw + angle;
                view.pitch = 0.42f + 0.15f * std::sin(angle);
                view.distance = 80.0f + 32.0f * std::cos(angle);
            }
            if (isOakBranchScene(scene->kind))
            {
                const bool farView = view.distance >= branchVoxelFarDistances.front();
                renderer.updateCameraProjection(farView ? branchVoxelFarNearPlane : scene->nearPlane,
                                                farView ? branchVoxelFarClipPlane : scene->farPlane());
            }
            const auto viewMatrix = view.matrix();
            renderer.updateBranchLod(*scene, viewMatrix);
            const auto& inspectorError = sceneError.empty() ? settings.error() : sceneError;
            const auto actions = inspector.frame(*scene, renderer, timing, inspectorError,
                                                 launch.kind == SceneKind::Imported, shaderReload.valid(),
                                                 shaderDiagnostics, view, wind.phase(), orbitSeconds, branchOrbitStart.has_value());
            if (actions.reloadShaders)
            {
                try
                {
                    shaderReload = std::async(std::launch::async, compileMeshShaders);
                    shaderDiagnostics.clear();
                }
                catch (const std::exception& error)
                {
                    shaderDiagnostics = std::format("Could not start shader reload. {}", error.what());
                }
            }
            if (actions.sunChanged) renderer.updateSun(*scene);
            if (actions.toggleWind) toggleWind();
            if (actions.toggleMsaa) toggleMsaa();
            if (actions.treeCountChanged) treeCountChanged();
            if (actions.settingsChanged || actions.sunChanged) settings.recordScene(*scene);
            settings.saveIfDue();
            timing = renderer.drawFrame(*scene, viewMatrix, wind.phase(), hud.text(),
                [&](ID3D12GraphicsCommandList* commands) { inspector.draw(commands); });
            hud.record(timing);
            if (actions.transitionGridChanged)
            {
                scene->setBranchGrid(scene->instanceCount() == 1 ? branchGridSide : 1);
                renderer.setScene(*scene);
            }
            if (actions.orbitDistance) camera.setDistance(*actions.orbitDistance);
            if (actions.orbitSeek)
            {
                orbitSeconds = *actions.orbitSeek;
                branchOrbitStart.reset();
            }
            else if (actions.restartOrbit)
            {
                orbitSeconds = 0.0f;
                branchOrbitStart = Clock::now();
            }
            else if (actions.toggleOrbit)
            {
                toggleOrbit();
            }
            if (actions.comparisonCamera)
            {
                useCameraView(*actions.comparisonCamera);
                refreshTitle();
            }
            else if (actions.camera)
            {
                applyView(*actions.camera);
                refreshTitle();
            }

            // Submit the current UI before switching the assets its texture preview references.
            if (actions.scene && *actions.scene != scene->kind)
            {
                ViewerScene* next = nullptr;
                try
                {
                    next = &scenes.open(*actions.scene, launch.imported);
                    settings.apply(*next);
                }
                catch (const std::exception& error)
                {
                    sceneError = std::format("Could not open scene. {}", error.what());
                }
                if (next)
                {
                    assetPath = next->source;
                    renderer.setScene(*next);
                    scene = next;
                    sceneError.clear();
                    wind = {};
                    if (scene->windPaused) wind.togglePause();
                    inspector.resetSelection();
                    applyView(CameraPreset::Overview);
                    hud.reset();
                    refreshTitle();
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        if (window)
        {
            DestroyWindow(window);
        }
        showError(error.what(), assetPath);
        return 1;
    }
}

