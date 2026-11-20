#pragma once

#include "ViewerScene.h"

struct DiffuseProbeGrid
{
    DirectX::XMFLOAT4 originSpacing{};
    DirectX::XMUINT4 size{};
};

struct ProbeProxyVolume
{
    DiffuseProbeGrid probes;
    DirectX::XMFLOAT4 originCellSize;
    DirectX::XMUINT4 size;
    // XYZ are projected foliage area per cubic meter. W is opaque wood area density.
    std::vector<DirectX::XMFLOAT4> density;
};

ProbeProxyVolume buildProbeProxy(const ViewerScene& scene);
