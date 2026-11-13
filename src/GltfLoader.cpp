#include "GltfLoader.h"
#include "TextureLoader.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
void checkGltf(cgltf_result result, const char* message)
{
    if (result != cgltf_result_success)
    {
        throw std::runtime_error(message);
    }
}

bool readComponents(const cgltf_accessor& accessor, cgltf_size index, std::span<float> components)
{
    return cgltf_accessor_read_float(&accessor, index, components.data(), components.size()) &&
           std::all_of(components.begin(), components.end(), [](float component) {
               return std::isfinite(component);
           });
}

MeshData readPrimitive(const cgltf_primitive& primitive, const DirectX::XMFLOAT4X4& localToWorld)
{
    if (primitive.type != cgltf_primitive_type_triangles || primitive.has_draco_mesh_compression ||
        primitive.targets_count != 0)
    {
        throw std::runtime_error("Expected a static, uncompressed triangle primitive.");
    }

    const auto* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
    const auto* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* texcoords[] = {
        cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0),
        cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 1),
    };
    if (!positions || !normals || positions->type != cgltf_type_vec3 || normals->type != cgltf_type_vec3)
    {
        throw std::runtime_error("Every triangle primitive needs POSITION and NORMAL attributes.");
    }

    const auto* indices = primitive.indices;
    const auto indexCount = indices ? indices->count : positions->count;
    constexpr auto maxBufferSize = (std::numeric_limits<std::uint32_t>::max)();
    if (positions->count > maxBufferSize / sizeof(Vertex) ||
        indexCount > maxBufferSize / sizeof(std::uint32_t) || indexCount == 0 || indexCount % 3 != 0)
    {
        throw std::runtime_error("Primitive has an invalid vertex or triangle index count.");
    }

    MeshData mesh{};
    mesh.localToWorld = localToWorld;
    mesh.uvMask = (texcoords[0] ? 1u : 0u) | (texcoords[1] ? 2u : 0u);
    mesh.vertices.resize(positions->count);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        auto& vertex = mesh.vertices[i];
        float position[3]{};
        float normal[3]{};
        if (!readComponents(*positions, i, position) || !readComponents(*normals, i, normal))
        {
            throw std::runtime_error("Could not read a position or normal. Use non-sparse vertex attributes.");
        }
        vertex.position = DirectX::XMFLOAT3(position);
        vertex.normal = DirectX::XMFLOAT3(normal);
        for (std::size_t set = 0; set < std::size(texcoords); ++set)
        {
            const auto* accessor = texcoords[set];
            float uv[2]{};
            if (accessor && (accessor->type != cgltf_type_vec2 || !readComponents(*accessor, i, uv)))
            {
                throw std::runtime_error("Could not read texture coordinates. Use non-sparse VEC2 attributes.");
            }
            vertex.texcoords[set] = DirectX::XMFLOAT2(uv);
        }
    }

    mesh.indices.resize(indexCount);
    if (!indices)
    {
        std::iota(mesh.indices.begin(), mesh.indices.end(), std::uint32_t{0});
    }
    else if (cgltf_accessor_unpack_indices(indices, mesh.indices.data(),
                                          sizeof(mesh.indices[0]), indexCount) != indexCount)
    {
        throw std::runtime_error("Could not read triangle indices. Use a non-sparse index accessor.");
    }
    if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [&](std::uint32_t index) {
        return index >= mesh.vertices.size();
    }))
    {
        throw std::runtime_error("Triangle index is outside the vertex buffer.");
    }
    return mesh;
}

MaterialData readMaterial(const cgltf_material& source, AssetData& asset)
{
    MaterialData material;
    material.name = source.name ? source.name : "Material";
    if (source.alpha_mode == cgltf_alpha_mode_mask)
    {
        material.alphaCutoff = source.alpha_cutoff;
        // For this viewer, a two-sided cutout is treated as a thin surface.
        material.transmission = source.double_sided ? 0.25f : 0.0f;
    }
    if (!source.has_pbr_metallic_roughness)
    {
        return material;
    }
    const auto& pbr = source.pbr_metallic_roughness;
    material.color = DirectX::XMFLOAT4(pbr.base_color_factor);
    const auto& view = pbr.base_color_texture;
    if (!view.texture)
    {
        return material;
    }
    if (view.has_transform || view.texcoord < 0 || view.texcoord > 1)
    {
        throw std::runtime_error("A base-color texture needs untransformed TEXCOORD_0 or TEXCOORD_1.");
    }
    material.textureCoordinateSet = static_cast<std::uint32_t>(view.texcoord);

    const auto* image = view.texture->image;
    if (!image || !image->buffer_view || !image->mime_type ||
        (std::string_view(image->mime_type) != "image/png" && std::string_view(image->mime_type) != "image/jpeg"))
    {
        throw std::runtime_error("Base-color textures must be embedded PNG or JPEG images.");
    }
    const auto* encoded = cgltf_buffer_view_data(image->buffer_view);
    if (!encoded)
    {
        throw std::runtime_error("Could not read the embedded base-color texture.");
    }
    const float coverageCutoff = material.color.w > 0.0f ? material.alphaCutoff / material.color.w : -1.0f;
    material.colorTexture = asset.textures.size();
    asset.textures.push_back({image->name ? image->name : "Base color",
                              loadColorTexture({encoded, image->buffer_view->size}, coverageCutoff)});
    return material;
}
}

AssetData loadGlb(const std::filesystem::path& path)
{
    // Open through std::filesystem so Windows Unicode paths reach the file intact.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Could not open the GLB file.");
    }
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size < 12 || size > (std::numeric_limits<std::uint32_t>::max)())
    {
        throw std::runtime_error("Invalid GLB file size.");
    }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(bytes.data(), size))
    {
        throw std::runtime_error("Could not read the GLB file.");
    }

    cgltf_options options{};
    options.type = cgltf_file_type_glb;
    cgltf_data* parsed = nullptr;
    checkGltf(cgltf_parse(&options, bytes.data(), bytes.size(), &parsed),
              "Could not parse the file as a glTF 2.0 GLB.");
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
    if (data->buffers_count != 1 || data->buffers[0].uri || !data->bin)
    {
        throw std::runtime_error("Export a self-contained GLB with an embedded mesh buffer.");
    }
    if (data->extensions_required_count != 0)
    {
        throw std::runtime_error("The GLB requires an unsupported glTF extension.");
    }
    checkGltf(cgltf_load_buffers(&options, data.get(), nullptr), "Could not load the embedded mesh buffer.");
    checkGltf(cgltf_validate(data.get()), "The GLB failed glTF validation.");
    if (data->scenes_count == 0)
    {
        throw std::runtime_error("The GLB contains no scenes.");
    }

    const auto& scene = data->scene ? *data->scene : data->scenes[0];
    std::vector<const cgltf_node*> pending;
    for (cgltf_size i = 0; i < scene.nodes_count; ++i)
    {
        pending.push_back(scene.nodes[i]);
    }

    AssetData result;
    result.materials.emplace_back();
    std::vector<std::optional<std::size_t>> materialIds(data->materials_count);
    while (!pending.empty())
    {
        const auto& node = *pending.back();
        pending.pop_back();
        for (cgltf_size i = 0; i < node.children_count; ++i)
        {
            pending.push_back(node.children[i]);
        }
        if (!node.mesh)
        {
            continue;
        }
        if (node.skin || node.has_mesh_gpu_instancing)
        {
            throw std::runtime_error("Only static mesh nodes are supported.");
        }

        float matrix[16]{};
        cgltf_node_transform_world(&node, matrix);
        // glTF column-vector matrices have the same memory order as their row-vector transpose.
        const DirectX::XMFLOAT4X4 localToWorld(matrix);
        const float determinant = DirectX::XMVectorGetX(
            DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&localToWorld)));
        if (!std::isfinite(determinant) || determinant == 0.0f)
        {
            throw std::runtime_error("A mesh node has a non-invertible world transform.");
        }
        for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i)
        {
            const auto& primitive = node.mesh->primitives[i];
            auto mesh = readPrimitive(primitive, localToWorld);
            mesh.name = node.name ? node.name : "Mesh";
            if (node.mesh->primitives_count > 1)
            {
                mesh.name += " / Primitive " + std::to_string(i + 1);
            }
            if (primitive.material)
            {
                auto& id = materialIds[cgltf_material_index(data.get(), primitive.material)];
                if (!id)
                {
                    id = result.materials.size();
                    result.materials.push_back(readMaterial(*primitive.material, result));
                }
                mesh.material = *id;
                const auto& material = result.materials[*id];
                if (material.colorTexture && !(mesh.uvMask & (1u << material.textureCoordinateSet)))
                {
                    throw std::runtime_error("A base-color texture references a missing UV set.");
                }
            }
            result.meshes.push_back(std::move(mesh));
        }
    }
    if (result.meshes.empty())
    {
        throw std::runtime_error("The GLB scene contains no drawable meshes.");
    }
    return result;
}
