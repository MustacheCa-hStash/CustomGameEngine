#include "VoxelFoliage.h"

#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace
{
constexpr std::array<char, 8> magic{'V', 'O', 'X', 'E', 'L', 'F', 'O', 'L'};
constexpr std::uint32_t version = 1;
constexpr std::uintmax_t headerBytes = 60;
constexpr std::uintmax_t brickBytes = 20;
constexpr std::array<char, 8> legacyNormalMagic{'V', 'O', 'X', 'N', 'R', 'M', '0', '1'};
constexpr std::array<char, 8> directionalNormalMagic{'V', 'O', 'X', 'N', 'R', 'M', '0', '2'};
constexpr std::array<char, 8> normalMagic{'V', 'O', 'X', 'N', 'R', 'M', '0', '3'};
constexpr std::array<char, 8> regionMagic{'V', 'O', 'X', 'R', 'E', 'G', '0', '1'};
static_assert(std::endian::native == std::endian::little && std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);

template<class T>
void write(std::ostream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template<class T>
void read(std::istream& in, T& value)
{
    if (!in.read(reinterpret_cast<char*>(&value), sizeof(T))) throw std::runtime_error("Truncated voxel foliage file.");
}

void validate(const VoxelFoliage& foliage)
{
    const auto size = foliage.cellSize;
    if (!std::isfinite(size) || size <= 0 || size > std::numeric_limits<double>::max() / 0x1p31)
    {
        throw std::runtime_error("Invalid voxel cell size.");
    }
    for (double origin : foliage.origin)
    {
        if (!std::isfinite(origin)) throw std::runtime_error("Invalid voxel grid origin.");
    }
    for (std::size_t i = 0; i < foliage.bricks.size(); ++i)
    {
        const auto& brick = foliage.bricks[i];
        if (brick.occupancy == 0 || (i > 0 && !(foliage.bricks[i - 1].coordinate < brick.coordinate)))
        {
            throw std::runtime_error("Voxel bricks must be nonempty and sorted with unique coordinates.");
        }
        for (auto axis : brick.coordinate)
        {
            if (axis < std::numeric_limits<std::int32_t>::min() / voxelBrickSide ||
                axis > std::numeric_limits<std::int32_t>::max() / voxelBrickSide)
            {
                throw std::runtime_error("Voxel brick is outside the signed cell coordinate range.");
            }
        }
    }
}
}

VoxelFoliage readVoxelFoliage(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    std::ifstream in(path, std::ios::binary);
    std::array<char, magic.size()> fileMagic;
    std::uint32_t fileVersion;
    read(in, fileMagic);
    read(in, fileVersion);
    if (fileMagic != magic || fileVersion != version) throw std::runtime_error("Unsupported voxel foliage format.");

    VoxelFoliage foliage;
    std::uint32_t count;
    read(in, foliage.sourceFingerprint);
    read(in, foliage.meshIndex);
    for (auto& axis : foliage.origin) read(in, axis);
    read(in, foliage.cellSize);
    read(in, count);
    if (size != headerBytes + brickBytes * count) throw std::runtime_error("Invalid voxel foliage file length.");
    foliage.bricks.resize(count);
    for (auto& brick : foliage.bricks)
    {
        for (auto& axis : brick.coordinate) read(in, axis);
        read(in, brick.occupancy);
    }
    validate(foliage);
    return foliage;
}

void writeVoxelFoliage(const std::filesystem::path& path, const VoxelFoliage& foliage)
{
    validate(foliage);
    if (foliage.bricks.size() > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::runtime_error("Too many voxel bricks for the file format.");
    }
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    write(out, magic);
    write(out, version);
    write(out, foliage.sourceFingerprint);
    write(out, foliage.meshIndex);
    for (auto axis : foliage.origin) write(out, axis);
    write(out, foliage.cellSize);
    write(out, static_cast<std::uint32_t>(foliage.bricks.size()));
    for (const auto& brick : foliage.bricks)
    {
        for (auto axis : brick.coordinate) write(out, axis);
        write(out, brick.occupancy);
    }
    out.close();
    std::filesystem::rename(temporary, path);
}

std::size_t occupiedVoxelCount(const VoxelFoliage& foliage)
{
    std::size_t count = 0;
    for (const auto& brick : foliage.bricks) count += std::popcount(brick.occupancy);
    return count;
}

VoxelNormalData readVoxelNormals(const std::filesystem::path& path, std::uint64_t maskHash, std::size_t cellCount)
{
    std::ifstream in(path, std::ios::binary);
    std::array<char, 8> fileMagic;
    std::uint64_t fileHash;
    std::uint32_t count;
    read(in, fileMagic);
    read(in, fileHash);
    read(in, count);
    const bool receivers = fileMagic == normalMagic;
    const bool directional = receivers || fileMagic == directionalNormalMagic;
    const auto words = directional ? std::tuple_size_v<PackedVoxelNormals> : 2;
    if ((!directional && fileMagic != legacyNormalMagic) || fileHash != maskHash || count != cellCount ||
        std::filesystem::file_size(path) != 20 + std::uintmax_t(count) * words * sizeof(std::uint32_t))
        throw std::runtime_error("Voxel normals do not match the occupancy bake.");
    VoxelNormalData result{std::vector<PackedVoxelNormals>(count), directional, receivers};
    for (auto& cell : result.cells)
        for (std::size_t word = 0; word < words; ++word) read(in, cell[word]);
    return result;
}

void writeVoxelNormals(const std::filesystem::path& path, std::uint64_t maskHash, std::span<const PackedVoxelNormals> cells)
{
    if (cells.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Too many occupied voxels for the normal format.");
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    write(out, normalMagic);
    write(out, maskHash);
    write(out, static_cast<std::uint32_t>(cells.size()));
    for (const auto& cell : cells) write(out, cell);
    out.close();
    std::filesystem::rename(temporary, path);
}

std::vector<std::uint8_t> readVoxelRegionOwners(const std::filesystem::path& path, std::uint64_t maskHash,
                                               std::uint64_t lodHash, std::size_t cellCount)
{
    std::ifstream in(path, std::ios::binary);
    std::array<char, 8> fileMagic;
    std::uint64_t mask, lod;
    read(in, fileMagic);
    read(in, mask);
    read(in, lod);
    if (fileMagic != regionMagic || mask != maskHash || lod != lodHash ||
        std::filesystem::file_size(path) != 24 + cellCount)
        throw std::runtime_error("Voxel regions do not match the voxel mask and triangle groups.");
    std::vector<std::uint8_t> owners(cellCount);
    for (auto& region : owners)
    {
        read(in, region);
        if (region >= voxelBranchRegionCount) throw std::runtime_error("Invalid voxel region owner.");
    }
    return owners;
}

void writeVoxelRegionOwners(const std::filesystem::path& path, std::uint64_t maskHash, std::uint64_t lodHash,
                           std::span<const std::uint8_t> owners)
{
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    write(out, regionMagic);
    write(out, maskHash);
    write(out, lodHash);
    for (const auto owner : owners) write(out, owner);
    out.close();
    std::filesystem::rename(temporary, path);
}
