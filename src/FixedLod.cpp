#include "FixedLod.h"
#include "SourceFingerprint.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace
{
constexpr std::array<char, 8> fileMagic{'F', 'I', 'X', 'E', 'D', 'L', 'O', 'D'};
constexpr std::uint32_t fileVersion = 4;
constexpr std::uint32_t sprayGroupVersion = 5;
constexpr std::uint32_t wholeSprayVersion = 6;
static_assert(sizeof(FixedLodGroup) == 220);
static_assert(sizeof(FixedLodIndexRange) == 8);
static_assert(sizeof(SprayGroupCut) == 24);
static_assert(sizeof(Vertex) == 40);

template<class T>
void readValues(std::istream& file, T* data, std::size_t count = 1)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (!file.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(count * sizeof(T))))
    {
        throw std::runtime_error("Truncated fixed LOD bake.");
    }
}

template<class T>
void writeValues(std::ostream& file, const T* data, std::size_t count = 1)
{
    static_assert(std::is_trivially_copyable_v<T>);
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(count * sizeof(T)));
}

template<class T>
std::vector<T> readArray(std::istream& file, std::size_t maximum)
{
    std::uint32_t count;
    readValues(file, &count);
    if (!count || count > maximum)
    {
        throw std::runtime_error("Invalid fixed LOD array size.");
    }
    std::vector<T> data(count);
    readValues(file, data.data(), data.size());
    return data;
}

template<class T>
void writeArray(std::ostream& file, const std::vector<T>& values)
{
    const auto count = static_cast<std::uint32_t>(values.size());
    writeValues(file, &count);
    writeValues(file, values.data(), values.size());
}

bool validBounds(const Bounds& b)
{
    return std::isfinite(b.center.x) && std::isfinite(b.center.y) && std::isfinite(b.center.z) &&
           std::isfinite(b.halfExtent.x) && b.halfExtent.x >= 0 &&
           std::isfinite(b.halfExtent.y) && b.halfExtent.y >= 0 &&
           std::isfinite(b.halfExtent.z) && b.halfExtent.z >= 0;
}

bool validError(float error)
{
    return std::isfinite(error) && error >= 0;
}

bool finiteVertex(const Vertex& vertex)
{
    const auto finite3 = [](const auto& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    return finite3(vertex.position) && finite3(vertex.normal) &&
           std::all_of(std::begin(vertex.texcoords), std::end(vertex.texcoords), [](const auto& uv) {
               return std::isfinite(uv.x) && std::isfinite(uv.y);
           });
}

void validate(const FixedLod& lod, const MeshData& mesh)
{
    auto components = lod.components;
    std::sort(components.begin(), components.end());
    for (std::size_t id = 0; id < components.size(); ++id)
    {
        if (components[id] != id) throw std::runtime_error("Invalid fixed LOD component IDs.");
    }
    std::size_t totalComponents = 0;
    for (const auto& group : lod.groups)
    {
        if (!group.componentCount || group.componentCount > lod.componentsPerGroup || !validBounds(group.bounds) ||
            (!lod.proxyVertices.empty() && !validBounds(group.proxyBounds)))
        {
            throw std::runtime_error("Invalid fixed LOD group.");
        }
        totalComponents += group.componentCount;
    }
    if (totalComponents != components.size() || lod.levels.front().indices.size() != mesh.indices.size() ||
        !std::all_of(lod.proxyVertices.begin(), lod.proxyVertices.end(), finiteVertex))
    {
        throw std::runtime_error("Incomplete fixed LOD bake.");
    }
    std::size_t previousIndexCount = mesh.indices.size();
    for (std::size_t levelIndex = 0; levelIndex < lod.levels.size(); ++levelIndex)
    {
        const auto& level = lod.levels[levelIndex];
        const auto vertexCount = lod.usesProxyVertices(levelIndex) ? lod.proxyVertices.size() : mesh.vertices.size();
        if (level.indices.size() > previousIndexCount ||
            std::any_of(level.indices.begin(), level.indices.end(), [&](auto index) { return index >= vertexCount; }))
        {
            throw std::runtime_error("Invalid fixed LOD index buffer.");
        }
        previousIndexCount = level.indices.size();
        std::size_t nextIndex = 0;
        for (const auto& cluster : level.clusters)
        {
            if (cluster.firstIndex != nextIndex || !cluster.indexCount || cluster.indexCount % 3 != 0 ||
                cluster.indexCount / 3 > lod.triangleCap)
            {
                throw std::runtime_error("Invalid fixed LOD cluster range.");
            }
            nextIndex += cluster.indexCount;
        }
        std::size_t nextCluster = 0;
        for (const auto& group : lod.groups)
        {
            const auto& range = group.levels[levelIndex];
            if (range.firstCluster != nextCluster || !range.clusterCount || !validError(range.sourceToLevel) ||
                !validError(range.levelToSource) || !validError(range.simplifierError) ||
                !validError(range.rimDeviation))
            {
                throw std::runtime_error("Invalid fixed LOD group range or error.");
            }
            nextCluster += range.clusterCount;
        }
        if (nextIndex != level.indices.size() || nextCluster != level.clusters.size())
        {
            throw std::runtime_error("Incomplete fixed LOD cluster ranges.");
        }
    }
    std::size_t next = 0;
    const auto validateCuts = [&](const std::vector<SprayGroupCut>& group) {
        std::uint32_t previous = 0;
        for (const auto& cut : group)
        {
            const auto count = cut.range.indexCount;
            const auto vertices = cut.fittedVertices ? lod.proxyVertices.size() : mesh.vertices.size();
            if (!validError(cut.sourceError) || !validError(cut.referenceError) ||
                cut.fittedVertices > 1 || cut.tintLevel > fixedLodLevelCount + 2 ||
                cut.range.firstIndex != next || !count || count % 3 != 0 || count < previous ||
                next + count > lod.sprayGroupIndices.size())
            {
                throw std::runtime_error("Invalid fixed LOD spray group cut.");
            }
            const auto indices = std::span(lod.sprayGroupIndices).subspan(next, count);
            if (std::ranges::any_of(indices, [&](auto index) { return index >= vertices; }))
                throw std::runtime_error("Invalid fixed LOD spray group vertex.");
            next += count;
            previous = count;
        }
    };
    for (const auto& group : lod.sprayGroups) validateCuts(group);
    validateCuts(lod.wholeSprayCuts);
    if (next != lod.sprayGroupIndices.size())
        throw std::runtime_error("Incomplete fixed LOD spray group data.");
}
}

FixedLodIndexRange FixedLod::indicesForGroups(std::size_t level, std::size_t firstGroup, std::size_t lastGroup) const
{
    const auto& clusters = levels[level].clusters;
    const auto& first = clusters[groups[firstGroup].levels[level].firstCluster];
    const auto& last = groups[lastGroup].levels[level];
    const auto& lastCluster = clusters[last.firstCluster + last.clusterCount - 1];
    return {first.firstIndex, lastCluster.firstIndex + lastCluster.indexCount - first.firstIndex};
}

std::filesystem::path fixedLodPath(std::filesystem::path source)
{
    return source.replace_extension(L".fixedlod");
}

std::uint64_t fixedLodSourceFingerprint(const std::filesystem::path& source)
{
    return sourceFingerprint(source);
}

FixedLod readFixedLod(const std::filesystem::path& source, const AssetData& asset)
{
    return readFixedLod(source, fixedLodPath(source), asset);
}

FixedLod readFixedLod(const std::filesystem::path& source, const std::filesystem::path& bake, const AssetData& asset)
{
    std::ifstream file(bake, std::ios::binary);
    if (!file) throw std::runtime_error("Fixed LOD bake missing. Rebuild the viewer to generate it.");
    std::array<char, fileMagic.size()> magic{};
    std::uint32_t version;
    std::uint64_t fingerprint;
    readValues(file, magic.data(), magic.size());
    readValues(file, &version);
    readValues(file, &fingerprint);
    if (magic != fileMagic || (version != fileVersion && version != sprayGroupVersion && version != wholeSprayVersion) ||
        fingerprint != fixedLodSourceFingerprint(source))
    {
        throw std::runtime_error("Fixed LOD bake is incompatible with this GLB. Rebuild the viewer.");
    }

    FixedLod lod{};
    readValues(file, &lod.meshIndex);
    readValues(file, &lod.triangleCap);
    readValues(file, &lod.componentsPerGroup);
    readValues(file, &lod.sampleSubdivisions);
    if (lod.meshIndex >= asset.meshes.size() || !lod.triangleCap || lod.triangleCap > 512 ||
        !lod.componentsPerGroup || !lod.sampleSubdivisions)
    {
        throw std::runtime_error("Invalid fixed LOD header.");
    }
    const auto& mesh = asset.meshes[lod.meshIndex];
    const auto maxTriangles = mesh.indices.size() / 3;
    lod.groups = readArray<FixedLodGroup>(file, maxTriangles);
    lod.components = readArray<std::uint32_t>(file, maxTriangles);
    std::uint32_t levelCount;
    readValues(file, &levelCount);
    if (levelCount != fixedLodIndicesOnlyCount && levelCount != fixedLodSharedCoarseLevelCount &&
        levelCount != fixedLodLevelCount)
    {
        throw std::runtime_error("Invalid fixed LOD level count.");
    }
    if (levelCount == fixedLodLevelCount)
    {
        const auto vertices = lod.components.size() * fixedLodProxyVerticesPerComponent;
        if (vertices > std::numeric_limits<std::uint32_t>::max() / sizeof(Vertex))
        {
            throw std::runtime_error("Fixed LOD proxy vertex buffer is too large.");
        }
        lod.proxyVertices = readArray<Vertex>(file, vertices);
        if (lod.proxyVertices.size() != vertices) throw std::runtime_error("Incomplete fixed LOD proxy vertices.");
    }
    lod.levels.resize(levelCount);
    for (auto& level : lod.levels)
    {
        level.indices = readArray<std::uint32_t>(file, mesh.indices.size());
        level.clusters = readArray<FixedLodIndexRange>(file, maxTriangles);
    }
    if (version >= sprayGroupVersion)
    {
        std::uint32_t groupCount;
        readValues(file, &groupCount);
        if (!groupCount || groupCount > maxSprayLodGroups) throw std::runtime_error("Invalid spray group count.");
        lod.sprayGroups.reserve(groupCount);
        for (std::uint32_t i = 0; i < groupCount; ++i)
            lod.sprayGroups.push_back(readArray<SprayGroupCut>(file, maxSprayGroupCuts));
        if (version == wholeSprayVersion) lod.wholeSprayCuts = readArray<SprayGroupCut>(file, maxWholeSprayCuts);
        lod.sprayGroupIndices = readArray<std::uint32_t>(file, mesh.indices.size() * (maxSprayGroupCuts + maxWholeSprayCuts));
    }
    if (file.peek() != std::char_traits<char>::eof())
    {
        throw std::runtime_error("Unexpected data after the fixed LOD bake.");
    }
    validate(lod, mesh);
    return lod;
}

void writeFixedLod(const std::filesystem::path& output, std::uint64_t fingerprint, const FixedLod& lod)
{
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    writeValues(file, fileMagic.data(), fileMagic.size());
    const auto version = !lod.wholeSprayCuts.empty() ? wholeSprayVersion : lod.sprayGroups.empty() ? fileVersion : sprayGroupVersion;
    writeValues(file, &version);
    writeValues(file, &fingerprint);
    writeValues(file, &lod.meshIndex);
    writeValues(file, &lod.triangleCap);
    writeValues(file, &lod.componentsPerGroup);
    writeValues(file, &lod.sampleSubdivisions);
    writeArray(file, lod.groups);
    writeArray(file, lod.components);
    const auto levelCount = static_cast<std::uint32_t>(lod.levels.size());
    writeValues(file, &levelCount);
    if (!lod.proxyVertices.empty()) writeArray(file, lod.proxyVertices);
    for (const auto& level : lod.levels)
    {
        writeArray(file, level.indices);
        writeArray(file, level.clusters);
    }
    if (!lod.sprayGroups.empty())
    {
        const auto groupCount = static_cast<std::uint32_t>(lod.sprayGroups.size());
        writeValues(file, &groupCount);
        for (const auto& group : lod.sprayGroups) writeArray(file, group);
        if (!lod.wholeSprayCuts.empty()) writeArray(file, lod.wholeSprayCuts);
        writeArray(file, lod.sprayGroupIndices);
    }
    file.close();
}
