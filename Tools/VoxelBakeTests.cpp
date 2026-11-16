#include "VoxelBake.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using DirectX::XMFLOAT3;

namespace
{
void require(bool condition, const char* failure)
{
    if (!condition) throw std::runtime_error(failure);
}

void addTriangle(MeshData& mesh, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c)
{
    for (auto point : {a, b, c})
    {
        mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
        Vertex vertex{};
        vertex.position = point;
        mesh.vertices.push_back(vertex);
    }
}

void addQuad(MeshData& mesh, float left, float right, float bottom, float top, float z)
{
    addTriangle(mesh, {left, bottom, z}, {right, bottom, z}, {right, top, z});
    addTriangle(mesh, {left, bottom, z}, {right, top, z}, {left, top, z});
}

bool occupied(const VoxelFoliage& foliage, int x, int y, int z)
{
    // Decode the file layout independently of the packer's coordinate conversion.
    for (const auto& brick : foliage.bricks)
    {
        const int u = x - 4 * brick.coordinate[0];
        const int v = y - 4 * brick.coordinate[1];
        const int w = z - 4 * brick.coordinate[2];
        if (u >= 0 && u < 4 && v >= 0 && v < 4 && w >= 0 && w < 4)
        {
            return (brick.occupancy >> (u + 4 * v + 16 * w)) & 1;
        }
    }
    return false;
}

void checkCandidateCells()
{
    MeshData mesh{};
    require(sampleVoxelCoverage(mesh, 1, 8).cells.empty(), "An empty mesh produced voxel cells.");
    addTriangle(mesh, {0.125f, 0.125f, 0.5f}, {2.625f, 0.125f, 0.5f}, {0.125f, 2.625f, 0.5f});
    addTriangle(mesh, {20, 20, 20}, {21, 21, 21}, {22, 22, 22});
    const auto bake = sampleVoxelCoverage(mesh, 1, 8);
    constexpr std::array<VoxelCoordinate, 6> expected{{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}}};
    require(bake.degenerateTriangles == 1, "Zero-area triangle was not counted separately.");
    require(std::ranges::equal(bake.cells, expected, {}, &VoxelBakeCell::coordinate),
            "Diagonal triangle candidate set differs from the hand-calculated result.");
}

void checkCoverageAndGridBoundary()
{
    MeshData mesh{};
    addQuad(mesh, 0, 0.5f, 0, 1, 0);
    addQuad(mesh, 0, 0.5f, 0, 1, 0);
    const auto bake = sampleVoxelCoverage(mesh, 1, 8);
    require(bake.cells.size() == 1 && bake.cells.front().coordinate == VoxelCoordinate{0, 0, 0},
            "A surface on a shared grid plane did not get a single positive-side owner.");
    const auto& cell = bake.cells.front();
    require(bake.raysThroughCell[2] == 64 && cell.hits[2] == 32 && cell.hits[0] == 0 && cell.hits[1] == 0,
            "Half-covered face, parallel rays, or duplicate triangles gave the wrong hit count.");
    const auto finer = sampleVoxelCoverage(mesh, 1, 16);
    require(finer.cells.front().hits[2] == 128, "Doubling probe density changed the known half coverage.");
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) std::swap(mesh.indices[i], mesh.indices[i + 1]);
    require(sampleVoxelCoverage(mesh, 1, 8).cells.front().hits == cell.hits, "Changing winding changed opaque coverage.");
    const double coverage = bake.meanCoverage(cell);
    require(packVoxelFoliage(bake, 0, 0, coverage).bricks.size() == 1, "Coverage cutoff discarded an exact tie.");
    require(packVoxelFoliage(bake, 0, 0, coverage + 0.01).bricks.empty(), "Coverage cutoff kept a below-cutoff cell.");
}

void checkRayClipping()
{
    MeshData mesh{};
    addTriangle(mesh, {0, 0, 0}, {1, 0, 2}, {1, 1, 2});
    addTriangle(mesh, {0, 0, 0}, {1, 1, 2}, {0, 1, 0});
    const auto bake = sampleVoxelCoverage(mesh, 1, 8);
    require(bake.cells.size() == 2, "Tilted sheet should cross exactly two cells.");
    require(bake.cells[0].hits[2] == 32 && bake.cells[1].hits[2] == 32,
            "A ray hit the tilted sheet outside the sampled cell's depth interval.");
}

VoxelFoliage checkLayeredPatches()
{
    MeshData mesh{};
    addQuad(mesh, 0, 2, 0, 1, 0.5f);
    addQuad(mesh, 1, 3, 0, 1, 1.5f);
    addQuad(mesh, -1, 0, -1, 0, -0.5f);
    const auto bake = sampleVoxelCoverage(mesh, 1, 8);
    const auto foliage = packVoxelFoliage(bake, 0x0123456789abcdef, 2, 0.2);
    require(foliage.bricks.size() == 2, "Sparse pack wrote an unexpected brick count.");
    require(foliage.bricks.front() == VoxelBrick{{-1, -1, -1}, std::uint64_t{1} << 63},
            "Negative cell coordinates did not map to the expected brick and bit.");
    std::array<int, 4> fromFront{}, fromBack{};
    for (int x = 0; x < 4; ++x)
    {
        const bool front = occupied(foliage, x, 0, 0);
        const bool back = occupied(foliage, x, 0, 1);
        // Each column is an axial view ray through the center of its cell.
        fromFront[x] = front ? 1 : back ? 2 : 0;
        fromBack[x] = back ? 2 : front ? 1 : 0;
    }
    require(fromFront == std::array{1, 1, 2, 0} && fromBack == std::array{1, 2, 2, 0},
            "Layered patches did not preserve the expected gaps and first hits from opposite sides.");
    return foliage;
}

void checkNormalFit(const std::filesystem::path& path)
{
    MeshData mesh{};
    addQuad(mesh, 0, 1, 0, 1, 0.5f);
    const VoxelFoliage mask{1, 0, {}, 1, {{{0, 0, 0}, 1}}};
    const auto planar = bakeVoxelNormals(mesh, mask, 8);
    constexpr std::uint32_t positiveZ = 2048 | (2048 << 12);
    require(planar.cells.size() == 1 && planar.cells[0][0] == (positiveZ | (255u << 24)),
            "A flat patch should store its single normal axis with full weight.");
    const auto receiver = planar.cells[0][3] >> 16;
    require((receiver >> 15) == 1 && ((receiver >> 10) & 31) == 16,
            "The flat patch's receiver should be on the leaf plane at half cell depth.");

    addTriangle(mesh, {0.5f, 0, 0}, {0.5f, 1, 0}, {0.5f, 1, 1});
    addTriangle(mesh, {0.5f, 0, 0}, {0.5f, 1, 1}, {0.5f, 0, 1});
    const auto crossed = bakeVoxelNormals(mesh, mask, 8);
    const auto first = crossed.cells[0][0];
    const auto second = crossed.cells[0][1];
    constexpr std::uint32_t positiveX = 4095 | (2048 << 12);
    require((first >> 24) > 0 && (first >> 24) < 255 &&
            (((first & 0xFFFFFF) == positiveZ && second == positiveX) ||
             ((first & 0xFFFFFF) == positiveX && second == positiveZ)),
            "Crossed patches should retain both axes with nonzero weights.");
    const auto plusX = (crossed.cells[0][2] >> 8) & 255;
    const auto plusZ = (crossed.cells[0][3] >> 8) & 255;
    const bool primaryIsX = (first & 0xFFFFFF) == positiveX;
    require(primaryIsX ? plusX > 200 && plusZ < 55 : plusZ > 200 && plusX < 55,
            "Axial probe weights should select the patch that faces each probe.");
    writeVoxelNormals(path, 17, crossed.cells);
    const auto loaded = readVoxelNormals(path, 17, 1);
    require(loaded.hasDirectionalWeights && loaded.hasShadowReceivers && loaded.cells == crossed.cells,
            "Packed normals and shadow receivers did not round trip.");
    bool rejected = false;
    try { readVoxelNormals(path, 18, 1); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "The normal reader accepted a different occupancy fingerprint.");
    std::filesystem::remove(path);
}
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        require(argc == 2, "A fixture output path is required.");
        checkCandidateCells();
        checkCoverageAndGridBoundary();
        checkRayClipping();
        const auto foliage = checkLayeredPatches();
        const std::filesystem::path path = argv[1];
        auto normalPath = path;
        normalPath.replace_extension(".normals");
        checkNormalFit(normalPath);
        writeVoxelFoliage(path, foliage);
        require(readVoxelFoliage(path) == foliage, "Voxel asset did not round trip.");
        require(std::filesystem::file_size(path) == 100, "Voxel asset layout or record size changed.");
        std::filesystem::resize_file(path, 99);
        bool rejected = false;
        try { readVoxelFoliage(path); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Voxel asset reader accepted a truncated brick.");
        std::filesystem::remove(path);
        std::cout << "Voxel bake fixtures passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "VoxelBakeTests: " << error.what() << '\n';
        return 1;
    }
}
