#include "TreeHierarchy.h"
#include "SourceFingerprint.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace
{
constexpr std::array<char, 8> magic{'T', 'R', 'E', 'E', 'L', 'O', 'D', '2'};
constexpr std::array<char, 8> independentOrderMagic{'T', 'R', 'E', 'E', 'L', 'O', 'D', '3'};
constexpr std::array<char, 8> instancedMagic{'T', 'R', 'E', 'E', 'L', 'O', 'D', '4'};
static_assert(sizeof(TreeLodNode) == 56);
static_assert(sizeof(Vertex) == 40);

template<class T>
void read(std::istream& file, T* data, std::size_t count = 1)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (!file.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(count * sizeof(T))))
        throw std::runtime_error("Truncated tree LOD hierarchy.");
}

template<class T>
void write(std::ostream& file, const T* data, std::size_t count = 1)
{
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(count * sizeof(T)));
}

template<class T>
std::vector<T> readArray(std::istream& file, std::size_t maximum)
{
    std::uint32_t count;
    read(file, &count);
    if (count > maximum || count > std::numeric_limits<std::uint32_t>::max() / sizeof(T))
        throw std::runtime_error("Invalid tree LOD array size.");
    std::vector<T> result(count);
    if (count) read(file, result.data(), count);
    return result;
}

template<class T>
void writeArray(std::ostream& file, const std::vector<T>& data)
{
    const auto count = static_cast<std::uint32_t>(data.size());
    write(file, &count);
    if (count) write(file, data.data(), count);
}

bool finite3(const DirectX::XMFLOAT3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void validatePart(const TreeLodPart& part, std::size_t placements)
{
    const auto& nodes = part.nodes;
    if (nodes.empty() || nodes[0].firstPlacement || nodes[0].placementCount != placements)
        throw std::runtime_error("Tree LOD root does not cover its source geometry.");
    std::vector<bool> hasParent(nodes.size());
    hasParent[0] = true;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const auto& n = nodes[i];
        const auto end = n.firstPlacement + static_cast<std::size_t>(n.placementCount);
        if (!hasParent[i] || !n.placementCount || end > placements || !std::isfinite(n.error) || n.error < 0 ||
            !std::isfinite(n.referenceError) || n.referenceError < 0 ||
            !finite3(n.bounds.center) || !finite3(n.bounds.halfExtent) ||
            n.bounds.halfExtent.x < 0 || n.bounds.halfExtent.y < 0 || n.bounds.halfExtent.z < 0 ||
            n.geometry.firstIndex % 3 || n.geometry.indexCount % 3 ||
            n.geometry.firstIndex + static_cast<std::size_t>(n.geometry.indexCount) > part.geometry.indices.size())
            throw std::runtime_error("Invalid tree LOD node.");
        if (!n.childCount) continue;
        if (n.firstChild <= i || n.firstChild + static_cast<std::size_t>(n.childCount) > nodes.size())
            throw std::runtime_error("Invalid tree LOD children.");
        auto first = static_cast<std::size_t>(n.firstPlacement);
        const bool alternatives = !part.instancedNodes.empty() && part.instancedNodes[i] >= 2;
        for (std::size_t child = n.firstChild; child < n.firstChild + static_cast<std::size_t>(n.childCount); ++child)
        {
            if (hasParent[child] || nodes[child].firstPlacement != (alternatives ? n.firstPlacement : first) ||
                (alternatives && nodes[child].placementCount != n.placementCount))
                throw std::runtime_error("Tree LOD children overlap or leave a gap.");
            hasParent[child] = true;
            first += nodes[child].placementCount;
        }
        if (!alternatives && first != end) throw std::runtime_error("Tree LOD children do not cover the parent.");
    }
    if (std::ranges::any_of(part.geometry.indices, [&](auto index) { return index >= part.geometry.vertices.size(); }))
        throw std::runtime_error("Invalid tree LOD vertex index.");
    if (std::ranges::any_of(part.geometry.vertices, [](const Vertex& v) {
        return !finite3(v.position) || !finite3(v.normal) ||
               !std::isfinite(v.texcoords[0].x) || !std::isfinite(v.texcoords[0].y) ||
               !std::isfinite(v.texcoords[1].x) || !std::isfinite(v.texcoords[1].y);
    })) throw std::runtime_error("Non-finite tree LOD vertex.");
}
}

std::filesystem::path treeHierarchyPath(std::filesystem::path glb)
{
    return glb.replace_extension(L".treelod");
}

TreeHierarchy readTreeHierarchy(const SprayBoughSources& sources, const AssetData& asset)
{
    std::ifstream file(treeHierarchyPath(sources[0]), std::ios::binary);
    if (!file) throw std::runtime_error("Tree LOD hierarchy bake missing.");
    auto signature = magic;
    read(file, signature.data(), signature.size());
    if (signature != magic && signature != independentOrderMagic && signature != instancedMagic)
        throw std::runtime_error("Invalid tree LOD header.");
    for (const auto& source : sources)
    {
        std::uint64_t hash;
        read(file, &hash);
        if (hash != sourceFingerprint(source))
            throw std::runtime_error("Tree LOD hierarchy does not match this tree or its spray bakes.");
    }
    std::size_t maxPlacements = 0;
    for (const auto& mesh : asset.meshes)
        if (mesh.instances) maxPlacements = std::max(maxPlacements, mesh.instances->size());
    TreeHierarchy hierarchy;
    hierarchy.placementOrder = readArray<std::uint32_t>(file, maxPlacements);
    auto sorted = hierarchy.placementOrder;
    std::ranges::sort(sorted);
    for (std::size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i] != i) throw std::runtime_error("Invalid tree LOD placement order.");
    std::uint32_t count;
    read(file, &count);
    if (!count || count > asset.meshes.size()) throw std::runtime_error("Invalid tree LOD part count.");
    std::vector<bool> seen(asset.meshes.size());
    for (std::uint32_t i = 0; i < count; ++i)
    {
        auto& part = hierarchy.parts.emplace_back();
        read(file, &part.meshIndex);
        if (part.meshIndex >= asset.meshes.size() || seen[part.meshIndex])
            throw std::runtime_error("Invalid tree LOD source mesh.");
        seen[part.meshIndex] = true;
        const auto& source = asset.meshes[part.meshIndex];
        const auto placements = source.instances ? source.instances->size() : 1;
        if (source.instances && placements != hierarchy.placementOrder.size())
            throw std::runtime_error("Tree LOD placement count differs from its source mesh.");
        part.nodes = readArray<TreeLodNode>(file, placements * 2 + 32);
        const auto maximum = std::max(source.indices.size(), source.vertices.size()) * placements * 4;
        part.geometry.vertices = readArray<Vertex>(file, maximum);
        part.geometry.indices = readArray<std::uint32_t>(file, maximum);
        part.geometry.material = source.material;
        part.geometry.uvMask = source.uvMask;
        DirectX::XMStoreFloat4x4(&part.geometry.localToWorld, DirectX::XMMatrixIdentity());
        if (!source.instances) part.geometry.localToWorld = source.localToWorld;
    }
    if (signature != magic)
        for (auto& part : hierarchy.parts)
        {
            part.placementOrder = readArray<std::uint32_t>(file, maxPlacements);
            if (part.placementOrder.empty()) continue;
            auto ids = part.placementOrder;
            std::ranges::sort(ids);
            if (!asset.meshes[part.meshIndex].instances || ids != sorted)
                throw std::runtime_error("Invalid per-part tree placement order.");
        }
    if (signature == instancedMagic)
        for (auto& part : hierarchy.parts)
        {
            auto& flags = part.instancedNodes;
            flags = readArray<std::uint8_t>(file, part.nodes.size());
            if (flags.empty()) continue;
            if (!asset.meshes[part.meshIndex].instances || flags.size() != part.nodes.size())
                throw std::runtime_error("Invalid instanced tree node count.");
            for (std::size_t i = 0; i < flags.size(); ++i)
                if (flags[i] > 3 || (flags[i] == 1 && !part.nodes[i].geometry.indexCount) ||
                    (flags[i] >= 2 && (part.nodes[i].childCount != 2 || part.nodes[i].geometry.indexCount)))
                    throw std::runtime_error("Invalid instanced tree geometry range.");
        }
    for (const auto& part : hierarchy.parts)
    {
        const auto& instances = asset.meshes[part.meshIndex].instances;
        validatePart(part, instances ? instances->size() : 1);
    }
    if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected data after tree hierarchy.");
    return hierarchy;
}

void writeTreeHierarchy(const std::filesystem::path& path, const SprayBoughSources& sources,
                        const TreeHierarchy& hierarchy)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    const bool independent = std::ranges::any_of(hierarchy.parts, [](const auto& p) { return !p.placementOrder.empty(); });
    const bool instanced = std::ranges::any_of(hierarchy.parts, [](const auto& p) { return !p.instancedNodes.empty(); });
    write(file, instanced ? instancedMagic.data() : independent ? independentOrderMagic.data() : magic.data(), magic.size());
    for (const auto& source : sources)
    {
        const auto hash = sourceFingerprint(source);
        write(file, &hash);
    }
    writeArray(file, hierarchy.placementOrder);
    const auto count = static_cast<std::uint32_t>(hierarchy.parts.size());
    write(file, &count);
    for (const auto& part : hierarchy.parts)
    {
        write(file, &part.meshIndex);
        writeArray(file, part.nodes);
        writeArray(file, part.geometry.vertices);
        writeArray(file, part.geometry.indices);
    }
    if (independent || instanced)
        for (const auto& part : hierarchy.parts) writeArray(file, part.placementOrder);
    if (instanced)
        for (const auto& part : hierarchy.parts) writeArray(file, part.instancedNodes);
}
