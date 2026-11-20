#include "TreePyramid.hlsli"

Texture2D<float> cameraDepth : register(t0);
Texture2DMS<float> multisampleDepth : register(t1);
RWByteAddressBuffer tiles : register(u0);

cbuffer Build : register(b0)
{
    uint2 size;
    uint samples;
    uint coarseLevel;
};

groupshared uint farthest;

[numthreads(8, 8, 1)]
void BuildTiles(uint3 tile : SV_GroupID, uint3 pixel : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    if (lane == 0) farthest = 0;
    GroupMemoryBarrierWithGroupSync();
    uint2 fine = (size + 7) >> 3;
    float depth = 1;
    if (coarseLevel)
    {
        if (all(pixel.xy < fine)) depth = asfloat(tiles.Load(4 * (pixel.y * fine.x + pixel.x)));
    }
    else if (all(pixel.xy < size))
    {
        if (samples == 1) depth = cameraDepth.Load(int3(pixel.xy, 0));
        else
        {
            depth = 0;
            for (uint s = 0; s < samples; ++s) depth = max(depth, multisampleDepth.Load(pixel.xy, s));
        }
    }
    InterlockedMax(farthest, asuint(depth));
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        uint stride = coarseLevel ? (fine.x + 7) >> 3 : fine.x;
        uint offset = coarseLevel ? fine.x * fine.y : 0;
        tiles.Store(4 * (offset + tile.y * stride + tile.x), farthest);
    }
}

[numthreads(8, 8, 1)]
void BuildOcclusionTiles(uint2 tile : SV_DispatchThreadID)
{
    const uint2 outputSize = pyramidSize(size, coarseLevel);
    if (any(tile >= outputSize)) return;
    const uint2 childSize = coarseLevel ? pyramidSize(size, coarseLevel - 1) : size;
    const uint childOffset = coarseLevel ? pyramidOffset(size, coarseLevel - 1) : 0;
    float depth = 0;
    [unroll]
    for (uint y = 0; y < 4; ++y)
    {
        [unroll]
        for (uint x = 0; x < 4; ++x)
        {
            const uint2 child = 4 * tile + uint2(x, y);
            if (any(child >= childSize)) depth = 1;
            else if (coarseLevel) depth = max(depth, asfloat(tiles.Load(4 * (childOffset + child.y * childSize.x + child.x))));
            else if (samples == 1) depth = max(depth, cameraDepth.Load(int3(child, 0)));
            else
                for (uint sample = 0; sample < samples; ++sample)
                    depth = max(depth, multisampleDepth.Load(child, sample));
        }
    }
    tiles.Store(4 * (pyramidOffset(size, coarseLevel) + tile.y * outputSize.x + tile.x), asuint(depth));
}
