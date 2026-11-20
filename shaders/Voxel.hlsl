#include "LeafLighting.hlsli"
#include "ProbeLighting.hlsli"

cbuffer Frame : register(b0)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    row_major float4x4 meshToAsset;
    row_major float4x4 assetToMesh;
    float3 eye;
    float nearPlane;
    float farPlane;
    float projectionZScale;
    float projectionZOffset;
    float shadowBias;
    float3 baseColor;
    uint useNormals;
    uint viewMode;
    uint encodeColor;
    uint receivesShadows;
    uint mirrored;
    float3 toSun;
    float transmission;
    uint shadowReceiver;
    uint tightBrickProxies;
    uint encodedCamera;
    uint directionalSkylight;
    uint lightingMode;
    float3 transmissionColor;
    uint thinLeaf;
    uint orientedBricks;
    uint2 probePadding;
    float4 probeOriginSpacing;
    uint4 probeSize;
    float4 boughWind;
};

cbuffer Draw : register(b1)
{
    float3 origin;
    float cellSize;
    uint brickCount;
    uint firstPlacement;
};

struct Brick
{
    int3 coordinate;
    uint2 mask;
    uint packedProxyBounds;
};

StructuredBuffer<Brick> bricks : register(t0);
StructuredBuffer<float4> placements : register(t1);
Texture2D<float> shadowMap : register(t2);
ByteAddressBuffer cellNormals : register(t3);
SamplerComparisonState shadowSampler : register(s1);

static const uint bakedLeafReceiver = 1;
static const uint cellMidpointReceiver = 2;

struct Pixel
{
    float4 position : SV_Position;
    sample float3 backInCells : TEXCOORD0;
    sample float4 worldAndViewDepth : TEXCOORD1;
    nointerpolation float3 eyeInCells : TEXCOORD2;
    nointerpolation uint2 mask : TEXCOORD3;
    nointerpolation uint brickIndex : TEXCOORD4;
    nointerpolation float2 yawAndScale : TEXCOORD5;
};

float2 rotateXZ(float2 xz, float angle)
{
    float s, c;
    sincos(angle, s, c);
    return float2(c * xz.x + s * xz.y, -s * xz.x + c * xz.y);
}

Pixel VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const Brick brick = bricks[instance % brickCount];
    const uint axes = 4 * (instance % brickCount);
    const float4 placement = orientedBricks ? float4(0,0,0,1) : placements[firstPlacement + instance / brickCount];
    const float3 lower = origin + float3(brick.coordinate) * (4.0 * cellSize);
    // Reverse the winding if the mesh transform mirrors the local grid.
    const uint corner = vertex ^ (orientedBricks ? uint(placements[axes].w) : mirrored);
    Pixel output;
    const uint3 cornerBits = (corner >> uint3(0, 1, 2)) & 1;
    output.backInCells = 4.0 * float3(cornerBits);
    if (tightBrickProxies)
    {
        // Keep interpolation and traversal in the original brick coordinate system.
        const uint3 shifts = uint3(0, 3, 6) + 9 * cornerBits;
        // Leave a small guard band for rasterizer subpixel rounding at occupied edges.
        const float guard = 1.0 / 64.0;
        output.backInCells = clamp(float3((brick.packedProxyBounds >> shifts) & 7) +
            (2.0 * float3(cornerBits) - 1.0) * guard, 0.0, 4.0);
    }
    float3 world = mul(float4(lower + output.backInCells * cellSize, 1), meshToAsset).xyz;
    world.xz = rotateXZ(world.xz, placement.z);
    world *= placement.w;
    world.xz += placement.xy;
    float3 orientedEye = 0;
    if (orientedBricks)
    {
        float3 base = placements[axes].xyz, a = placements[axes+1].xyz;
        float3 b = placements[axes+2].xyz, c = placements[axes+3].xyz;
        float height = (base.y - boughWind.z) * boughWind.w;
        float weight = saturate(height);
        float2 slope = height > 0 && height < 1 ? 2 * weight * boughWind.w * boughWind.xy : 0;
        base.xz += weight * weight * boughWind.xy;
        a.xz += a.y * slope; b.xz += b.y * slope; c.xz += c.y * slope;
        world = base + output.backInCells.x * a + output.backInCells.y * b + output.backInCells.z * c;
        float3 delta = eye - base;
        orientedEye = float3(dot(delta,cross(b,c)),dot(delta,cross(c,a)),dot(delta,cross(a,b))) / dot(a,cross(b,c));
    }
    output.position = mul(float4(world, 1), viewProjection);
    output.worldAndViewDepth = float4(world, output.position.w);
    // Keep an exit face at the far plane if the last part of a brick extends beyond it.
    output.position.z = min(output.position.z, output.position.w);
    float3 assetEye = eye;
    assetEye.xz = rotateXZ(assetEye.xz - placement.xy, -placement.z);
    output.eyeInCells = (mul(float4(assetEye / placement.w, 1), assetToMesh).xyz - lower) / cellSize;
    if (orientedBricks) output.eyeInCells = orientedEye;
    output.mask = brick.mask;
    output.brickIndex = instance % brickCount;
    output.yawAndScale = placement.zw;
    return output;
}

bool firstOccupied(float3 back, float3 direction, float tMin, float tMax, uint2 mask,
                   out float hit, out float hitExit, out uint hitBit)
{
    hit = 0;
    hitExit = 0;
    hitBit = 0;
    float3 next, stride;
    int3 step = int3(direction > 0) - int3(direction < 0);
    [unroll]
    for (uint axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] == 0)
        {
            if (back[axis] < 0 || back[axis] >= 4) return false;
            continue;
        }
        float a = -back[axis] / direction[axis];
        float b = (4.0 - back[axis]) / direction[axis];
        tMin = max(tMin, min(a, b));
        tMax = min(tMax, max(a, b));
    }
    if (tMin >= tMax) return false;
    const float3 entry = back + tMin * direction;
    int3 cell = clamp(int3(floor(entry)), 0, 3);
    [unroll]
    for (uint axis = 0; axis < 3; ++axis)
    {
        if (step[axis] < 0) cell[axis] = clamp(int(ceil(entry[axis])) - 1, 0, 3);
        const int edge = cell[axis] + (step[axis] > 0 ? 1 : 0);
        next[axis] = step[axis] != 0 ? (edge - back[axis]) / direction[axis] : 1e30;
        stride[axis] = step[axis] != 0 ? rcp(abs(direction[axis])) : 1e30;
    }
    // A ray crosses at most nine interior grid planes of one 4^3 brick.
    [loop]
    for (uint iteration = 0; iteration < 12; ++iteration)
    {
        const float boundary = min(next.x, min(next.y, next.z));
        const uint bit = uint(cell.x + 4 * cell.y + 16 * cell.z);
        const uint word = bit < 32 ? mask.x : mask.y;
        const float cellExit = min(boundary, tMax);
        if (cellExit > tMin + 0.00001 && (word & (1u << (bit & 31))))
        {
            hit = tMin;
            hitExit = cellExit;
            hitBit = bit;
            return true;
        }
        if (boundary >= tMax) return false;
        const bool3 crossed = next <= boundary + 0.00001;
        cell += int3(crossed) * step;
        if (any(cell < 0) || any(cell >= 4)) return false;
        next += float3(crossed) * stride;
        tMin = max(tMin, boundary);
    }
    return false;
}

float ShadowPS(Pixel input) : SV_Depth
{
    // Orthographic shadow-map rays are parallel and point away from the sun.
    const float3 worldRay = -toSun;
    float3 assetRay = worldRay;
    assetRay.xz = rotateXZ(assetRay.xz, -input.yawAndScale.x);
    const float3 localRay = mul(float4(assetRay, 0), assetToMesh).xyz;
    const float localRayLength = length(localRay);
    const float depthPerCell = mul(float4(worldRay, 0), lightViewProjection).z *
                               (cellSize * input.yawAndScale.y / localRayLength);
    const float backDepth = mul(float4(input.worldAndViewDepth.xyz, 1), lightViewProjection).z;
    float hit, hitExit;
    uint bit;
    if (!firstOccupied(input.backInCells, localRay / localRayLength, -backDepth / depthPerCell,
                       min(0, (1 - backDepth) / depthPerCell), input.mask, hit, hitExit, bit)) discard;
    return saturate(backDepth + (0.5 * (hit + hitExit)) * depthPerCell);
}

float3 leafNormal(uint packedNormal, float yaw, float3 toEye)
{
    const float2 xy = float2(packedNormal & 4095, (packedNormal >> 12) & 4095) * (2.0 / 4095.0) - 1.0;
    float3 axis = float3(xy, 1.0 - abs(xy.x) - abs(xy.y));
    if (axis.z < 0) axis.xy = (1.0 - abs(xy.yx)) * (2.0 * float2(xy >= 0) - 1.0);
    axis = normalize(mul(axis, transpose((float3x3)assetToMesh)));
    axis.xz = rotateXZ(axis.xz, yaw);
    return dot(axis, toEye) < 0 ? -axis : axis;
}

float leafDiffuse(float3 axis)
{
    const float facing = dot(axis, toSun);
    return saturate(facing) + transmission * saturate(-facing);
}

uint cellAttributeAddress(Pixel input, uint bit)
{
    const uint preceding = (1u << (bit & 31)) - 1u;
    const uint rank = bit < 32 ? countbits(input.mask.x & preceding)
                              : countbits(input.mask.x) + countbits(input.mask.y & preceding);
    const uint cell = cellNormals.Load(input.brickIndex * 4) + rank;
    return brickCount * 4 + cell * 16;
}

float leafWeight(uint address, uint packedNormal, float3 localToEye)
{
    float weight = (packedNormal >> 24) * (1.0 / 255.0);
    [branch]
    if (useNormals == 2)
    {
        const uint2 directionWeights = cellNormals.Load2(address + 8);
        const uint3 shifts = uint3(localToEye.x >= 0, localToEye.y >= 0, localToEye.z >= 0) * 8;
        const float3 values = float3((directionWeights.x >> shifts.x) & 255,
                                    (directionWeights.x >> (16 + shifts.y)) & 255,
                                    (directionWeights.y >> shifts.z) & 255);
        const float3 blend = abs(localToEye);
        weight = dot(values, blend) / (255.0 * (blend.x + blend.y + blend.z));
    }
    return weight;
}

float3 leafShadowReceiver(Pixel input, uint bit, uint address, float3 entryInCells, float3 worldEntry)
{
    const uint packed = cellNormals.Load(address + 12) >> 16;
    if ((packed & 0x8000u) == 0) return worldEntry;
    const float3 cell = float3(bit & 3u, (bit >> 2) & 3u, bit >> 4);
    const float3 leaf = cell + float3(packed & 31u, (packed >> 5) & 31u, (packed >> 10) & 31u) / 31.0;
    // Apply only the instance's linear transform to the offset from the ray entry.
    float3 delta = mul((leaf - entryInCells) * cellSize, (float3x3)meshToAsset);
    delta.xz = rotateXZ(delta.xz, input.yawAndScale.x);
    return worldEntry + delta * input.yawAndScale.y;
}

float3 displayColor(float3 rgb)
{
    if (encodeColor)
        return lerp(12.92 * rgb, 1.055 * pow(rgb, 1.0 / 2.4) - 0.055, step(0.0031308, rgb));
    return rgb;
}

struct Output
{
    float4 color : SV_Target;
    float depth : SV_Depth;
};

Output PSMain(Pixel input)
{
    const float3 path = input.backInCells - input.eyeInCells;
    const float lengthToBack = length(path);
    const float backViewDepth = input.worldAndViewDepth.w;
    if (lengthToBack <= 0 || backViewDepth <= 0) discard;
    float hit, hitExit;
    uint hitBit;
    if (!firstOccupied(input.backInCells, path / lengthToBack,
                       (nearPlane / backViewDepth - 1) * lengthToBack,
                       min(0, (farPlane / backViewDepth - 1) * lengthToBack), input.mask, hit, hitExit, hitBit)) discard;
    const float offset = (orientedBricks ? 0.5 * (hit + hitExit) : hit) / lengthToBack;
    const float viewDepth = clamp(backViewDepth * (1 + offset), nearPlane, farPlane);
    Output output;
    output.depth = saturate(projectionZScale + projectionZOffset / viewDepth);
    if (viewMode == 6 || viewMode == 7)
    {
        output.color = viewMode == 6 ? 0 : float4(1, 0.56, 0.18, 1);
        return output;
    }
    if (viewMode == 8 || viewMode == 9)
    {
        output.color = viewMode == 8 ? float4(0,1,0,1) : float4(1,0,0,1);
        return output;
    }
    if (viewMode == 2 || viewMode == 10)
    {
        const uint value = uint(round(saturate(viewDepth / (viewMode == 10 ? 1000.0 : farPlane)) * 16777215.0));
        output.color = float4(float3(value & 255, (value >> 8) & 255, (value >> 16) & 255) / 255.0, 0.25);
        return output;
    }
    if (viewMode == 1)
    {
        output.color = float4(0.15, 0.55, 0.32, 1);
        return output;
    }
    if (viewMode == 3)
    {
        output.color = float4(displayColor(saturate(baseColor)), 1);
        return output;
    }

    const float3 back = input.worldAndViewDepth.xyz;
    const float3 worldHit = back + (back - eye) * offset;
    const bool useBakedReceiver = receivesShadows && shadowReceiver == bakedLeafReceiver && viewMode != 4;
    uint address = 0;
    [branch]
    if (useNormals || useBakedReceiver) address = cellAttributeAddress(input, hitBit);
    float diffuse = 0.6; // Shared preview lighting for occupancy-only bakes.
    // Occupancy-only bakes use the SH mean because they lack a surface normal.
    float3 ambient = directionalSkylight ? skyIrradiance[0].rgb : 0.35;
    LeafIncidentLight leafLight = (LeafIncidentLight)0;
    [branch]
    if (useNormals)
    {
        const uint2 packed = cellNormals.Load2(address);
        const float weight = leafWeight(address, packed.x, -path);
        const float3 a = leafNormal(packed.x, input.yawAndScale.x, eye - worldHit);
        const float3 b = leafNormal(packed.y, input.yawAndScale.x, eye - worldHit);
        diffuse = lerp(leafDiffuse(b), leafDiffuse(a), weight);
        if (thinLeaf)
        {
            LeafIncidentLight lightA = leafIncidentLight(a, dot(a, toSun), directionalSkylight);
            LeafIncidentLight lightB = leafIncidentLight(b, dot(b, toSun), directionalSkylight);
            localDiffuse(worldHit, a, probeOriginSpacing, probeSize.xyz, true, lightA.skyFront, lightA.skyBack);
            localDiffuse(worldHit, b, probeOriginSpacing, probeSize.xyz, true, lightB.skyFront, lightB.skyBack);
            leafLight.skyFront = lerp(lightB.skyFront, lightA.skyFront, weight);
            leafLight.skyBack = lerp(lightB.skyBack, lightA.skyBack, weight);
            leafLight.sunCosines = lerp(lightB.sunCosines, lightA.sunCosines, weight);
        }
        else if (directionalSkylight)
        {
            float3 aSky = evaluateSkylight(a), bSky = evaluateSkylight(b);
            float3 unused = 0;
            localDiffuse(worldHit, a, probeOriginSpacing, probeSize.xyz, false, aSky, unused);
            localDiffuse(worldHit, b, probeOriginSpacing, probeSize.xyz, false, bSky, unused);
            ambient = lerp(bSky, aSky, weight);
        }
    }
    else
    {
        // With no baked normal, average the six directional corrections.
        float3 opposite = ambient;
        localDiffuse(worldHit, 0.57735027.xxx, probeOriginSpacing, probeSize.xyz, true, ambient, opposite);
        ambient = 0.5 * (ambient + opposite);
    }
    if (viewMode == 4)
    {
        output.color = float4(diffuse.xxx, 1);
        return output;
    }
    float visibility = 1;
    if (receivesShadows && lightingMode != skyOnly)
    {
        float3 receiver = worldHit;
        [branch]
        if (useBakedReceiver)
            receiver = leafShadowReceiver(input, hitBit, address, input.backInCells + path * offset, worldHit);
        else if (shadowReceiver == cellMidpointReceiver)
            receiver += (back - eye) * (0.5 * (hitExit - hit) / lengthToBack);
        const float4 light = mul(float4(receiver, 1), lightViewProjection);
        const float3 projected = light.xyz / light.w;
        const float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
        if (all(uv >= 0) && all(uv <= 1) && projected.z >= 0 && projected.z <= 1)
            visibility = shadowMap.SampleCmpLevelZero(shadowSampler, uv, projected.z - shadowBias);
    }
    if (viewMode == 5)
    {
        output.color = float4(visibility.xxx, 1);
        return output;
    }
    if (lightingMode == shadowVisibility)
    {
        output.color = float4(visibility.xxx, 1);
        return output;
    }
    if (thinLeaf)
    {
        output.color = float4(shadeLeaf(leafLight, visibility, lightingMode, baseColor, transmissionColor, transmission), 1);
        return output;
    }
    if (lightingMode == transmissionOnly)
    {
        output.color = float4(0, 0, 0, 1);
        return output;
    }
    if (lightingMode == sunOnly) ambient = 0;
    const float direct = lightingMode == skyOnly ? 0.0 : 0.65 * diffuse * visibility;
    float3 lit = baseColor * (ambient + direct);
    output.color = float4(encodedCamera ? displayColor(saturate(lit)) : lit, 1);
    return output;
}
