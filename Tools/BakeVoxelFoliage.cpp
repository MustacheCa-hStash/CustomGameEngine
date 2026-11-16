#include "GltfLoader.h"
#include "FixedLod.h"
#include "SourceFingerprint.h"
#include "VoxelBake.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{
struct Options
{
    double cellSize = 0;
    unsigned levels = 3;
    unsigned samplesPerSide = 8;
    std::vector<double> cutoffs;
    bool cellReport = false;
    std::filesystem::path normalMask;
    std::filesystem::path regionMask;
};

double number(std::wstring_view value)
{
    std::size_t consumed;
    const double result = std::stod(std::wstring(value), &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result <= 0)
    {
        throw std::runtime_error("Expected a finite positive number.");
    }
    return result;
}

unsigned count(std::wstring_view value, unsigned limit)
{
    const double result = number(value);
    if (result > limit || std::floor(result) != result) throw std::runtime_error("Count is outside the supported range.");
    return static_cast<unsigned>(result);
}

Options parseOptions(int argc, wchar_t* argv[])
{
    Options options;
    for (int i = 4; i < argc; ++i)
    {
        const std::wstring_view option = argv[i];
        if (option == L"--cell-report")
        {
            options.cellReport = true;
            continue;
        }
        if (++i == argc) throw std::runtime_error("Missing voxel bake option value.");
        const std::wstring_view value = argv[i];
        if (option == L"--normal-mask") options.normalMask = value;
        else if (option == L"--region-mask") options.regionMask = value;
        else if (option == L"--cell-size") options.cellSize = number(value);
        else if (option == L"--levels") options.levels = count(value, 16);
        else if (option == L"--samples") options.samplesPerSide = count(value, 128);
        else if (option == L"--cutoff")
        {
            const double cutoff = number(value);
            if (cutoff > 1) throw std::runtime_error("Coverage cutoff must be at most one.");
            options.cutoffs.push_back(cutoff);
        }
        else throw std::runtime_error("Unknown voxel bake option.");
    }
    if (static_cast<int>(options.cellSize > 0) + !options.normalMask.empty() + !options.regionMask.empty() != 1)
        throw std::runtime_error("Specify exactly one of --cell-size, --normal-mask, or --region-mask.");
    if (options.cutoffs.empty()) options.cutoffs = {0.05, 0.10, 0.20};
    std::sort(options.cutoffs.begin(), options.cutoffs.end());
    if (std::adjacent_find(options.cutoffs.begin(), options.cutoffs.end()) != options.cutoffs.end())
    {
        throw std::runtime_error("Coverage cutoffs must be distinct.");
    }
    return options;
}

void writeCellReport(const std::filesystem::path& path, const VoxelBake& bake)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.imbue(std::locale::classic());
    out << "cell_x,cell_y,cell_z,mean_coverage";
    for (std::size_t i = 0; i < voxelProbeDirections.size(); ++i)
    {
        const auto& d = voxelProbeDirections[i];
        out << ",hits_" << d[0] << '_' << d[1] << '_' << d[2] << "_of_" << bake.raysThroughCell[i];
    }
    out << '\n' << std::fixed << std::setprecision(9);
    for (const auto& cell : bake.cells)
    {
        out << cell.coordinate[0] << ',' << cell.coordinate[1] << ',' << cell.coordinate[2] << ',' << bake.meanCoverage(cell);
        for (const auto hits : cell.hits) out << ',' << hits;
        out << '\n';
    }
    out.close();
}

void emitVariant(std::ostream& report, const std::filesystem::path& output, const VoxelBake& bake,
                 std::uint64_t fingerprint, std::uint32_t meshIndex, std::optional<double> cutoff)
{
    const auto foliage = packVoxelFoliage(bake, fingerprint, meshIndex, cutoff);
    writeVoxelFoliage(output, foliage);
    if (readVoxelFoliage(output) != foliage) throw std::runtime_error("Voxel foliage bake round-trip mismatch.");
    std::size_t occupiedCells = 0;
    for (const auto& brick : foliage.bricks) occupiedCells += std::popcount(brick.occupancy);
    report << "  " << output.filename().string();
    if (cutoff) report << "   cutoff " << *cutoff;
    else report << "   any overlap";
    report << "   " << foliage.bricks.size() << " bricks   " << occupiedCells << " cells   "
           << std::filesystem::file_size(output) << " bytes\n";
}

void emitLevel(std::ostream& report, const std::filesystem::path& directory, const MeshData& mesh,
               const Options& options, unsigned level, std::uint64_t fingerprint, std::uint32_t meshIndex)
{
    const auto start = std::chrono::steady_clock::now();
    const auto bake = sampleVoxelCoverage(mesh, std::ldexp(options.cellSize, level), options.samplesPerSide);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    const auto prefix = "level-" + std::to_string(level);
    report << "\nLevel " << level << "   Cell size " << bake.cellSize << " mesh units\n"
           << "  " << bake.cells.size() << " overlap candidates, " << bake.degenerateTriangles << " degenerate triangles\n"
           << "  Sampling time " << elapsed.count() << " ms\n";
    std::array<std::uint64_t, voxelProbeDirections.size()> hits{};
    std::size_t unsampled = 0;
    for (const auto& cell : bake.cells)
    {
        if (cell.hits == VoxelProbeCounts{}) ++unsampled;
        for (std::size_t i = 0; i < hits.size(); ++i) hits[i] += cell.hits[i];
    }
    report << "  " << unsampled << " overlap cells with no sampled hits\n";
    for (std::size_t i = 0; i < hits.size(); ++i)
    {
        const auto& d = voxelProbeDirections[i];
        report << "  Direction (" << d[0] << ' ' << d[1] << ' ' << d[2] << ")   " << hits[i]
               << " hits / " << std::uint64_t{bake.raysThroughCell[i]} * bake.cells.size() << " rays\n";
    }
    emitVariant(report, directory / (prefix + "-overlap.voxels"), bake, fingerprint, meshIndex, std::nullopt);
    for (std::size_t i = 0; i < options.cutoffs.size(); ++i)
    {
        const auto name = prefix + "-cutoff-" + std::to_string(i + 1) + ".voxels";
        emitVariant(report, directory / name, bake, fingerprint, meshIndex, options.cutoffs[i]);
    }
    if (options.cellReport) writeCellReport(directory / (prefix + "-coverage.csv"), bake);
}
}

int wmain(int argc, wchar_t* argv[])
{
    try
    {
        if (argc < 4)
        {
            throw std::runtime_error("Usage: BakeVoxelFoliage source.glb \"mesh name\" output-directory "
                                     "[--cell-size N | --normal-mask occupancy.voxels | --region-mask occupancy.voxels] "
                                     "[--levels N] [--samples N] [--cutoff N ...] [--cell-report]");
        }
        const auto options = parseOptions(argc, argv);
        const std::filesystem::path source = argv[1];
        const std::filesystem::path directory = argv[3];
        const auto asset = loadGlb(source);
        const auto name = std::filesystem::path(argv[2]).u8string();
        const std::string_view meshName(reinterpret_cast<const char*>(name.data()), name.size());
        const auto named = [&](const MeshData& mesh) { return mesh.name == meshName; };
        const auto mesh = std::find_if(asset.meshes.begin(), asset.meshes.end(), named);
        if (mesh == asset.meshes.end() || std::count_if(asset.meshes.begin(), asset.meshes.end(), named) != 1)
        {
            throw std::runtime_error("Specify a unique mesh name from the source GLB.");
        }
        const auto distance = mesh - asset.meshes.begin();
        if (std::cmp_greater(distance, std::numeric_limits<std::uint32_t>::max()))
        {
            throw std::runtime_error("Source mesh index exceeds the voxel foliage format.");
        }
        const auto index = static_cast<std::uint32_t>(distance);
        const auto fingerprint = sourceFingerprint(source);
        if (!options.regionMask.empty())
        {
            const auto mask = readVoxelFoliage(options.regionMask);
            const auto lod = readFixedLod(source, asset);
            if (mask.sourceFingerprint != fingerprint || mask.meshIndex != index || lod.meshIndex != index ||
                mask.bricks.empty() || mask.origin != std::array<double, 3>{} || lod.groups.size() < voxelBranchRegionCount)
                throw std::runtime_error("Region bake needs the matching voxel mask and source triangle groups.");
            std::vector<VoxelBake> samples;
            MeshData regionMesh{};
            regionMesh.vertices = mesh->vertices;
            for (std::size_t region = 0; region < voxelBranchRegionCount; ++region)
            {
                const auto first = voxelRegionGroupStart(region, lod.groups.size());
                const auto end = voxelRegionGroupStart(region + 1, lod.groups.size());
                const auto range = lod.indicesForGroups(0, first, end - 1);
                const auto begin = lod.levels[0].indices.begin() + range.firstIndex;
                regionMesh.indices.assign(begin, begin + range.indexCount);
                samples.push_back(sampleVoxelCoverage(regionMesh, mask.cellSize, options.samplesPerSide));
            }
            const auto owners = assignVoxelRegions(mask, samples);
            const auto maskHash = sourceFingerprint(options.regionMask);
            const auto lodHash = sourceFingerprint(fixedLodPath(source));
            std::filesystem::create_directories(directory);
            const auto output = directory / "voxel-regions.bin";
            writeVoxelRegionOwners(output, maskHash, lodHash, owners);
            if (readVoxelRegionOwners(output, maskHash, lodHash, occupiedVoxelCount(mask)) != owners)
                throw std::runtime_error("Voxel regions did not round trip.");
            for (std::size_t region = 0; region < voxelBranchRegionCount; ++region)
                std::cout << "Region " << region << ": " << std::ranges::count(owners, region) << " cells\n";
            return 0;
        }
        if (!options.normalMask.empty())
        {
            const auto mask = readVoxelFoliage(options.normalMask);
            if (mask.sourceFingerprint != fingerprint || mask.meshIndex != index || mask.bricks.empty())
                throw std::runtime_error("Normal mask does not match the requested source mesh.");
            const auto maskHash = sourceFingerprint(options.normalMask);
            const auto result = bakeVoxelNormals(*mesh, mask, options.samplesPerSide);
            std::filesystem::create_directories(directory);
            const auto output = directory / "voxel-normals.bin";
            writeVoxelNormals(output, maskHash, result.cells);
            const auto decoded = readVoxelNormals(output, maskHash, occupiedVoxelCount(mask));
            if (!decoded.hasDirectionalWeights || !decoded.hasShadowReceivers || decoded.cells != result.cells)
                throw std::runtime_error("Voxel normals round-trip mismatch.");
            std::cout << "Baked two weighted normal axes for " << result.cells.size() << " occupied cells ("
                      << result.fallbackCells << " cells used an overlapping source face as fallback).\n";
            return 0;
        }

        std::ostringstream report;
        report.imbue(std::locale::classic());
        report << std::fixed << std::setprecision(9)
               << "Voxel foliage bake\nSource GLB FNV-1a " << std::hex << fingerprint << std::dec
               << "\nMesh " << mesh->name << " (" << index << ")   " << mesh->indices.size() / 3 << " triangles\n"
               << "Mesh-local grid origin (0 0 0)\nProbe lattice " << options.samplesPerSide << " x "
               << options.samplesPerSide << " per direction\n"
               << "Coverage is the mean of seven directional hit fractions. Source triangles are treated as opaque.\n";
        std::filesystem::create_directories(directory);
        for (unsigned level = 0; level < options.levels; ++level)
        {
            emitLevel(report, directory, *mesh, options, level, fingerprint, index);
        }
        std::ofstream summary(directory / "voxel-bake.txt", std::ios::binary | std::ios::trunc);
        summary.exceptions(std::ios::failbit | std::ios::badbit);
        summary << report.str();
        summary.close();
        std::cout << report.str();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "BakeVoxelFoliage: " << e.what() << '\n';
        return 1;
    }
}
