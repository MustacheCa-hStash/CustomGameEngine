#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct MeshShaders
{
    std::vector<std::byte> cameraVertex;
    std::vector<std::byte> cameraPixel;
    std::vector<std::byte> shadowVertex;
    std::vector<std::byte> shadowPixel;
};

struct ShaderCompilation
{
    std::optional<MeshShaders> shaders;
    std::string diagnostics;
};

ShaderCompilation compileMeshShaders();
