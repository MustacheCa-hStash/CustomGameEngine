#pragma once

#include "MeshData.h"

#include <array>
#include <filesystem>

using SkyIrradiance = std::array<DirectX::XMFLOAT4, 9>;

struct SkyLighting
{
    TextureData radianceMips;
    SkyIrradiance irradiance;
    float radianceScale = 1;
};

// Load an equirectangular RGBE dome into linear RGBA16F and project its upper hemisphere.
SkyLighting loadSkyLighting(const std::filesystem::path& panorama);
