#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 texcoords[2];
};

struct Bounds
{
    DirectX::XMFLOAT3 center;
    DirectX::XMFLOAT3 halfExtent;
};

struct MeshData
{
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    DirectX::XMFLOAT4X4 localToWorld;
    Bounds localBounds{};
    std::size_t material = 0;
    std::uint32_t uvMask = 0;
};

struct MaterialData
{
    std::string name = "Default";
    DirectX::XMFLOAT4 color{0.85f, 0.68f, 0.40f, 1.0f};
    std::optional<std::size_t> colorTexture;
    std::uint32_t textureCoordinateSet = 0;
    float alphaCutoff = -1.0f;
    float transmission = 0.0f;
    bool hasBacklighting = false;
    bool twoSidedTransmission = false;
};

struct TextureMip
{
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::uint8_t> rgba;
};

using TextureData = std::vector<TextureMip>;

struct TextureAsset
{
    std::string name;
    TextureData mips;
};

struct AssetData
{
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<TextureAsset> textures;
};
