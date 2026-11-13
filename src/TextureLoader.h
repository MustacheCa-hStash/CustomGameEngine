#pragma once

#include "MeshData.h"

#include <span>

TextureData loadColorTexture(std::span<const std::uint8_t> encoded, float coverageCutoff);
