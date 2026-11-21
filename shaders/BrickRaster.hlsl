#include "Voxel.hlsl"

RWByteAddressBuffer hits : register(u0);
ByteAddressBuffer visibleCells : register(t4);

struct BrickWork
{
    float4 placement;
    uint4 rect;
    uint firstPixel;
    uint3 padding;
};
StructuredBuffer<BrickWork> work : register(t5);

float3 pixelRay(float2 screen)
{
    float2 ndc = screen / rasterSize * float2(2,-2) + float2(-1,1);
    return screenRayForward.xyz + screenRayX.xyz * ndc.x + screenRayY.xyz * ndc.y;
}

uint hitAddress(BrickWork item, uint2 pixel)
{
    return 8 * (item.firstPixel + pixel.y * item.rect.z + pixel.x);
}

[numthreads(8,8,1)]
void BrickClearCS(uint3 id : SV_DispatchThreadID)
{
    BrickWork item = work[id.z];
    if (all(id.xy < item.rect.zw))
        hits.Store2(hitAddress(item,id.xy), uint2(~0u,~0u));
}

[numthreads(64,1,1)]
void BrickVisibilityCS(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    BrickWork item = work[id.y];
    uint4 rasterRect = item.rect;
    if (index >= brickCount) return;
    Brick brick = bricks[index];
    float3 base,a,b,c;
    brickBasis(4*index,item.placement,base,a,b,c);
    // Homogeneous interval of the occupied sub-box, conservative under perspective.
    const uint3 shifts = uint3(0,3,6);
    float3 lo = (brick.packedProxyBounds >> shifts) & 7;
    float3 hi = (brick.packedProxyBounds >> (shifts+9)) & 7;
    float3 halfSize = .5*(hi-lo), mid = .5*(lo+hi);
    float3 center = base + mid.x*a + mid.y*b + mid.z*c;
    float4 clip = mul(float4(center,1),viewProjection);
    float4 extent = abs(mul(float4(a*halfSize.x,0),viewProjection)) +
                    abs(mul(float4(b*halfSize.y,0),viewProjection)) +
                    abs(mul(float4(c*halfSize.z,0),viewProjection));
    if (clip.w + extent.w <= nearPlane || clip.w - extent.w >= farPlane) return;
    float front = max(nearPlane,clip.w-extent.w), back = max(nearPlane,clip.w+extent.w);
    float2 low = min((clip.xy-extent.xy)/front,(clip.xy-extent.xy)/back);
    float2 high = max((clip.xy+extent.xy)/front,(clip.xy+extent.xy)/back);
    int2 first = max(int2(rasterRect.xy),int2(ceil((.5+.5*float2(low.x,-high.y))*rasterSize-.501)));
    int2 last = min(int2(rasterRect.xy+rasterRect.zw)-1,int2(floor((.5+.5*float2(high.x,-low.y))*rasterSize-.499)));
    if (any(first > last)) return;
    float3 x = cross(b,c), y = cross(c,a), z = cross(a,b);
    float det = dot(a,x);
    float3x3 toCells = float3x3(x,y,z) / det;
    for (int py=first.y; py<=last.y; ++py)
    for (int px=first.x; px<=last.x; ++px)
    {
        float3 ray = pixelRay(float2(px,py)+.5);
        float3 originInCells = mid + mul(toCells,(eye-center) + clip.w*ray);
        float3 direction = mul(toCells,ray);
        float entry,exit;
        uint bit;
        float cellsPerMeter = length(direction);
        if (firstOccupied(originInCells,direction/cellsPerMeter,(nearPlane-clip.w)*cellsPerMeter,
                          (farPlane-clip.w)*cellsPerMeter,brick.mask,entry,exit,bit))
        {
            float depth = clip.w + .5*(entry+exit)/cellsPerMeter;
            uint64_t key = (uint64_t(asuint(depth))<<32) | ((index<<6) | bit);
            uint64_t previous;
            hits.InterlockedMin64(hitAddress(item,uint2(px,py)-rasterRect.xy),key,previous);
        }
    }
}

struct ResolvePixel
{
    float4 position : SV_Position;
    nointerpolation uint batch : TEXCOORD0;
};

ResolvePixel BrickResolveVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const uint corners[6] = {0,1,2,2,1,3};
    uint corner = corners[vertex];
    uint4 rect = work[instance].rect;
    float2 screen = float2(rect.xy + uint2(corner & 1, corner >> 1) * rect.zw);
    ResolvePixel pixel;
    pixel.position = float4(screen/rasterSize*float2(2,-2)+float2(-1,1),0,1);
    pixel.batch = instance;
    return pixel;
}

Output BrickResolvePS(ResolvePixel pixel)
{
    BrickWork item = work[pixel.batch];
    uint2 coord = uint2(clamp(int2(pixel.position.xy)-int2(item.rect.xy),0,int2(item.rect.zw)-1));
    uint2 key = visibleCells.Load2(hitAddress(item,coord));
    if (key.y == ~0u || (key.x >> 6) >= brickCount)
    {
        discard;
        return (Output)0;
    }
    Pixel input = (Pixel)0;
    input.brickIndex = min(key.x >> 6,brickCount-1);
    input.mask = bricks[input.brickIndex].mask;
    float depth = asfloat(key.y);
    float3 ray = pixelRay(pixel.position.xy);
    input.worldAndViewDepth = float4(eye + ray*depth,depth);
    input.yawAndScale = item.placement.zw;
    return shadeCell(input,ray,1,depth,0,0,key.x & 63);
}
