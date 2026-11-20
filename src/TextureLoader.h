#pragma once

#include "MeshData.h"

#include <filesystem>
#include <span>

TextureData loadColorTexture(std::span<const std::uint8_t> encoded, float coverageCutoff);
TextureData loadColorTexture(const std::filesystem::path& path);
