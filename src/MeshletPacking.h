#pragma once

#include <meshoptimizer.h>

#include <cstdint>
#include <span>
#include <vector>

// The scan builder preserves triangle order. Its sizes can partition an existing index buffer.
inline std::vector<std::uint32_t> scanMeshletIndexCounts(std::span<const std::uint32_t> indices,
                                                        std::size_t vertexCount, unsigned triangleCap)
{
    constexpr unsigned vertexCap = 256;
    std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(indices.size(), vertexCap, triangleCap));
    std::vector<unsigned int> vertices(indices.size());
    std::vector<unsigned char> triangles(indices.size());
    const auto count = meshopt_buildMeshletsScan(meshlets.data(), vertices.data(), triangles.data(),
        indices.data(), indices.size(), vertexCount, vertexCap, triangleCap);
    std::vector<std::uint32_t> sizes;
    sizes.reserve(count);
    for (const auto& meshlet : std::span(meshlets).first(count))
        sizes.push_back(meshlet.triangle_count * 3);
    return sizes;
}
