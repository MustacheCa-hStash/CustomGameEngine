#pragma once

#include "D3D12Renderer.h"

int runBranchBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                       const std::filesystem::path& report);
int runBranchShadowBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                             const std::filesystem::path& report);
