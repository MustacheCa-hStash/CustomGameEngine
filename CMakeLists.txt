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

add_executable(CustomGameEngine WIN32
    src/main.cpp
    src/BranchBenchmark.cpp
    src/BranchBenchmark.h
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
    "${mesh_vs}"
    "${mesh_ps}"
    "${shadow_vs}"
    "${shadow_ps}"
    "${hud_vs}"
    "${hud_ps}"
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
target_include_directories(BakeFixedLod PRIVATE src)
target_include_directories(BakeFixedLod SYSTEM PRIVATE third_party/cgltf)
target_compile_features(BakeFixedLod PRIVATE cxx_std_20)
target_compile_definitions(BakeFixedLod PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(BakeFixedLod PRIVATE /W4 /permissive-)
target_link_libraries(BakeFixedLod PRIVATE meshoptimizer ole32 windowscodecs)

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
add_custom_target(viewer_assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/cube.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOakLOD0.glb"
        "${branch_glb}"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOak-Modeled.glb"
        "${branch_bake}"
        "${branch_bake}.txt"
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    DEPENDS "${branch_bake}" "${branch_bake}.txt"
    VERBATIM
)
add_dependencies(CustomGameEngine viewer_assets)
