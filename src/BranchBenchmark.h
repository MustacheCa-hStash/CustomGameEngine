#pragma once

#include "D3D12Renderer.h"

int runBranchBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                       const std::filesystem::path& report);
int runBranchShadowBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                             const std::filesystem::path& report);
enum class VoxelShadingComparison { None, PooledNormals, DirectionalNormals, ShadowReceiver };
int runBranchVoxelBenchmark(HWND window, ViewerScene& scene, D3D12Renderer& renderer,
                            const std::filesystem::path& report,
                            const std::array<std::filesystem::path, 2>& farVoxelBakes, VoxelShadingComparison shading,
                            bool compareProxyBounds = false);
int runBranchLightingDiagnostics(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                                 bool directional, bool compareMidpoint, bool compareCasters);
int runBranchTransitionBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                                 float entryPixels);
int runBranchRegionBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                             unsigned region, const std::filesystem::path& candidate);
int runOakWalkBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report);
int runOakWalkCullBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report);
int runSunCascadeBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report,
                           bool stabilized);
int runSunCascadeStabilityBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report);
int runSunCascadeTransitionBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report);
int runSunCascadeCasterBenchmark(ViewerScene& scene, D3D12Renderer& renderer, const std::filesystem::path& report);
