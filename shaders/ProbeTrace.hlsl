struct ProbeLobe
{
    float3 correction;
    uint distanceMoments;
};

cbuffer ProbeUpdate : register(b0)
{
    float4 proxyOriginCellSize;
    uint4 proxySize;
    float4 probeOriginSpacing;
    uint4 probeSize;
    float skyRadianceScale;
    uint firstProbe;
    uint updateCount;
    uint includeBounce;
    float3 toSun;
    float sunStrength;
    float3 groundColor;
    float groundHalfWidth;
    float3 openSkyGround;
    float groundHalfDepth;
    float2 proxyWind;
    float windStart;
    float inverseWindHeight;
    uint groundRowStride;
    uint groundFirstRow;
};

StructuredBuffer<float4> proxyDensity : register(t0);
Texture2D<float4> skyPanorama : register(t1);
StructuredBuffer<uint> updateOrder : register(t2);
SamplerState panoramaSampler : register(s0);
RWStructuredBuffer<ProbeLobe> probes : register(u0);
RWStructuredBuffer<float4> groundLight : register(u1);
RWStructuredBuffer<float4> advectedDensity : register(u2);

static const uint rayCount = 96;
static const float3 axes[6] = {
    float3(1,0,0), float3(-1,0,0), float3(0,1,0),
    float3(0,-1,0), float3(0,0,1), float3(0,0,-1)
};

float3 skyRadiance(float3 ray)
{
    if (ray.y <= 0) return 0;
    const float2 uv = float2(0.5 + atan2(ray.x, ray.z + 1e-8) * 0.1591549431,
                             0.5 - asin(clamp(ray.y, -1.0, 1.0)) * 0.3183098862);
    return skyPanorama.SampleLevel(panoramaSampler, uv, 4).rgb * skyRadianceScale;
}

uint densityIndex(uint3 cell)
{
    return (cell.z * proxySize.y + cell.y) * proxySize.x + cell.x;
}

float4 densityAt(int3 cell)
{
    if (any(cell < 0) || any(cell >= int3(proxySize.xyz))) return 0;
    return proxyDensity[densityIndex(uint3(cell))];
}

[numthreads(8, 4, 4)]
void AdvectCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch >= proxySize.xyz)) return;
    const int3 cell = int3(dispatch);
    const uint index = densityIndex(dispatch);
    const float height = proxyOriginCellSize.y + (cell.y + 0.5) * proxyOriginCellSize.w;
    const float weight = saturate((height - windStart) * inverseWindHeight);
    const float2 rest = cell.xz - (proxyWind * weight * weight) / proxyOriginCellSize.w;
    const int2 a = int2(floor(rest));
    const float2 f = frac(rest);
    const float3 low = lerp(densityAt(int3(a.x, cell.y, a.y)).xyz,
                            densityAt(int3(a.x + 1, cell.y, a.y)).xyz, f.x);
    const float3 high = lerp(densityAt(int3(a.x, cell.y, a.y + 1)).xyz,
                             densityAt(int3(a.x + 1, cell.y, a.y + 1)).xyz, f.x);
    advectedDensity[index] = float4(lerp(low, high, f.y), proxyDensity[index].w);
}

float traceProxy(float3 origin, float3 direction, float limit, out float blockerDistance)
{
    const float step = proxyOriginCellSize.w;
    const float3 start = (origin - proxyOriginCellSize.xyz) / step;
    const float3 inverse = 1.0 / select(abs(direction) < 1e-6, 1e-6.xxx, direction);
    const float3 a = -start * inverse;
    const float3 b = (float3(proxySize.xyz) - start) * inverse;
    const float3 nearT = min(a, b);
    const float3 farT = max(a, b);
    float t = max(0, max(nearT.x, max(nearT.y, nearT.z)));
    const float exit = min(limit / step, min(farT.x, min(farT.y, farT.z)));
    blockerDistance = limit;
    if (exit <= t) return 1;

    int3 cell = clamp(int3(floor(start + direction * (t + 1e-4))), 0, int3(proxySize.xyz) - 1);
    const int3 advance = 2 * int3(direction >= 0) - 1;
    const float3 period = abs(inverse);
    float3 crossing = ((float3(cell) + float3(advance > 0)) - start) * inverse;
    float visibility = 1;
    [loop]
    for (uint sample = 0; sample < 320 && t < exit; ++sample)
    {
        const float boundary = min(crossing.x, min(crossing.y, crossing.z));
        const float segmentLength = max(0, min(boundary, exit) - t) * step;
        const float4 density = proxyDensity[densityIndex(uint3(cell))];
        // The projected components can cancel for oblique rays. Their length avoids summing
        // all three unsigned projections as independent blockers.
        const float foliage = length(density.xyz * abs(direction)) * 0.92;
        // Wood extinction is baked into the static channel of the coverage proxy.
        visibility *= exp(-(foliage + density.w) * segmentLength);
        if (visibility < 0.60 && blockerDistance == limit)
            blockerDistance = min(boundary * step, limit);
        if (visibility < 0.002) return 0;
        const bool3 crossed = crossing <= boundary + 1e-6;
        crossing += float3(crossed) * period;
        cell += int3(crossed) * advance;
        if (any(cell < 0) || any(cell >= int3(proxySize.xyz))) break;
        t = boundary;
    }
    return visibility;
}

uint2 groundSize()
{
    return (proxySize.xz + 1) / 2;
}

[numthreads(8, 8, 1)]
void GroundCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint2 size = groundSize();
    const uint2 cell = uint2(dispatch.x, dispatch.y * groundRowStride + groundFirstRow);
    if (any(cell >= size)) return;
    const float step = proxyOriginCellSize.w;
    const float2 coord = proxyOriginCellSize.xz + (cell + 0.5) * (2 * step);
    const float3 origin = float3(coord.x, proxyOriginCellSize.y + 0.08 * step, coord.y);
    const float limit = length(float3(proxySize.xyz)) * step;
    const uint skyRays = 24;
    float3 incoming = openSkyGround;
    [loop]
    for (uint i = 0; i < skyRays; ++i)
    {
        // Cosine-weighted integration yields the irradiance divided by pi directly.
        const float r2 = (i + 0.5) / skyRays;
        float sine, cosine;
        sincos(i * 2.39996323 + 0.5, sine, cosine);
        const float3 direction = float3(sqrt(r2) * cosine, sqrt(1 - r2), sqrt(r2) * sine);
        float blocker;
        incoming += skyRadiance(direction) * (traceProxy(origin, direction, limit, blocker) - 1) / skyRays;
    }
    if (sunStrength > 0 && toSun.y > 0)
    {
        float blocker;
        const float visibility = traceProxy(origin, toSun, limit, blocker);
        incoming += (sunStrength * toSun.y * visibility).xxx;
    }
    groundLight[cell.y * size.x + cell.x] = float4(max(0, incoming) * groundColor, 0);
}

float3 groundRadiance(float2 position)
{
    if (any(abs(position) > float2(groundHalfWidth, groundHalfDepth))) return 0;
    const uint2 size = groundSize();
    const float2 grid = (position - proxyOriginCellSize.xz) / (2 * proxyOriginCellSize.w) - 0.5;
    if (any(grid < -0.5) || any(grid > float2(size) - 0.5))
        return groundColor * (openSkyGround + sunStrength * max(0, toSun.y));
    const float2 clamped = clamp(grid, 0, float2(size - 1));
    const uint2 a = uint2(clamped);
    const uint2 b = min(a + 1, size - 1);
    const float2 f = clamped - a;
    return lerp(lerp(groundLight[a.y * size.x + a.x].rgb, groundLight[a.y * size.x + b.x].rgb, f.x),
                lerp(groundLight[b.y * size.x + a.x].rgb, groundLight[b.y * size.x + b.x].rgb, f.x), f.y);
}

// One thread per probe. Reuse each software trace for all six irradiance lobes.
[numthreads(32, 1, 1)]
void CSMain(uint3 dispatch : SV_DispatchThreadID)
{
    if (dispatch.x >= updateCount) return;
    const uint index = updateOrder[firstProbe + dispatch.x];
    const uint3 coord = uint3(index % probeSize.x, (index / probeSize.x) % probeSize.y,
                              index / (probeSize.x * probeSize.y));
    const float3 origin = probeOriginSpacing.xyz + coord * probeOriginSpacing.w;
    const float traceRange = length(float3(proxySize.xyz)) * proxyOriginCellSize.w;
    const float momentRange = 4 * probeOriginSpacing.w;
    float3 radiance[6] = {0.xxx, 0.xxx, 0.xxx, 0.xxx, 0.xxx, 0.xxx};
    float normalization[6] = {0, 0, 0, 0, 0, 0};
    float3 moments[6] = {0.xxx, 0.xxx, 0.xxx, 0.xxx, 0.xxx, 0.xxx};
    [loop]
    for (uint i = 0; i < rayCount; ++i)
    {
        const float y = 1.0 - 2.0 * (i + 0.5) / rayCount;
        const float radius = sqrt(1.0 - y * y);
        float sinPhi, cosPhi;
        sincos(i * 2.39996323 + 0.5, sinPhi, cosPhi);
        const float3 direction = float3(radius * cosPhi, y, radius * sinPhi);
        const float groundDistance = y < 0 ? (proxyOriginCellSize.y - origin.y) / y : traceRange;
        float blocker;
        const float visibility = traceProxy(origin, direction, min(groundDistance, traceRange), blocker);
        float3 correction = skyRadiance(direction) * (visibility - 1);
        if (includeBounce && y < 0)
            correction += visibility * groundRadiance(origin.xz + direction.xz * groundDistance);
        const float distance = saturate(blocker / momentRange);
        [unroll]
        for (uint axis = 0; axis < 6; ++axis)
        {
            const float cosine = max(0, dot(direction, axes[axis]));
            radiance[axis] += correction * cosine;
            normalization[axis] += cosine;
            const float cone = pow(cosine, 8);
            moments[axis] += cone * float3(distance, distance * distance, 1);
        }
    }
    [unroll]
    for (uint axis = 0; axis < 6; ++axis)
    {
        const float2 stats = moments[axis].xy / max(moments[axis].z, 1e-5);
        ProbeLobe lobe;
        lobe.correction = radiance[axis] / normalization[axis];
        lobe.distanceMoments = f32tof16(stats.x) | (f32tof16(stats.y) << 16);
        probes[index * 6 + axis] = lobe;
    }
}
