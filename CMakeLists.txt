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

add_custom_target(viewer_assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/cube.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOakLOD0.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOak-SingleBranch.glb"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/LowBranchedWhiteOak-Modeled.glb"
        "$<TARGET_FILE_DIR:CustomGameEngine>/assets"
    VERBATIM
)
add_dependencies(CustomGameEngine viewer_assets)
