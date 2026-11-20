// Each group handles one material part of one tree in the current camera or light view.
#include "TreeCull.hlsli"

cbuffer Selection : register(b0)
{
    row_major float4x4 treeToWorld;
    row_major float4x4 worldToClip;
    row_major float4x4 treeToView;
    row_major float4x4 treeToClip;
    float4 treeWind;
    float4 viewWind;
    float4 sprayCenter;
    float4 sprayExtent;
    float4 projection; // budget, focal pixels, camera near plane, shadow texels per NDC unit
    uint4 sizes; // nodes, placements, buckets, local plan count
    uint4 options; // flags, whole plan count, depth width, depth height
};

StructuredBuffer<Node> nodes : register(t0);
StructuredBuffer<Placement> placements : register(t1);
StructuredBuffer<Plan> plans : register(t2);
ByteAddressBuffer templates : register(t3);
StructuredBuffer<uint> planBuckets : register(t4);
ByteAddressBuffer depthTiles : register(t5);
RWByteAddressBuffer commands : register(u0);
RWByteAddressBuffer results : register(u1);
RWByteAddressBuffer compacted : register(u2);
RWByteAddressBuffer tags : register(u3);

static const uint commandBytes = 88;
static const uint threads = 256;
groupshared uint frontier[2048];
groupshared uint fallback[2048];
groupshared uint nextEnd;
groupshared uint bucketSize[1024];
groupshared uint counts[10];

bool hiddenByDepth(float3 center, float3 extent)
{
    float2 lower = 1, upper = -1;
    float nearest = 1;
    for (uint i = 0; i < 8; ++i)
    {
        float3 sign = float3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1);
        float4 p = mul(float4(center + extent * sign, 1), worldToClip);
        if (p.w <= 0 || p.z < 0) return false;
        float3 ndc = p.xyz / p.w;
        lower = min(lower, ndc.xy);
        upper = max(upper, ndc.xy);
        nearest = min(nearest, ndc.z);
    }
    uint2 resolution = options.zw;
    float2 a = float2(lower.x * 0.5 + 0.5, 0.5 - upper.y * 0.5) * resolution - 1;
    float2 b = float2(upper.x * 0.5 + 0.5, 0.5 - lower.y * 0.5) * resolution + 1;
    uint2 lo = (uint2)clamp(a, 0, resolution - 1), hi = (uint2)clamp(b, 0, resolution - 1);
    uint2 fine = (resolution + 7) >> 3;
    // Coarser tiles reject broad bounds. Use the fine level for a small projected rectangle.
    bool coarse = any((hi >> 3) - (lo >> 3) >= 6);
    uint shift = coarse ? 6 : 3;
    uint2 first = lo >> shift, last = hi >> shift;
    if (any(last - first > 8)) return false;
    uint stride = coarse ? (fine.x + 7) >> 3 : fine.x;
    uint offset = coarse ? fine.x * fine.y : 0;
    for (uint y = first.y; y <= last.y; ++y)
        for (uint x = first.x; x <= last.x; ++x)
            if (asfloat(depthTiles.Load(4 * (offset + y * stride + x))) + 0.000004 >= nearest) return false;
    return true;
}

void outputCommand(uint destination, uint source, uint instances)
{
    uint begin = source * commandBytes;
    uint end = destination * commandBytes;
    for (uint i = 0; i < commandBytes; i += 4)
        commands.Store(end + i, i == 68 ? instances : templates.Load(begin + i));
    InterlockedAdd(counts[2], templates.Load(begin + 64) / 3 * instances);
    InterlockedXor(counts[9], (source + 1) * 2654435761u ^ instances * 2246822519u);
}

[numthreads(threads, 1, 1)]
void Select(uint lane : SV_GroupIndex)
{
    if (lane < 10) counts[lane] = 0;
    for (uint b = lane; b < sizes.z; b += threads) bucketSize[b] = 0;
    for (uint i = lane; i < sizes.x; i += threads) fallback[i] = 0;
    if (lane == 0)
    {
        frontier[0] = 0;
        nextEnd = 1;
    }
    GroupMemoryBarrierWithGroupSync();
    uint first = 0, last = 1;
    // Only expand a node if its parent passed visibility and needed a finer cut.
    while (first < last)
    {
        for (uint cursor = first + lane; cursor < last; cursor += threads)
        {
            uint id = frontier[cursor];
            Node n = nodes[id];
            float3 center = mul(float4(n.center, 1), treeToWorld).xyz + treeWind.xyz;
            float3 extent = halfExtent(n.extent, treeToWorld) + abs(treeWind.xyz);
            if ((options.x & 1) && !intersectsFrustum(center, extent, worldToClip, 0)) continue;
            if ((options.x & 4) && hiddenByDepth(center, extent))
            {
                InterlockedAdd(counts[7], 1);
                continue;
            }
            float scale = options.x & 2 ? shadowTexelsPerUnit(treeToClip, projection.w) : pixelsPerUnit(n.center, n.extent, treeToView, viewWind.xyz, projection.yz);
            if (n.indexCount && (n.error == 0 || n.error * scale <= projection.x * 0.9999))
            {
                uint destination;
                InterlockedAdd(counts[0], 1, destination);
                outputCommand(destination, id, 1);
                InterlockedAdd(counts[3], n.placements);
                InterlockedAdd(counts[8], n.indexCount / 3);
            }
            else if (n.childCount)
            {
                uint destination;
                InterlockedAdd(nextEnd, n.childCount, destination);
                for (uint c = 0; c < n.childCount; ++c)
                    frontier[destination + c] = n.firstChild + c;
            }
            else
            {
                fallback[id] = 1;
                InterlockedAdd(counts[4], 1);
                if (!sizes.y) bucketSize[0] = 1;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        first = last;
        last = nextEnd;
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) counts[6] = last;
    if (counts[4])
    {
        float4 wind = mul(float4(2 * treeWind.xyz, 0), worldToClip);
        uint visible = 0;
        uint occluded = 0;
        uint hash = 0;
        for (uint id = lane; id < sizes.y; id += threads)
        {
            if (!fallback[placements[id].leaf]) continue;
            Placement p = placements[id];
            row_major float4x4 toClip = mul(p.toAsset, treeToClip);
            if ((options.x & 1) && !intersectsFrustum(sprayCenter.xyz, sprayExtent.xyz, toClip, wind))
                continue;
            if (options.x & 4)
            {
                row_major float4x4 transform = mul(p.toAsset, treeToWorld);
                float3 center = mul(float4(sprayCenter.xyz, 1), transform).xyz + treeWind.xyz;
                float3 extent = halfExtent(sprayExtent.xyz, transform) + abs(treeWind.xyz);
                if (hiddenByDepth(center, extent))
                {
                    ++occluded;
                    continue;
                }
            }
            ++visible;
            float pixelScale = options.x & 2 ? shadowTexelsPerUnit(toClip, projection.w)
                : pixelsPerUnit(sprayCenter.xyz, sprayExtent.xyz, mul(p.toAsset, treeToView), viewWind.xyz, projection.yz);
            uint firstPlan = options.x & 8 ? 0 : sizes.w;
            uint planCount = options.x & 8 ? sizes.w : options.y;
            Plan plan = plans[selectPlan(plans, firstPlan, planCount, projection.x, pixelScale)];
            for (uint slot = 0; slot < plan.count; ++slot)
            {
                uint bucket = planBuckets[plan.first + slot];
                uint local;
                InterlockedAdd(bucketSize[bucket], 1, local);
                uint destination = sizes.y * bucket + local;
                uint offset = destination * 40;
                compacted.Store3(offset, asuint(p.translation));
                compacted.Store4(offset + 12, asuint(p.rotation));
                compacted.Store3(offset + 28, asuint(p.scale));
                if (options.x & 16) tags.Store3(destination * 12, p.tag);
                hash ^= (id + 1) * 3266489917u ^ (bucket + 1) * 668265263u;
            }
        }
        if (visible) InterlockedAdd(counts[5], visible);
        if (occluded) InterlockedAdd(counts[7], occluded);
        if (hash) InterlockedXor(counts[9], hash);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint b = lane; b < sizes.z; b += threads)
    {
        uint size = bucketSize[b];
        if (!size) continue;
        uint destination;
        InterlockedAdd(counts[1], 1, destination);
        outputCommand(sizes.x + destination, sizes.x + b, size);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 10) results.Store(lane * 4, counts[lane]);
}
