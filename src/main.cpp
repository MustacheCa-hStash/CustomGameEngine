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
    XMFLOAT3 focus{0.0f, 0.75f, 0.0f};
    POINT lastMouse{};
    bool dragging = false;

    void useView(const CameraView& view, float overviewDistance)
    {
        focus = view.focus;
        yaw = view.yaw;
        pitch = view.pitch;
        distance = view.distance;
        homeDistance = overviewDistance;
    }

    void zoom(float steps)
    {
        const float scale = homeDistance / defaultDistance;
        distance = std::clamp(distance * std::pow(0.85f, steps), 1.5f * scale, 45.0f * scale);
    }

    XMFLOAT4X4 view() const
    {
        const float horizontal = distance * std::cos(pitch);
        const auto target = XMLoadFloat3(&focus);
        const auto eye = target + XMVectorSet(horizontal * std::sin(yaw),
                                             distance * std::sin(pitch),
                                             horizontal * std::cos(yaw), 0.0f);
        XMFLOAT4X4 result;
        XMStoreFloat4x4(&result, XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0)));
        return result;
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

struct LaunchOptions
{
    SceneKind kind = SceneKind::Cube;
    std::filesystem::path imported;
    bool resetSettings = false;
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
    if (count > 2)
    {
        throw std::runtime_error("Usage: CustomGameEngine.exe [--oak | --modeled-oak | --grove | --branch | path-to-mesh.glb | --reset-settings]");
    }
    if (count == 2)
    {
        const std::wstring_view option = arguments.get()[1];
        if (option == L"--grove") return {SceneKind::Grove, {}};
        if (option == L"--oak") return {SceneKind::Oak, {}};
        if (option == L"--modeled-oak") return {SceneKind::ModeledOak, {}};
        if (option == L"--branch") return {SceneKind::OakBranch, {}};
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
        std::string sceneError;
        std::future<ShaderCompilation> shaderReload;
        std::string shaderDiagnostics;

        const auto applyView = [&](CameraPreset preset) {
            routeStart.reset();
            camera.useView(scene->cameraView(preset), scene->cameraView(CameraPreset::Overview).distance);
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

            const auto& inspectorError = sceneError.empty() ? settings.error() : sceneError;
            const auto actions = inspector.frame(*scene, renderer, timing, inspectorError,
                                                 launch.kind == SceneKind::Imported, shaderReload.valid(),
                                                 shaderDiagnostics);
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
            if (actions.camera)
            {
                applyView(*actions.camera);
                refreshTitle();
            }

            auto view = camera.view();
            if (routeStart)
            {
                constexpr double routeSeconds = 24.0;
                const double elapsed = std::chrono::duration<double>(Clock::now() - *routeStart).count();
                const float angle = static_cast<float>(std::fmod(elapsed, routeSeconds) *
                                                       (XM_2PI / routeSeconds));
                auto routeCamera = camera;
                routeCamera.yaw = OrbitCamera::defaultYaw + angle;
                routeCamera.pitch = 0.42f + 0.15f * std::sin(angle);
                routeCamera.distance = 80.0f + 32.0f * std::cos(angle);
                view = routeCamera.view();
            }
            timing = renderer.drawFrame(*scene, view, wind.phase(), hud.text(),
                [&](ID3D12GraphicsCommandList* commands) { inspector.draw(commands); });
            hud.record(timing);

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

