cmake_minimum_required(VERSION 3.24)
project(CustomGameEngine LANGUAGES CXX)

find_program(DXC_EXECUTABLE NAMES dxc
    HINTS "[HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots;KitsRoot10]"
    PATH_SUFFIXES "bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    REGISTRY_VIEW BOTH
    REQUIRED
)

set(mesh_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Mesh.hlsl")
set(mesh_vs "${CMAKE_CURRENT_BINARY_DIR}/MeshVS.h")
set(mesh_ps "${CMAKE_CURRENT_BINARY_DIR}/MeshPS.h")
set(shadow_vs "${CMAKE_CURRENT_BINARY_DIR}/ShadowVS.h")
set(shadow_ps "${CMAKE_CURRENT_BINARY_DIR}/ShadowPS.h")
set(hud_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Hud.hlsl")
set(hud_vs "${CMAKE_CURRENT_BINARY_DIR}/HudVS.h")
set(hud_ps "${CMAKE_CURRENT_BINARY_DIR}/HudPS.h")
set(voxel_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Voxel.hlsl")
set(voxel_vs "${CMAKE_CURRENT_BINARY_DIR}/VoxelVS.h")
set(voxel_ps "${CMAKE_CURRENT_BINARY_DIR}/VoxelPS.h")
set(voxel_shadow_ps "${CMAKE_CURRENT_BINARY_DIR}/VoxelShadowPS.h")
add_custom_command(
    OUTPUT "${mesh_vs}" "${mesh_ps}" "${shadow_vs}" "${shadow_ps}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fh "${mesh_vs}" -Vn meshVS "${mesh_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fh "${mesh_ps}" -Vn meshPS "${mesh_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E ShadowVS -Fh "${shadow_vs}" -Vn shadowVS "${mesh_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E ShadowPS -Fh "${shadow_ps}" -Vn shadowPS "${mesh_shader}"
    DEPENDS "${mesh_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling Mesh.hlsl with DXC"
    VERBATIM
)
add_custom_command(
    OUTPUT "${hud_vs}" "${hud_ps}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fh "${hud_vs}" -Vn hudVS "${hud_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fh "${hud_ps}" -Vn hudPS "${hud_shader}"
    DEPENDS "${hud_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling Hud.hlsl with DXC"
    VERBATIM
)
add_custom_command(
    OUTPUT "${voxel_vs}" "${voxel_ps}" "${voxel_shadow_ps}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fh "${voxel_vs}" -Vn voxelVS "${voxel_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fh "${voxel_ps}" -Vn voxelPS "${voxel_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E ShadowPS -Fh "${voxel_shadow_ps}" -Vn voxelShadowPS "${voxel_shader}"
    DEPENDS "${voxel_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling Voxel.hlsl with DXC"
    VERBATIM
)

add_executable(CustomGameEngine WIN32
    src/main.cpp
    src/BranchBenchmark.cpp
    src/BranchBenchmark.h
    src/BranchVoxelBenchmark.cpp
    src/ViewerScene.cpp
    src/ViewerScene.h
    src/ViewerSettings.cpp
    src/ViewerSettings.h
    src/ShaderCompiler.cpp
    src/ShaderCompiler.h
    src/SceneInspector.cpp
    src/SceneInspector.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/GltfLoader.cpp
    src/GltfLoader.h
    src/TextureLoader.cpp
    src/TextureLoader.h
    src/MeshData.h
    src/FixedLod.cpp
    src/FixedLod.h
    src/VoxelFoliage.cpp
    "${mesh_vs}"
    "${mesh_ps}"
    "${shadow_vs}"
    "${shadow_ps}"
    "${hud_vs}"
    "${hud_ps}"
    "${voxel_vs}"
    "${voxel_ps}"
    "${voxel_shadow_ps}"
)
add_library(dear_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx12.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
)
target_include_directories(dear_imgui SYSTEM PUBLIC third_party/imgui)
target_compile_definitions(dear_imgui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(dear_imgui PRIVATE d3dcompiler dwmapi gdi32)
target_include_directories(CustomGameEngine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(CustomGameEngine SYSTEM PRIVATE third_party/cgltf)
target_compile_features(CustomGameEngine PRIVATE cxx_std_20)
target_compile_definitions(CustomGameEngine PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    "VIEWER_PROJECT_DIRECTORY=L\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "VIEWER_DXC_EXECUTABLE=L\"${DXC_EXECUTABLE}\"")
target_compile_options(CustomGameEngine PRIVATE /W4 /permissive-)
target_link_libraries(CustomGameEngine PRIVATE dear_imgui d3d12 dxgi ole32 shell32 user32 windowscodecs)

add_library(meshoptimizer STATIC
    third_party/meshoptimizer/allocator.cpp
    third_party/meshoptimizer/clusterizer.cpp
    third_party/meshoptimizer/simplifier.cpp
)
target_include_directories(meshoptimizer SYSTEM PUBLIC third_party/meshoptimizer)

add_executable(BakeFixedLod Tools/BakeFixedLod.cpp src/FixedLod.cpp src/GltfLoader.cpp src/TextureLoader.cpp)
add_library(voxel_bake STATIC Tools/VoxelBake.cpp src/VoxelFoliage.cpp)
target_include_directories(voxel_bake PUBLIC src)
target_compile_features(voxel_bake PUBLIC cxx_std_20)
target_compile_options(voxel_bake PRIVATE /W4 /permissive-)
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # Debug viewer builds also generate these bakes. Optimize the offline sampling loops.
    target_compile_definitions(voxel_bake PUBLIC "$<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>")
    set_property(SOURCE Tools/VoxelBake.cpp PROPERTY VS_SETTINGS
        "$<$<CONFIG:Debug>:Optimization=MaxSpeed>"
        "$<$<CONFIG:Debug>:BasicRuntimeChecks=Default>"
        "$<$<CONFIG:Debug>:InlineFunctionExpansion=AnySuitable>")
endif()

add_executable(BakeVoxelFoliage Tools/BakeVoxelFoliage.cpp src/GltfLoader.cpp src/TextureLoader.cpp src/FixedLod.cpp)
add_library(foliage_trace STATIC Tools/FoliageTrace.cpp Tools/FoliageComparison.cpp)
target_include_directories(foliage_trace PUBLIC src)
target_compile_features(foliage_trace PUBLIC cxx_std_20)
target_compile_options(foliage_trace PRIVATE /W4 /permissive-)

add_executable(CompareVoxelFoliage Tools/CompareVoxelFoliage.cpp Tools/VoxelCoverageOptimizer.cpp Tools/VoxelBake.cpp src/ViewerScene.cpp src/FixedLod.cpp
    src/GltfLoader.cpp src/TextureLoader.cpp src/VoxelFoliage.cpp)
foreach(tool IN ITEMS BakeFixedLod BakeVoxelFoliage CompareVoxelFoliage)
    target_include_directories(${tool} PRIVATE src)
    target_include_directories(${tool} SYSTEM PRIVATE third_party/cgltf)
    target_compile_features(${tool} PRIVATE cxx_std_20)
    target_compile_definitions(${tool} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${tool} PRIVATE /W4 /permissive-)
    target_link_libraries(${tool} PRIVATE ole32 windowscodecs)
endforeach()
target_link_libraries(BakeFixedLod PRIVATE meshoptimizer)
target_link_libraries(BakeVoxelFoliage PRIVATE voxel_bake)
target_link_libraries(CompareVoxelFoliage PRIVATE foliage_trace)
target_link_libraries(CustomGameEngine PRIVATE foliage_trace)

enable_testing()
add_executable(VoxelBakeTests Tools/VoxelBakeTests.cpp)
target_compile_options(VoxelBakeTests PRIVATE /W4 /permissive-)
target_link_libraries(VoxelBakeTests PRIVATE voxel_bake)
add_test(NAME VoxelBakeFixtures COMMAND VoxelBakeTests "${CMAKE_CURRENT_BINARY_DIR}/voxel-fixture-$<CONFIG>.voxels")
add_executable(FoliageTraceTests Tools/FoliageTraceTests.cpp Tools/VoxelCoverageOptimizer.cpp)
target_compile_options(FoliageTraceTests PRIVATE /W4 /permissive-)
target_link_libraries(FoliageTraceTests PRIVATE foliage_trace)
add_test(NAME FoliageTraceFixtures COMMAND FoliageTraceTests)

set(branch_name LowBranchedWhiteOak-SingleBranch)
set(branch_glb "${CMAKE_CURRENT_SOURCE_DIR}/assets/${branch_name}.glb")
set(branch_bake "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/${branch_name}.fixedlod")
add_custom_command(
    OUTPUT "${branch_bake}" "${branch_bake}.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes"
    COMMAND BakeFixedLod "${branch_glb}" "Oak branch - modeled leaves" "${branch_bake}" --rim-anchors uv-v --uv-weight 0.1 --leaf-proxies uv-v
    DEPENDS BakeFixedLod "${branch_glb}"
    COMMENT "Baking fixed LODs for Oak Branch"
    VERBATIM
)
set(branch_voxel_assets)
set(branch_voxel_size_fine 0.015625)
set(branch_voxel_size_coarse 0.03125)
set(branch_voxel_size_sixteenth 0.0625)
set(branch_voxel_size_eighth 0.125)
set(branch_voxel_size_quarter 0.25)
set(branch_voxel_size_half 0.5)
foreach(level IN ITEMS fine coarse sixteenth eighth quarter half)
    set(voxel_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/voxels-${level}")
    set(voxel_asset "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/${branch_name}.${level}.voxels")
    add_custom_command(
        OUTPUT "${voxel_asset}"
        COMMAND BakeVoxelFoliage "${branch_glb}" "Oak branch - modeled leaves" "${voxel_dir}"
            --cell-size ${branch_voxel_size_${level}} --levels 1 --samples 16 --cutoff 0.25
        COMMAND "${CMAKE_COMMAND}" -E copy "${voxel_dir}/level-0-cutoff-1.voxels" "${voxel_asset}"
        DEPENDS BakeVoxelFoliage "${branch_glb}"
        COMMENT "Baking ${level} Oak Branch voxels"
        VERBATIM
    )
    list(APPEND branch_voxel_assets "${voxel_asset}")
endforeach()
add_custom_target(viewer_assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/cube.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOakLOD0.glb"
        "${branch_glb}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOak-Modeled.glb"
        "${branch_bake}"
        "${branch_bake}.txt"
        ${branch_voxel_assets}
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    DEPENDS "${branch_bake}" "${branch_bake}.txt" ${branch_voxel_assets}
    VERBATIM
)
add_dependencies(CustomGameEngine viewer_assets)

set(crown_glb "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOak-Modeled.glb")
set(crown_bake "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/LowBranchedWhiteOak-Modeled.fixedlod")
add_custom_command(
    OUTPUT "${crown_bake}" "${crown_bake}.txt"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes"
    COMMAND BakeFixedLod "${crown_glb}" "Individual curved leaf geometry" "${crown_bake}"
        --triangle-cap 128 --group-size 64 --samples 4 --rim-anchors uv-v --uv-weight 0.1 --leaf-proxies uv-v
    DEPENDS BakeFixedLod "${crown_glb}"
    COMMENT "Baking the full Modeled Oak triangle LODs"
    VERBATIM
)
add_custom_target(oak_crown_lod
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${crown_bake}" "${crown_bake}.txt"
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    DEPENDS "${crown_bake}" "${crown_bake}.txt"
    VERBATIM
)
add_dependencies(oak_crown_lod viewer_assets)

set(branch_coverage_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/coverage")
set(branch_coverage_commands)
foreach(level IN ITEMS sixteenth eighth quarter)
    set(fit_route --near-fit)
    if(level STREQUAL "quarter")
        set(fit_route)
    endif()
    list(APPEND branch_coverage_commands
        COMMAND CompareVoxelFoliage "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${branch_coverage_dir}/${level}"
            --optimize "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/voxels-${level}/level-0-overlap.voxels"
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/${branch_name}.${level}.voxels" --samples 4 ${fit_route}
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${branch_coverage_dir}/${level}/coverage-optimized.voxels"
            "$<TARGET_FILE_DIR:CustomGameEngine>/assets/${branch_name}.${level}.coverage.voxels"
    )
endforeach()
add_custom_target(oak_branch_coverage
    ${branch_coverage_commands}
    COMMENT "Fitting Oak Branch voxel occupancy to projected coverage"
    VERBATIM
)
add_dependencies(oak_branch_coverage CompareVoxelFoliage viewer_assets)

set(branch_normal_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/normals")
add_custom_target(oak_branch_normals
    COMMAND BakeVoxelFoliage "${branch_glb}" "Oak branch - modeled leaves" "${branch_normal_dir}"
        --samples 16 --normal-mask "${branch_coverage_dir}/eighth/coverage-optimized.voxels"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${branch_normal_dir}/voxel-normals.bin"
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets/${branch_name}.eighth.coverage.normals"
    COMMENT "Baking normal distributions for the optimized Oak Branch voxel mask"
    VERBATIM
)
add_dependencies(oak_branch_normals oak_branch_coverage)

add_custom_target(oak_branch_voxel_regions
    COMMAND BakeVoxelFoliage "$<TARGET_FILE_DIR:CustomGameEngine>/assets/${branch_name}.glb"
        "Oak branch - modeled leaves" "${branch_normal_dir}" --samples 16
        --region-mask "${branch_coverage_dir}/eighth/coverage-optimized.voxels"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${branch_normal_dir}/voxel-regions.bin"
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets/${branch_name}.eighth.coverage.regions"
    COMMENT "Assigning optimized Oak Branch voxels to triangle regions"
    VERBATIM
)
add_dependencies(oak_branch_voxel_regions oak_branch_normals)
