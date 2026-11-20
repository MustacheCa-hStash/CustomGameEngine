#include "SprayBough.h"
#include "SourceFingerprint.h"

#include <cmath>
#include <limits>

namespace
{
constexpr std::array<char, 8> magic{'S', 'P', 'R', 'B', 'O', 'U', 'G', 'H'};
constexpr std::uint32_t version = 1;

template<class T>
void read(std::istream& file, T* data, std::size_t count = 1)
{
    if (!file.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(count * sizeof(T))))
        throw std::runtime_error("Truncated spray bough bake.");
}

template<class T>
void write(std::ostream& file, const T* data, std::size_t count = 1)
{
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(count * sizeof(T)));
}

template<class T>
std::vector<T> readArray(std::istream& file, std::size_t limit)
{
    std::uint32_t count;
    read(file, &count);
    if (!count || count > limit || count > std::numeric_limits<std::uint32_t>::max() / sizeof(T))
        throw std::runtime_error("Invalid spray bough buffer size.");
    std::vector<T> values(count);
    read(file, values.data(), count);
    return values;
}

template<class T>
void writeArray(std::ostream& file, const std::vector<T>& data)
{
    const auto count = static_cast<std::uint32_t>(data.size());
    write(file, &count);
    write(file, data.data(), count);
}
}

SprayBoughSources sprayBoughSources(const std::filesystem::path& glb)
{
    auto leaves = glb;
    auto wood = glb;
    return {glb, leaves.replace_extension(L".leaves.fixedlod"), wood.replace_extension(L".spray-wood.fixedlod")};
}

SprayBough readSprayBough(const SprayBoughSources& sources, const AssetData& asset,
                         const std::array<std::shared_ptr<const FixedLod>, 2>& lods, const std::filesystem::path& bake)
{
    auto path = sources.front();
    std::ifstream file(bake.empty() ? path.replace_extension(L".bough") : bake, std::ios::binary);
    if (!file) throw std::runtime_error("Bough proxy bake missing. Rebuild the viewer to generate it.");
    auto header = magic;
    std::uint32_t fileVersion;
    read(file, header.data(), header.size());
    read(file, &fileVersion);
    if (header != magic || fileVersion != version) throw std::runtime_error("Invalid spray bough header.");
    for (const auto& source : sources)
    {
        std::uint64_t hash;
        read(file, &hash);
        if (hash != sourceFingerprint(source))
            throw std::runtime_error("Bough proxy bake is incompatible with this tree or its spray bakes.");
    }
    const auto instanceCount = asset.meshes[lods[0]->meshIndex].instances->size();
    SprayBough bough;
    read(file, &bough.error);
    bough.placements = readArray<std::uint32_t>(file, instanceCount - 1);
    if (!std::isfinite(bough.error) || bough.error <= 0.0f ||
        !std::ranges::is_sorted(bough.placements) ||
        std::ranges::adjacent_find(bough.placements) != bough.placements.end() ||
        bough.placements.back() >= instanceCount)
        throw std::runtime_error("Invalid spray bough placement set.");

    for (std::size_t i = 0; i < bough.parts.size(); ++i)
    {
        auto& part = bough.parts[i];
        const auto& source = asset.meshes[lods[i]->meshIndex];
        const auto cap = lods[i]->levels[sprayBoughInputLevel(*lods[i])].indices.size() * bough.placements.size();
        part.vertices = readArray<Vertex>(file, cap);
        part.indices = readArray<std::uint32_t>(file, cap);
        if (part.indices.size() % 3 ||
            std::ranges::any_of(part.indices, [&](auto index) { return index >= part.vertices.size(); }))
            throw std::runtime_error("Invalid spray bough triangles.");
        auto finite3 = [](const DirectX::XMFLOAT3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        if (std::ranges::any_of(part.vertices, [&](const Vertex& vertex) {
            return !finite3(vertex.position) || !finite3(vertex.normal) ||
                !std::isfinite(vertex.texcoords[0].x) || !std::isfinite(vertex.texcoords[0].y) ||
                !std::isfinite(vertex.texcoords[1].x) || !std::isfinite(vertex.texcoords[1].y);
        })) throw std::runtime_error("Invalid spray bough vertex.");
        DirectX::XMStoreFloat4x4(&part.localToWorld, DirectX::XMMatrixIdentity());
        part.material = source.material;
    }
    if (file.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Unexpected spray bough data.");
    return bough;
}

void writeSprayBough(const std::filesystem::path& output, const SprayBoughSources& sources, const SprayBough& bough)
{
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    write(file, magic.data(), magic.size());
    write(file, &version);
    for (const auto& source : sources)
    {
        const auto hash = sourceFingerprint(source);
        write(file, &hash);
    }
    write(file, &bough.error);
    writeArray(file, bough.placements);
    for (const auto& part : bough.parts)
    {
        writeArray(file, part.vertices);
        writeArray(file, part.indices);
    }
    file.close();
}
