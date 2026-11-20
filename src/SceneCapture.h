#pragma once

#include "ViewerScene.h"

#include <filesystem>
#include <span>

struct SceneCaptureReview
{
    ViewerScene* scene;
    CameraView camera;
    unsigned msaa;
    double windTime;
    bool cullMeshBackfaces;
};

SceneCaptureReview prepareSceneCaptureReview(std::span<wchar_t* const> arguments, ViewerScenes& scenes);
void captureScene(std::span<wchar_t* const> arguments, const std::filesystem::path& assetDirectory);
void captureProbeWalk(const std::filesystem::path& output, const std::filesystem::path& assetDirectory);
void captureProbeWind(const std::filesystem::path& output, const std::filesystem::path& assetDirectory, bool captureQuality);
void reportCaptureError(const char* message);
