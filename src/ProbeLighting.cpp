#include "ProbeLighting.h"

#include <DirectXCollision.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace
{
struct Sample
{
    XMFLOAT3 point;
    XMFLOAT3 unitNormal;
};

struct SurfaceSamples
{
    std::vector<Sample> points;
    float areaPerSample = 0;
};

SurfaceSamples sampleSurface(const MeshData& mesh, unsigned budget)
{
    SurfaceSamples samples;
    std::vector<double> areas;
    areas.reserve(mesh.indices.size() / 3);
    double total = 0;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
    {
        const auto a = XMLoadFloat3(&mesh.vertices[mesh.indices[i]].position);
        const auto b = XMLoadFloat3(&mesh.vertices[mesh.indices[i + 1]].position);
        const auto c = XMLoadFloat3(&mesh.vertices[mesh.indices[i + 2]].position);
        total += 0.5 * XMVectorGetX(XMVector3Length(XMVector3Cross(b - a, c - a)));
        areas.push_back(total);
    }
    if (total <= 0) return samples;
    samples.points.reserve(budget);
    samples.areaPerSample = static_cast<float>(total / budget);
    std::size_t triangle = 0;
    for (unsigned i = 0; i < budget; ++i)
    {
        const double target = (i + 0.5) * total / budget;
        while (areas[triangle] <= target) ++triangle;
        const auto first = triangle * 3;
        const auto a = XMLoadFloat3(&mesh.vertices[mesh.indices[first]].position);
        const auto ab = XMLoadFloat3(&mesh.vertices[mesh.indices[first + 1]].position) - a;
        const auto ac = XMLoadFloat3(&mesh.vertices[mesh.indices[first + 2]].position) - a;
        float u = static_cast<float>(std::fmod((i + 0.5) * 0.7548776662467, 1.0));
        float v = static_cast<float>(std::fmod((i + 0.5) * 0.5698402909981, 1.0));
        if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
        auto& sample = samples.points.emplace_back();
        XMStoreFloat3(&sample.point, a + u * ab + v * ac);
        XMStoreFloat3(&sample.unitNormal, XMVector3Normalize(XMVector3Cross(ab, ac)));
    }
    return samples;
}

void deposit(ProbeProxyVolume& volume, const SurfaceSamples& samples, FXMMATRIX transform,
             bool foliage, float coverage)
{
    const auto origin = XMLoadFloat4(&volume.originCellSize);
    const float step = volume.originCellSize.w;
    XMVECTOR determinant;
    const auto areaTransform = XMMatrixTranspose(XMMatrixInverse(&determinant, transform));
    const float densityScale = coverage * samples.areaPerSample * std::abs(XMVectorGetX(determinant)) /
                               (step * step * step);
    for (const auto& sample : samples.points)
    {
        XMFLOAT3 coord;
        XMStoreFloat3(&coord, (XMVector3TransformCoord(XMLoadFloat3(&sample.point), transform) - origin) / step);
        if (coord.x < 0 || coord.y < 0 || coord.z < 0 ||
            coord.x >= volume.size.x || coord.y >= volume.size.y || coord.z >= volume.size.z) continue;
        const auto index = (static_cast<unsigned>(coord.z) * volume.size.y + static_cast<unsigned>(coord.y)) *
                           volume.size.x + static_cast<unsigned>(coord.x);
        const auto normal = XMVectorAbs(XMVector3TransformNormal(XMLoadFloat3(&sample.unitNormal), areaTransform)) *
                            densityScale;
        auto& cell = volume.density[index];
        if (foliage) XMStoreFloat4(&cell, XMVectorSetW(XMLoadFloat4(&cell) + normal, cell.w));
        else cell.w += XMVectorGetX(XMVector3Length(normal));
    }
}
}

ProbeProxyVolume buildProbeProxy(const ViewerScene& scene)
{
    const BoundingBox local(scene.assetBounds.center, scene.assetBounds.halfExtent);
    auto lower = XMVectorReplicate(FLT_MAX);
    auto upper = XMVectorReplicate(-FLT_MAX);
    const auto placements = scene.instanceCount();
    for (unsigned i = 0; i < placements; ++i)
    {
        BoundingBox bounds;
        local.Transform(bounds, scene.objects[i].placementMatrix());
        const auto center = XMLoadFloat3(&bounds.Center);
        const auto extent = XMLoadFloat3(&bounds.Extents);
        lower = XMVectorMin(lower, center - extent);
        upper = XMVectorMax(upper, center + extent);
    }
    const float ground = scene.ground.localBounds.center.y;
    lower = XMVectorSetY(lower, std::min(XMVectorGetY(lower), ground));
    XMFLOAT3 extent;
    XMStoreFloat3(&extent, upper - lower);
    const float cell = std::max(0.025f, std::max({extent.x, extent.y, extent.z}) / 88.0f);
    const float spacing = 4.0f * cell;
    lower -= XMVectorReplicate(2.0f * spacing);
    lower = XMVectorSetY(lower, ground);
    upper += XMVectorReplicate(2.0f * spacing);
    XMStoreFloat3(&extent, upper - lower);
    const auto cells = [&](float length) { return std::max(3u, static_cast<unsigned>(std::ceil(length / spacing)) + 1); };

    ProbeProxyVolume volume{};
    XMStoreFloat4(&volume.originCellSize, XMVectorSetW(lower, cell));
    XMStoreFloat4(&volume.probes.originSpacing, XMVectorSetW(XMVectorSetY(lower, ground + spacing * 0.35f), spacing));
    volume.probes.size = {cells(extent.x), cells(extent.y), cells(extent.z), 0};
    volume.size = {(volume.probes.size.x - 1) * 4, (volume.probes.size.y - 1) * 4,
                   (volume.probes.size.z - 1) * 4, 0};
    volume.density.resize(static_cast<std::size_t>(volume.size.x) * volume.size.y * volume.size.z);

    for (const auto& mesh : scene.asset->meshes)
    {
        const auto& material = scene.asset->materials[mesh.material];
        const bool foliage = material.hasBacklighting;
        float coverage = 1.0f;
        if (foliage && material.alphaCutoff >= 0 && material.colorTexture)
            coverage = scene.asset->textures[*material.colorTexture].mips.back().rgba[3] / 255.0f;
        const auto samples = sampleSurface(mesh, mesh.instances ? (foliage ? 1152u : 512u) : 24576u);
        const auto localToAsset = XMLoadFloat4x4(&mesh.localToWorld);
        for (unsigned i = 0; i < placements; ++i)
        {
            const auto world = localToAsset * scene.objects[i].placementMatrix();
            if (mesh.instances)
                for (const auto& instance : *mesh.instances)
                    deposit(volume, samples, instance.matrix() * world, foliage, coverage);
            else
                deposit(volume, samples, world, foliage, coverage);
        }
    }
    // Sparse wood covers only part of a cell face. Retain strong extinction once
    // enough wood surface spans a cell, as on the trunk and larger boughs.
    for (auto& density : volume.density)
    {
        const float woodAreaPerFace = density.w * cell;
        const float t = std::clamp((woodAreaPerFace - 0.55f) / (1.25f - 0.55f), 0.0f, 1.0f);
        const float coverage = t * t * (3.0f - 2.0f * t);
        density.w *= std::lerp(0.25f, 3.0f, coverage);
    }
    return volume;
}
