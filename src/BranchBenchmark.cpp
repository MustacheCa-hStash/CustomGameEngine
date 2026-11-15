#include "BranchBenchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

using namespace DirectX;

namespace
{
using Clock = std::chrono::steady_clock;
constexpr unsigned warmupFrames = 12;
constexpr unsigned routeSamples = 8;
constexpr unsigned repetitions = 4;

struct Segment
{
    std::string_view name;
    float fromDistance;
    float toDistance;
    bool compareProxy = false;
};

constexpr Segment segments[]{{"near", 12.0f, 20.0f}, {"mixed", 26.0f, 54.0f}, {"far", 100.0f, 300.0f, true}};

struct Mode
{
    std::string_view name;
    int level;
};

constexpr Mode modes[]{{"source", sourceBranchLod}, {"lod3", static_cast<int>(fixedLodRimLevel)},
                       {"auto", automaticBranchLod}, {"lod6", static_cast<int>(fixedLodTwoTriangleLevel)}};

bool processMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

std::string csvQuoted(const std::wstring& value)
{
    if (value.empty()) return "\"\"";
    const int length = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, utf8.data(), bytes, nullptr, nullptr);
    std::string escaped = "\"";
    for (char ch : utf8)
    {
        if (ch == '"') escaped += '"';
        escaped += ch;
    }
    return escaped + '"';
}

void writePass(std::ostream& out, const D3D12Renderer::PassStats& pass)
{
    out << ',' << pass.draws << ',' << pass.triangles << ',' << pass.foliageDraws << ',' << pass.foliageTriangles
        << ',' << pass.foliageCalls;
}
}

int runBranchBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                       const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
    {
        throw std::runtime_error("The benchmark requires the complete Oak Branch bake. Rebuild the viewer.");
    }

    scene.sunAzimuth = -100.0f;
    scene.sunElevation = 54.5715f;
    scene.windStrength = 1.0f;
    constexpr float windPhase = 0.0f;
    constexpr float pitch = 67.8383f;
    const auto viewport = renderer.viewportSize();
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const auto sourceFingerprint = fixedLodSourceFingerprint(scene.source);
    const auto bakeFingerprint = fixedLodSourceFingerprint(fixedLodPath(scene.source));
#ifdef _DEBUG
    constexpr auto build = "debug";
#else
    constexpr auto build = "release";
#endif
    std::ostringstream environment;
    environment.imbue(std::locale::classic());
    environment << "3," << build << ',' << csvQuoted(renderer.adapterName()) << ',' << std::hex << sourceFingerprint
                << ',' << bakeFingerprint
                << std::dec << ',' << viewport.x << ',' << viewport.y << ',' << XMConvertToDegrees(renderer.cameraVerticalFov)
                << ',' << (renderer.msaaEnabled() ? 4 : 1) << ',' << scene.sunAzimuth << ',' << scene.sunElevation
                << ',' << scene.windStrength << ',' << windPhase << ',' << scene.branchLodSelection.farPixelLimit
                << ',' << scene.branchLodSelection.proxyPixelLimit << ',';
    const auto settings = environment.str();

    std::ostringstream rows;
    rows.imbue(std::locale::classic());
    rows << "schema,build,adapter,source_fingerprint,bake_fingerprint,viewport_width,viewport_height,fov_y_deg,msaa,"
            "sun_azimuth_deg,sun_elevation_deg,wind_strength,wind_phase,lod4_limit_px,lod6_limit_px,"
            "instances,segment,mode,repetition,pose,distance,yaw_deg,pitch_deg,groups_lod3,groups_lod4,groups_lod6,"
            "cut_hash,instance_lod_checks,instance_lod6_accepts,group_lod_checks,"
            "cpu_select_ms,cpu_draw_list_ms,cpu_submit_ms,gpu_camera_ms,gpu_shadow_ms,gpu_frame_ms,wall_frame_ms,"
            "camera_draws,camera_triangles,camera_foliage_draws,camera_foliage_triangles,camera_foliage_calls,"
            "shadow_draws,shadow_triangles,shadow_foliage_draws,shadow_foliage_triangles,shadow_foliage_calls\n";
    rows << std::fixed << std::setprecision(6);

    std::vector<std::size_t> referenceCut;
    for (int side : {1, branchGridSide})
    {
        scene.setBranchGrid(side);
        renderer.setScene(scene);
        const auto count = scene.instanceCount();
        const auto groupCount = scene.fixedLod->groups.size() * count;
        for (const auto& segment : segments)
        {
            for (const auto& mode : modes)
            {
                if (mode.level == static_cast<int>(fixedLodTwoTriangleLevel) && !segment.compareProxy) continue;
                scene.branchLod = mode.level;
                const std::wstring stage(segment.name.begin(), segment.name.end());
                const std::wstring label(mode.name.begin(), mode.name.end());
                const auto title = L"Oak Branch benchmark | " + std::to_wstring(count) + L" instances | " + stage + L" | " + label;
                SetWindowTextW(window, title.c_str());

                const bool automatic = mode.level == automaticBranchLod;
                constexpr std::array automaticModes{"auto_direct", "auto_indirect", "auto_instance"};
                const unsigned variants = automatic ? static_cast<unsigned>(automaticModes.size()) : 1;
                for (unsigned frame = 0; frame < warmupFrames + routeSamples * repetitions; ++frame)
                {
                    const bool warming = frame < warmupFrames;
                    const auto sample = warming ? 0 : frame - warmupFrames;
                    const auto pose = sample % routeSamples;
                    const auto t = static_cast<float>(pose) / (routeSamples - 1);
                    const float yaw = std::lerp(-125.5924f, -115.5924f, t);
                    const float distance = std::lerp(segment.fromDistance, segment.toDistance, t);
                    const CameraView view{focus, XMConvertToRadians(yaw), XMConvertToRadians(pitch), distance};
                    const auto matrix = view.matrix();
                    for (unsigned variant = 0; variant < variants; ++variant)
                    {
                        if (!processMessages()) return 2;
                        // Rotate the first variant across poses and repetitions.
                        const auto index = (variant + pose + sample / routeSamples) % variants;
                        scene.branchLodSelection.indirectDraws = automatic && index != 0;
                        scene.branchLodSelection.useInstanceBounds = automatic && index == 2;
                        const auto frameStart = Clock::now();
                        double selectMs = 0.0;
                        if (automatic)
                        {
                            renderer.updateBranchLod(scene, matrix);
                            selectMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
                        }
                        const auto timing = renderer.drawFrame(scene, matrix, windPhase, {});
                        const double wallMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
                        if (warming) continue;
                        if (!timing.gpuCameraMilliseconds || !timing.gpuShadowMilliseconds || !timing.gpuMilliseconds)
                        {
                            throw std::runtime_error("GPU timestamps are unavailable for the branch benchmark.");
                        }

                        std::array<std::size_t, fixedLodLevelCount> selected{};
                        std::uint64_t cutHash = 0;
                        BranchLodSelection::TestCounts tests;
                        if (automatic)
                        {
                            const auto& selection = scene.branchLodSelection;
                            tests = selection.tests;
                            if (variant == 0) referenceCut = selection.groupLevels;
                            else if (selection.groupLevels != referenceCut)
                            {
                                throw std::runtime_error("Automatic benchmark variants selected different group levels.");
                            }
                            cutHash = 14695981039346656037ull;
                            for (auto level : selection.groupLevels)
                            {
                                ++selected[level];
                                cutHash = (cutHash ^ level) * 1099511628211ull;
                            }
                        }
                        else if (mode.level >= 0)
                        {
                            selected[static_cast<std::size_t>(mode.level)] = groupCount;
                        }
                        const auto csvMode = automatic ? automaticModes[index] : mode.name;
                        rows << settings << count << ',' << segment.name << ',' << csvMode << ','
                             << sample / routeSamples << ',' << pose << ',' << distance << ',' << yaw << ',' << pitch
                             << ',' << selected[fixedLodRimLevel] << ',' << selected[fixedLodFarLevel]
                             << ',' << selected[fixedLodTwoTriangleLevel] << ',' << std::hex << cutHash << std::dec
                             << ',' << tests.instances << ',' << tests.acceptedInstances << ',' << tests.groups
                             << ',' << selectMs << ',' << timing.cpuBranchDrawListMilliseconds << ',' << timing.cpuMilliseconds
                             << ',' << *timing.gpuCameraMilliseconds << ',' << *timing.gpuShadowMilliseconds
                             << ',' << *timing.gpuMilliseconds << ',' << wallMs;
                        writePass(rows, timing.camera);
                        writePass(rows, timing.shadow);
                        rows << '\n';
                    }
                }
            }
        }
    }

    std::ofstream output(report, std::ios::binary | std::ios::trunc);
    output << rows.str();
    output.close();
    if (!output) throw std::runtime_error("Could not write the branch benchmark CSV. Check the output path.");
    return 0;
}

namespace
{
struct ShadowMode
{
    std::string_view name;
    int level;
    bool offCameraOnly = false;
};

constexpr ShadowMode shadowModes[] = {
    {"match", matchCameraShadowLod}, {"all3", fixedLodRimLevel}, {"all4", fixedLodFarLevel},
    {"all6", fixedLodTwoTriangleLevel}, {"off3", fixedLodRimLevel, true},
    {"off4", fixedLodFarLevel, true}, {"off6", fixedLodTwoTriangleLevel, true}};

struct ShadowPose
{
    const wchar_t* title;
    std::string_view name;
    float distance;
    float yaw;
    float pitch;
};

constexpr ShadowPose shadowPoses[] = {{L"Near", "near", 20.0f, -122.0f, 25.0f},
    {L"Off-screen casters", "offscreen", 35.0f, -125.0f, 68.0f}, {L"Distant", "distant", 120.0f, -125.0f, 68.0f}};
constexpr float shadowWindPhases[] = {0.0f, 0.0f, 0.12f, 0.24f, 0.36f, 0.48f, 0.60f, 0.72f, 0.84f};
constexpr std::size_t shadowTimeSamples = 5;

struct ReceiverDifference
{
    std::uint64_t receiverPixels = 0;
    std::uint64_t shadowPixels = 0;
    std::uint64_t changedPixels = 0;
    std::uint64_t quarterChangedPixels = 0;
    std::uint64_t lightGained = 0;
    std::uint64_t lightLost = 0;
    std::uint64_t temporalPixels = 0;
    std::uint64_t temporalDelta = 0;
};

using ReceiverDifferences = std::array<ReceiverDifference, 2>;

ReceiverDifferences compareShadowFactors(std::span<const std::uint32_t> reference,
                                       std::span<const std::uint32_t> sample,
                                       std::span<const std::uint32_t> previousReference,
                                       std::span<std::int16_t> previousDifference)
{
    ReceiverDifferences result{};
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        const auto receiver = reference[i] >> 24;
        if (receiver != sample[i] >> 24)
        {
            throw std::runtime_error("The shadow LOD override changed camera receiver coverage.");
        }
        if (receiver != 64 && receiver != 191) continue;
        auto& metric = result[receiver == 64 ? 0 : 1];
        const int before = reference[i] & 255;
        const int after = sample[i] & 255;
        const int delta = after - before;
        ++metric.receiverPixels;
        metric.shadowPixels += before < 255 || after < 255;
        metric.changedPixels += delta != 0;
        metric.quarterChangedPixels += std::abs(delta) >= 64;
        metric.lightGained += std::max(0, delta);
        metric.lightLost += std::max(0, -delta);
        if (!previousReference.empty() && receiver == previousReference[i] >> 24 &&
            (before < 255 || after < 255 || (previousReference[i] & 255) < 255 || previousDifference[i] != 0))
        {
            ++metric.temporalPixels;
            metric.temporalDelta += std::abs(delta - previousDifference[i]);
        }
        previousDifference[i] = static_cast<std::int16_t>(delta);
    }
    return result;
}

void writeReceiver(std::ostream& out, const ReceiverDifference& metric)
{
    // Light gained/lost use the equivalent area of fully lit pixels.
    out << ',' << metric.receiverPixels << ',' << metric.shadowPixels << ',' << metric.changedPixels
        << ',' << metric.quarterChangedPixels << ',' << metric.lightGained / 255.0 << ',' << metric.lightLost / 255.0
        << ',' << metric.temporalPixels << ',' << metric.temporalDelta / 255.0;
}

void applyShadowMode(ViewerScene& scene, const ShadowMode& mode)
{
    scene.branchShadowLod = mode.level;
    scene.shadowLodOffCameraOnly = mode.offCameraOnly;
}

struct ShadowSample
{
    std::array<double, shadowTimeSamples> gpuShadow{};
    std::array<double, shadowTimeSamples> cpuDrawList{};
    D3D12Renderer::FrameTiming timing{};
};
}

int runBranchShadowBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                             const std::filesystem::path& report)
{
    if (!scene.fixedLod || scene.fixedLod->levels.size() != fixedLodLevelCount)
    {
        throw std::runtime_error("The shadow benchmark requires the complete Oak Branch bake.");
    }
    scene.sunAzimuth = -100.0f;
    scene.branchLod = automaticBranchLod;
    scene.branchFrustumCulling = true;
    scene.setBranchGrid(branchGridSide);
    renderer.setScene(scene);

    std::ostringstream rows;
    rows.imbue(std::locale::classic());
    rows << "schema,adapter,source_fingerprint,bake_fingerprint,width,height,msaa,pose,distance,yaw,pitch,"
            "sun_azimuth,sun_elevation,wind_strength,wind_phase,mode,estimated_max_texels,"
            "camera_instances,shadow_instances,overridden_instances,camera_foliage_triangles,shadow_triangles,"
            "shadow_foliage_triangles,shadow_draws,shadow_foliage_draws,shadow_foliage_calls,"
            "gpu_shadow_median_ms,cpu_draw_list_median_ms";
    for (auto receiver : {"ground_", "branch_"})
    {
        for (auto field : {"pixels", "shadow_union_pixels", "changed_pixels", "quarter_changed_pixels",
                           "light_gained_px", "light_lost_px", "temporal_union_pixels", "temporal_delta_px"})
        {
            rows << ',' << receiver << field;
        }
    }
    rows << '\n' << std::fixed << std::setprecision(6);

    const auto& lod = *scene.fixedLod;
    const auto viewport = renderer.viewportSize();
    const auto focus = scene.cameraView(CameraPreset::Overview).focus;
    const auto sourceFingerprint = fixedLodSourceFingerprint(scene.source);
    const auto bakeFingerprint = fixedLodSourceFingerprint(fixedLodPath(scene.source));
    const auto adapter = csvQuoted(renderer.adapterName());
    const auto meshTransform = XMLoadFloat4x4(&scene.asset->meshes[lod.meshIndex].localToWorld);
    std::array<float, fixedLodLevelCount> maxMeshErrors{};
    for (const auto& group : lod.groups)
    {
        for (std::size_t level = 0; level < maxMeshErrors.size(); ++level)
        {
            maxMeshErrors[level] = std::max(maxMeshErrors[level], group.levels[level].surfaceError());
        }
    }

    for (float elevation : {27.0f, 68.0f})
    {
        scene.sunElevation = elevation;
        renderer.updateSun(scene);
        std::array<float, fixedLodLevelCount> texelErrors{};
        for (const auto& object : scene.objects)
        {
            XMFLOAT4X4 model;
            XMStoreFloat4x4(&model, meshTransform * object.placementMatrix());
            for (std::size_t level = 0; level < texelErrors.size(); ++level)
            {
                texelErrors[level] = std::max(texelErrors[level], renderer.projectShadowError(maxMeshErrors[level], model));
            }
        }
        for (const auto& pose : shadowPoses)
        {
            const auto title = std::wstring(L"Oak Branch shadow benchmark | ") + pose.title;
            SetWindowTextW(window, title.c_str());
            const CameraView camera{focus, XMConvertToRadians(pose.yaw), XMConvertToRadians(pose.pitch), pose.distance};
            const auto view = camera.matrix();
            scene.branchLodSelection.frozen = false;
            renderer.updateBranchLod(scene, view);
            scene.branchLodSelection.frozen = true;
            std::array<std::vector<std::int16_t>, std::size(shadowModes)> previousDifference;
            std::vector<std::uint32_t> reference, sample, previousReference;

            for (std::size_t phase = 0; phase < std::size(shadowWindPhases); ++phase)
            {
                scene.windStrength = phase == 0 ? 0.0f : 1.0f;
                const float windPhase = shadowWindPhases[phase];
                std::array<ShadowSample, std::size(shadowModes)> samples;
                scene.showShadowFactor = false;
                for (std::size_t repetition = 0; repetition <= shadowTimeSamples; ++repetition)
                {
                    for (std::size_t offset = 0; offset < std::size(shadowModes); ++offset)
                    {
                        if (!processMessages()) return 2;
                        const auto i = (offset + repetition + phase) % std::size(shadowModes);
                        applyShadowMode(scene, shadowModes[i]);
                        const auto timing = renderer.drawFrame(scene, view, windPhase, {});
                        if (repetition == 0) continue;
                        if (!timing.gpuShadowMilliseconds) throw std::runtime_error("Shadow GPU timestamps unavailable.");
                        auto& measured = samples[i];
                        measured.timing = timing;
                        measured.gpuShadow[repetition - 1] = *timing.gpuShadowMilliseconds;
                        measured.cpuDrawList[repetition - 1] = timing.cpuBranchDrawListMilliseconds;
                    }
                }

                scene.showShadowFactor = true;
                if (phase == 1) previousReference.clear();
                const auto& matched = samples.front().timing;
                for (std::size_t i = 0; i < std::size(shadowModes); ++i)
                {
                    const auto& mode = shadowModes[i];
                    applyShadowMode(scene, mode);
                    const auto& frame = samples[i].timing;
                    const auto& shadow = frame.shadow;
                    if (frame.camera.visibleInstances != matched.camera.visibleInstances ||
                        frame.camera.foliageTriangles != matched.camera.foliageTriangles ||
                        frame.camera.draws != matched.camera.draws || shadow.visibleInstances != scene.instanceCount())
                    {
                        throw std::runtime_error("Shadow comparison did not preserve the camera cut and caster set.");
                    }
                    if (mode.level >= 0)
                    {
                        const auto overridden = shadow.visibleInstances - (mode.offCameraOnly ? frame.camera.visibleInstances : 0);
                        const auto forcedTriangles = lod.levels[mode.level].indices.size() / 3 * overridden;
                        const auto expected = forcedTriangles + (mode.offCameraOnly ? frame.camera.foliageTriangles : 0);
                        if (shadow.shadowLodOverrides != overridden || shadow.foliageTriangles != expected)
                        {
                            throw std::runtime_error("The override submitted the wrong set of shadow foliage.");
                        }
                    }

                    auto& pixels = i == 0 ? reference : sample;
                    renderer.drawFrame(scene, view, windPhase, {}, {}, &pixels);
                    auto& difference = previousDifference[i];
                    difference.resize(reference.size());
                    const auto quality = compareShadowFactors(reference, pixels, previousReference, difference);
                    if (!quality[0].receiverPixels || !quality[1].receiverPixels ||
                        (mode.offCameraOnly && shadow.shadowLodOverrides == 0 && pixels != reference))
                    {
                        throw std::runtime_error("Shadow benchmark receivers or no-override image did not match expectations.");
                    }
                    auto& measured = samples[i];
                    std::sort(measured.gpuShadow.begin(), measured.gpuShadow.end());
                    std::sort(measured.cpuDrawList.begin(), measured.cpuDrawList.end());
                    rows << "1," << adapter << ',' << std::hex << sourceFingerprint << ',' << bakeFingerprint << std::dec
                         << ',' << viewport.x << ',' << viewport.y << ",1," << pose.name << ',' << pose.distance
                         << ',' << pose.yaw << ',' << pose.pitch << ',' << scene.sunAzimuth << ',' << elevation
                         << ',' << scene.windStrength << ',' << windPhase << ',' << mode.name << ',';
                    if (mode.level >= 0) rows << texelErrors[mode.level];
                    rows << ',' << frame.camera.visibleInstances << ',' << shadow.visibleInstances << ','
                         << shadow.shadowLodOverrides << ',' << frame.camera.foliageTriangles << ',' << shadow.triangles
                         << ',' << shadow.foliageTriangles << ',' << shadow.draws << ',' << shadow.foliageDraws
                         << ',' << shadow.foliageCalls << ',' << measured.gpuShadow[shadowTimeSamples / 2]
                         << ',' << measured.cpuDrawList[shadowTimeSamples / 2];
                    for (const auto& receiver : quality) writeReceiver(rows, receiver);
                    rows << '\n';
                }
                previousReference.swap(reference);
            }
        }
    }
    std::ofstream output(report, std::ios::binary | std::ios::trunc);
    output << rows.str();
    output.close();
    if (!output) throw std::runtime_error("Could not write the branch shadow benchmark CSV.");
    return 0;
}
