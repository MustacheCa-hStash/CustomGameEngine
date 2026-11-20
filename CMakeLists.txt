cmake_minimum_required(VERSION 3.24)
project(CustomGameEngine LANGUAGES CXX)

find_program(DXC_EXECUTABLE NAMES dxc
    HINTS "[HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots;KitsRoot10]"
    PATH_SUFFIXES "bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    REGISTRY_VIEW BOTH
    REQUIRED
)

set(mesh_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Mesh.hlsl")
set(skylight_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Skylight.hlsli")
set(leaf_lighting_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/LeafLighting.hlsli")
set(probe_lighting_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ProbeLighting.hlsli")
set(probe_trace_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ProbeTrace.hlsl")
set(probe_cs "${CMAKE_CURRENT_BINARY_DIR}/ProbeCS.h")
set(ground_probe_cs "${CMAKE_CURRENT_BINARY_DIR}/GroundProbeCS.h")
set(advect_probe_cs "${CMAKE_CURRENT_BINARY_DIR}/AdvectProbeCS.h")
set(tree_select_cs "${CMAKE_CURRENT_BINARY_DIR}/TreeSelectCS.h")
set(tree_depth_cs "${CMAKE_CURRENT_BINARY_DIR}/TreeDepthCS.h")
set(tree_batch_depth_cs "${CMAKE_CURRENT_BINARY_DIR}/TreeBatchDepthCS.h")
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
set(sky_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Sky.hlsl")
set(sky_vs "${CMAKE_CURRENT_BINARY_DIR}/SkyVS.h")
set(sky_ps "${CMAKE_CURRENT_BINARY_DIR}/SkyPS.h")
set(display_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Display.hlsl")
set(display_vs "${CMAKE_CURRENT_BINARY_DIR}/DisplayVS.h")
set(display_ps "${CMAKE_CURRENT_BINARY_DIR}/DisplayPS.h")
add_custom_command(
    OUTPUT "${mesh_vs}" "${mesh_ps}" "${shadow_vs}" "${shadow_ps}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fh "${mesh_vs}" -Vn meshVS "${mesh_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fh "${mesh_ps}" -Vn meshPS "${mesh_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E ShadowVS -Fh "${shadow_vs}" -Vn shadowVS "${mesh_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E ShadowPS -Fh "${shadow_ps}" -Vn shadowPS "${mesh_shader}"
    DEPENDS "${mesh_shader}" "${leaf_lighting_shader}" "${skylight_shader}" "${probe_lighting_shader}" "${DXC_EXECUTABLE}"
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
    DEPENDS "${voxel_shader}" "${leaf_lighting_shader}" "${skylight_shader}" "${probe_lighting_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling Voxel.hlsl with DXC"
    VERBATIM
)

add_custom_command(
    OUTPUT "${sky_vs}" "${sky_ps}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fh "${sky_vs}" -Vn skyVS "${sky_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fh "${sky_ps}" -Vn skyPS "${sky_shader}"
    DEPENDS "${sky_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling Sky.hlsl with DXC"
    VERBATIM
)

add_custom_command(
    OUTPUT "${display_vs}" "${display_ps}"
    COMMAND "${DXC_EXECUTABLE}" -T vs_6_0 -E VSMain -Fh "${display_vs}" -Vn displayVS "${display_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T ps_6_0 -E PSMain -Fh "${display_ps}" -Vn displayPS "${display_shader}"
    DEPENDS "${display_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling Display.hlsl with DXC"
    VERBATIM
)

add_custom_command(
    OUTPUT "${probe_cs}" "${ground_probe_cs}" "${advect_probe_cs}"
    COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E CSMain -Fh "${probe_cs}" -Vn probeCS "${probe_trace_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E GroundCS -Fh "${ground_probe_cs}" -Vn groundProbeCS "${probe_trace_shader}"
    COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E AdvectCS -Fh "${advect_probe_cs}" -Vn advectProbeCS "${probe_trace_shader}"
    DEPENDS "${probe_trace_shader}" "${DXC_EXECUTABLE}"
    COMMENT "Compiling ProbeTrace.hlsl with DXC"
    VERBATIM
)

add_custom_command(
    OUTPUT "${tree_select_cs}" "${tree_depth_cs}" "${tree_batch_depth_cs}"
    COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E Select -Fh "${tree_select_cs}" -Vn treeSelectCS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/TreeSelect.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E BuildTiles -Fh "${tree_depth_cs}" -Vn treeDepthCS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/TreeDepth.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E BuildOcclusionTiles -Fh "${tree_batch_depth_cs}" -Vn treeBatchDepthCS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/TreeDepth.hlsl"
    DEPENDS shaders/TreeSelect.hlsl shaders/TreeDepth.hlsl shaders/TreeCull.hlsli shaders/TreePyramid.hlsli "${DXC_EXECUTABLE}"
    COMMENT "Compiling tree traversal and depth-pyramid shaders"
    VERBATIM
)

foreach(stage IN ITEMS Traverse Select Clip Encode Scatter PrepareOcclusion CullHistory RetestOcclusion)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/TreeBatch${stage}CS.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E "${stage}" -Fh "${header}" -Vn "treeBatch${stage}CS"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/TreeBatch.hlsl"
        DEPENDS shaders/TreeBatch.hlsl shaders/TreeCull.hlsli shaders/TreePyramid.hlsli "${DXC_EXECUTABLE}"
        COMMENT "Compiling batched tree ${stage}"
        VERBATIM
    )
    list(APPEND tree_batch_headers "${header}")
endforeach()

add_executable(CustomGameEngine WIN32
    src/main.cpp
    src/SceneCapture.cpp
    src/SceneCapture.h
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
    src/SkyLighting.cpp
    src/SkyLighting.h
    src/ProbeLighting.cpp
    src/ProbeLighting.h
    src/GltfLoader.cpp
    src/GltfLoader.h
    src/TextureLoader.cpp
    src/TextureLoader.h
    src/MeshData.h
    src/FixedLod.cpp
    src/FixedLod.h
    src/SprayBough.cpp
    src/SprayBough.h
    src/TreeHierarchy.cpp
    src/TreeHierarchy.h
    src/TreeGpu.cpp
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
    "${sky_vs}"
    "${sky_ps}"
    "${display_vs}"
    "${display_ps}"
    "${probe_cs}"
    "${ground_probe_cs}"
    "${advect_probe_cs}"
    "${tree_select_cs}"
    "${tree_depth_cs}"
    "${tree_batch_depth_cs}"
    ${tree_batch_headers}
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
target_link_libraries(CustomGameEngine PRIVATE meshoptimizer)

add_executable(BakeFixedLod Tools/BakeFixedLod.cpp src/FixedLod.cpp src/GltfLoader.cpp src/TextureLoader.cpp)
add_executable(BakeSprayBough Tools/BakeSprayBough.cpp src/SprayBough.cpp src/TreeHierarchy.cpp src/FixedLod.cpp src/GltfLoader.cpp src/TextureLoader.cpp)
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
add_executable(BakeOakBoughVoxels Tools/BakeOakBoughVoxels.cpp Tools/VoxelCoverageOptimizer.cpp)
target_link_libraries(BakeOakBoughVoxels PRIVATE voxel_bake)
target_compile_definitions(BakeOakBoughVoxels PRIVATE NOMINMAX)
add_library(foliage_trace STATIC Tools/FoliageTrace.cpp Tools/FoliageComparison.cpp)
target_include_directories(foliage_trace PUBLIC src)
target_compile_features(foliage_trace PUBLIC cxx_std_20)
target_compile_options(foliage_trace PRIVATE /W4 /permissive-)

add_executable(CompareVoxelFoliage Tools/CompareVoxelFoliage.cpp Tools/VoxelCoverageOptimizer.cpp Tools/VoxelBake.cpp src/ViewerScene.cpp src/FixedLod.cpp src/SprayBough.cpp src/TreeHierarchy.cpp
    src/GltfLoader.cpp src/TextureLoader.cpp src/VoxelFoliage.cpp)
foreach(tool IN ITEMS BakeFixedLod BakeSprayBough BakeVoxelFoliage CompareVoxelFoliage)
    target_include_directories(${tool} PRIVATE src)
    target_include_directories(${tool} SYSTEM PRIVATE third_party/cgltf)
    target_compile_features(${tool} PRIVATE cxx_std_20)
    target_compile_definitions(${tool} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${tool} PRIVATE /W4 /permissive-)
    target_link_libraries(${tool} PRIVATE ole32 windowscodecs)
endforeach()
target_link_libraries(BakeFixedLod PRIVATE meshoptimizer)
target_link_libraries(BakeSprayBough PRIVATE meshoptimizer)
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
set(forked_oak_glb "${CMAKE_CURRENT_SOURCE_DIR}/assets/WhiteOak-ForkedCanopy.glb")
set(forked_oak_bakes)
foreach(part IN ITEMS leaves spray-wood)
    if(part STREQUAL "leaves")
        set(primitive "Forked sprays / Primitive 1")
        set(recipe --rim-anchors uv-v --lod1 30 --lod2 10 --lod3 3 --lod4 1
            --lod4-component-triangles 33 --lod4-refinement-error 0.006066 --leaf-proxies uv-v --transition-min-lod 3)
    else()
        set(primitive "Forked sprays / Primitive 2")
        set(recipe --lod1 50 --lod2 20 --lod3 8 --lod4 3 --weld-normal-splits on
            --shared-coarse-triangles 425 --shared-far-triangles 950 --shared-far-uv-weight 2 --transition-min-lod 0)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/WhiteOak-ForkedCanopy.${part}.fixedlod")
    add_custom_command(
        OUTPUT "${output}" "${output}.txt"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes"
        COMMAND BakeFixedLod "${forked_oak_glb}" "${primitive}" "${output}" --samples 4 --uv-weight 0.1 --spray-groups 8 ${recipe}
        DEPENDS BakeFixedLod "${forked_oak_glb}"
        COMMENT "Baking shared forked oak ${part}"
        VERBATIM
    )
    list(APPEND forked_oak_bakes "${output}" "${output}.txt")
endforeach()

set(bough_members "${CMAKE_CURRENT_SOURCE_DIR}/assets/source/white-oak-forked-spray/WhiteOak-Bough07.txt")
set(bough_bake "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/WhiteOak-ForkedCanopy.bough")
list(GET forked_oak_bakes 0 leaf_bake)
list(GET forked_oak_bakes 2 spray_wood_bake)
set(tree_hierarchy_bake "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/bakes/WhiteOak-ForkedCanopy.treelod")
add_custom_command(
    OUTPUT "${tree_hierarchy_bake}"
    COMMAND BakeSprayBough --hierarchy "${forked_oak_glb}" "${leaf_bake}" "${spray_wood_bake}" 2 "${tree_hierarchy_bake}" 96 384
    DEPENDS BakeSprayBough "${forked_oak_glb}" "${leaf_bake}" "${spray_wood_bake}"
    COMMENT "Baking the oak trunk and canopy geometry hierarchy"
    VERBATIM
)
add_custom_command(
    OUTPUT "${bough_bake}"
    COMMAND BakeSprayBough "${forked_oak_glb}" "${leaf_bake}" "${spray_wood_bake}" "${bough_members}" "${bough_bake}"
            --leaf-error 0.016 --wood-cell 0.045 --wood-error 0.021
    DEPENDS BakeSprayBough "${forked_oak_glb}" "${leaf_bake}" "${spray_wood_bake}" "${bough_members}"
    COMMENT "Baking the forked oak bough proxy"
    VERBATIM
)

add_custom_target(viewer_assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/cube.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOakLOD0.glb"
        "${branch_glb}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOak-Modeled.glb"
        "${forked_oak_glb}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/MeadowDaySky.hdr"
        "${branch_bake}"
        "${branch_bake}.txt"
        ${branch_voxel_assets}
        ${forked_oak_bakes}
        "${bough_bake}"
        "${tree_hierarchy_bake}"
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    DEPENDS "${branch_bake}" "${branch_bake}.txt" ${branch_voxel_assets} ${forked_oak_bakes} "${bough_bake}" "${tree_hierarchy_bake}"
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

# Keep the two-parent woody repair pilot alongside the ordinary viewer for A/B captures.
set(twig_pilot_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/twig-repair")
set(oak_view_assets "$<TARGET_FILE_DIR:CustomGameEngine>/assets")
set(twig_pilot_tree "WhiteOak-ForkedCanopy")
set(OAK_TWIG_REPAIR_MODE "anchored" CACHE STRING "Twig parent repair to show in the pilot viewer")
set_property(CACHE OAK_TWIG_REPAIR_MODE PROPERTY STRINGS anchored intermediate loose)
if(OAK_TWIG_REPAIR_MODE STREQUAL "anchored")
    set(twig_repair_flag --twig-repair)
elseif(OAK_TWIG_REPAIR_MODE MATCHES "^(loose|intermediate)$")
    set(twig_repair_flag "--twig-repair-${OAK_TWIG_REPAIR_MODE}")
else()
    message(FATAL_ERROR "OAK_TWIG_REPAIR_MODE must be anchored, intermediate or loose")
endif()
add_custom_target(oak_twig_repair_pilot
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${twig_pilot_dir}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:CustomGameEngine>" "${twig_pilot_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${oak_view_assets}/${twig_pilot_tree}.glb" "${oak_view_assets}/${twig_pilot_tree}.leaves.fixedlod"
        "${oak_view_assets}/${twig_pilot_tree}.spray-wood.fixedlod" "${oak_view_assets}/${twig_pilot_tree}.bough"
        "${oak_view_assets}/MeadowDaySky.hdr" "${twig_pilot_dir}/assets"
    COMMAND BakeSprayBough "${twig_repair_flag}" "${oak_view_assets}/${twig_pilot_tree}.glb"
        "${oak_view_assets}/${twig_pilot_tree}.leaves.fixedlod" "${oak_view_assets}/${twig_pilot_tree}.spray-wood.fixedlod"
        "${twig_pilot_dir}/assets/${twig_pilot_tree}.treelod" 56 164 "${twig_pilot_dir}/samples"
    DEPENDS CustomGameEngine BakeSprayBough
    COMMENT "Refitting failed twig patches on the 74-spray and 296-spray test parents"
    VERBATIM
)

set(branch_cut_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/branch-cut")
add_custom_command(
    OUTPUT "${branch_cut_dir}/assets/${twig_pilot_tree}.treelod" "${branch_cut_dir}/grid-control.treelod"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${branch_cut_dir}/assets"
    COMMAND BakeSprayBough --branch-cut "${oak_view_assets}/${twig_pilot_tree}.glb"
        "${oak_view_assets}/${twig_pilot_tree}.leaves.fixedlod" "${oak_view_assets}/${twig_pilot_tree}.spray-wood.fixedlod"
        "${bough_members}" "${branch_cut_dir}/grid-control.treelod" "${branch_cut_dir}/assets/${twig_pilot_tree}.treelod"
    DEPENDS BakeSprayBough "${tree_hierarchy_bake}" "${bough_members}" CustomGameEngine
    COMMENT "Building branch-connected cuts for white oak bough 7"
    VERBATIM
)
add_custom_target(oak_branch_cut_pilot
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:CustomGameEngine>" "${branch_cut_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${oak_view_assets}/${twig_pilot_tree}.glb" "${oak_view_assets}/${twig_pilot_tree}.leaves.fixedlod"
        "${oak_view_assets}/${twig_pilot_tree}.spray-wood.fixedlod" "${oak_view_assets}/${twig_pilot_tree}.bough"
        "${oak_view_assets}/MeadowDaySky.hdr" "${branch_cut_dir}/assets"
    DEPENDS "${branch_cut_dir}/assets/${twig_pilot_tree}.treelod" "${branch_cut_dir}/grid-control.treelod"
    VERBATIM
)
