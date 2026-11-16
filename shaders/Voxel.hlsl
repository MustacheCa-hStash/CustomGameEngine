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
    const float4 placement = placements[firstPlacement + instance / brickCount];
    const float3 lower = origin + float3(brick.coordinate) * (4.0 * cellSize);
    // Reverse the winding if the mesh transform mirrors the local grid.
    const uint corner = vertex ^ mirrored;
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
    output.position = mul(float4(world, 1), viewProjection);
    output.worldAndViewDepth = float4(world, output.position.w);
    // Keep an exit face at the far plane if the last part of a brick extends beyond it.
    output.position.z = min(output.position.z, output.position.w);
    float3 assetEye = eye;
    assetEye.xz = rotateXZ(assetEye.xz - placement.xy, -placement.z);
    output.eyeInCells = (mul(float4(assetEye / placement.w, 1), assetToMesh).xyz - lower) / cellSize;
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

float leafDiffuse(uint packedNormal, float yaw, float3 toEye)
{
    const float2 xy = float2(packedNormal & 4095, (packedNormal >> 12) & 4095) * (2.0 / 4095.0) - 1.0;
    float3 axis = float3(xy, 1.0 - abs(xy.x) - abs(xy.y));
    if (axis.z < 0) axis.xy = (1.0 - abs(xy.yx)) * (2.0 * float2(xy >= 0) - 1.0);
    axis = normalize(mul(axis, transpose((float3x3)assetToMesh)));
    axis.xz = rotateXZ(axis.xz, yaw);
    if (dot(axis, toEye) < 0) axis = -axis;
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

float weightedDiffuse(uint address, float yaw, float3 toEye, float3 localToEye)
{
    const uint2 packed = cellNormals.Load2(address);
    float weight = (packed.x >> 24) * (1.0 / 255.0);
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
    return lerp(leafDiffuse(packed.y, yaw, toEye), leafDiffuse(packed.x, yaw, toEye), weight);
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
    const float offset = hit / lengthToBack;
    const float viewDepth = clamp(backViewDepth * (1 + offset), nearPlane, farPlane);
    Output output;
    output.depth = saturate(projectionZScale + projectionZOffset / viewDepth);
    if (viewMode == 6 || viewMode == 7)
    {
        output.color = viewMode == 6 ? 0 : float4(1, 0.56, 0.18, 1);
        return output;
    }
    if (viewMode == 2)
    {
        const uint value = uint(round(saturate(viewDepth / farPlane) * 16777215.0));
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
    [branch]
    if (useNormals) diffuse = weightedDiffuse(address, input.yawAndScale.x, eye - worldHit, -path);
    if (viewMode == 4)
    {
        output.color = float4(diffuse.xxx, 1);
        return output;
    }
    float visibility = 1;
    if (receivesShadows)
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
    float3 lit = saturate(baseColor * (0.35 + 0.65 * diffuse * visibility));
    output.color = float4(displayColor(lit), 1);
    return output;
}
