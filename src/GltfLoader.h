#pragma once

#include "MeshData.h"

#include <filesystem>

AssetData loadGlb(const std::filesystem::path& path);
