#include "LeafLighting.hlsli"
#include "ProbeLighting.hlsli"

cbuffer DrawConstants : register(b0)
{
    row_major float4x4 model;
    row_major float4x4 normalToWorld;
    float4 surfaceColor;
    uint textureCoordinateSet;
    uint hasColorTexture;
    float alphaCutoff;
    float transmission;
    uint movesWithWind;
    uint receivesShadows;
    int twoSidedNormalSign;
    uint lodTint;
    uint highlightedLodGroup;
    uint firstLodTag;
    uint lodLevel;
    uint shadowView;
    uint usesMeshInstances;
    float3 transmissionColor;
    uint transmissionFlags;
};

cbuffer FrameConstants : register(b1)
{
    row_major float4x4 viewProjection;
    row_major float4x4 lightViewProjection;
    float3 toSun;
    float shadowBias;
    float2 windOffset;
    float windStart;
    float inverseWindHeight;
    float normalBiasWorld;
    uint useCascades;
    uint showCascadeColors;
    uint filterCascades;
    float4 cascadeSplits;
    row_major float4x4 cascadeViewProjections[3];
    float4 cascadeShadowBias;
    float4 cascadeNormalBias;
    float4 cascadeBlendWidths;
    float4 cascadeDepthPerTexel;
    float4 lightRight;
    uint encodedCamera;
    uint directionalSkylight;
    uint lightingMode;
    uint probePadding;
    float4 probeOriginSpacing;
    uint4 probeSize;
};

Texture2D<float4> colorTexture : register(t0);
SamplerState colorSampler : register(s0);
Texture2D<float> shadowMap : register(t1);
SamplerComparisonState shadowSampler : register(s1);
StructuredBuffer<uint3> lodTags : register(t2);
Texture2DArray<float> cascadeShadowMaps : register(t3);
Texture2D<float4> transmissionColorTexture : register(t4);
Texture2D<float4> transmissionStrengthTexture : register(t5);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 instance : INSTANCE;
    float3 meshTranslation : MESHINSTANCE0;
    float4 meshRotation : MESHINSTANCE1;
    float3 meshScale : MESHINSTANCE2;
};

struct PixelInput
{
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float4 texcoords : TEXCOORD0;
    float4 lightPosition : TEXCOORD1;
    float4 worldPositionAndViewDepth : TEXCOORD2;
    nointerpolation uint meshInstanceId : TEXCOORD3;
};

struct ShadowInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float2 materialUv(VertexInput input)
{
    return textureCoordinateSet == 0 ? input.uv0 : input.uv1;
}

float2 rotateXZ(float2 xz, float angle)
{
    float sine, cosine;
    sincos(angle, sine, cosine);
    return float2(cosine * xz.x + sine * xz.y, -sine * xz.x + cosine * xz.y);
}

float3 rotateMeshInstance(float3 value, float4 rotation)
{
    const float3 twiceCross = 2.0 * cross(rotation.xyz, value);
    return value + rotation.w * twiceCross + cross(rotation.xyz, twiceCross);
}

float4 bendPosition(VertexInput input, out float2 bendSlope)
{
    float3 position = input.position;
    if (usesMeshInstances)
    {
        position = rotateMeshInstance(position * input.meshScale, input.meshRotation) + input.meshTranslation;
    }
    float4 asset = mul(float4(position, 1.0), model);
    float4 world = asset;
    world.xz = rotateXZ(asset.xz, input.instance.z);
    world.xyz *= input.instance.w;
    world.xz += input.instance.xy;
    bendSlope = 0.0;
    if (movesWithWind)
    {
        // Use asset height to keep the lower trunk fixed at any instance scale.
        // Apply the shared wind direction after yaw, with amplitude scaled per tree.
        float height = (asset.y - windStart) * inverseWindHeight;
        float weight = saturate(height);
        world.xz += windOffset * (weight * weight * input.instance.w);
        if (height > 0.0 && height < 1.0)
        {
            // Uniform instance scale cancels out of displacement per unit of world height.
            bendSlope = windOffset * (2.0 * weight * inverseWindHeight);
        }
    }
    return world;
}

PixelInput VSMain(VertexInput input, uint instanceId : SV_InstanceID)
{
    PixelInput output;
    float2 bendSlope;
    float4 world = bendPosition(input, bendSlope);
    output.position = mul(world, viewProjection);
    float3 normal = input.normal;
    if (usesMeshInstances)
    {
        normal = rotateMeshInstance(normal / input.meshScale, input.meshRotation);
    }
    output.worldNormal = mul(float4(normal, 0.0), normalToWorld).xyz;
    output.worldNormal.xz = rotateXZ(output.worldNormal.xz, input.instance.z);
    // The normal's Y component changes under the horizontal shear.
    output.worldNormal.y -= dot(bendSlope, output.worldNormal.xz);
    output.texcoords = float4(input.uv0, input.uv1);
    output.lightPosition = mul(world, lightViewProjection);
    output.worldPositionAndViewDepth = float4(world.xyz, output.position.w);
    output.meshInstanceId = instanceId;
    return output;
}

float3 encodeSrgb(float3 linearColor)
{
    linearColor = saturate(linearColor);
    return lerp(12.92 * linearColor, 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055,
                step(0.0031308, linearColor));
}

float4 sampleBaseColor(float2 uv)
{
    float4 baseColor = surfaceColor;
    if (hasColorTexture)
    {
        baseColor *= colorTexture.Sample(colorSampler, uv);
    }
    return baseColor;
}

void applyCutout(float alpha)
{
    if (alphaCutoff >= 0.0)
    {
        clip(alpha - alphaCutoff);
    }
}

float sunVisibility(float4 lightPosition, float3 worldNormal, float normalToSun)
{
    if (normalBiasWorld > 0.0)
    {
        // Use the normal facing the light, including for backlit foliage.
        float3 biasNormal = normalToSun < 0.0 ? -worldNormal : worldNormal;
        float grazingWeight = sqrt(saturate(1.0 - normalToSun * normalToSun));
        float3 worldOffset = biasNormal * (normalBiasWorld * grazingWeight);
        // w = 0 applies only the linear part of the light transform to the offset.
        lightPosition += mul(float4(worldOffset, 0.0), lightViewProjection);
    }
    float3 projected = lightPosition.xyz / lightPosition.w;
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || projected.z < 0.0 || projected.z > 1.0)
    {
        return 1.0;
    }
    return shadowMap.SampleCmpLevelZero(shadowSampler, uv, projected.z - shadowBias);
}

uint cascadeForDepth(float depth)
{
    return uint(depth >= cascadeSplits.x) + uint(depth >= cascadeSplits.y) + uint(depth >= cascadeSplits.z);
}

float sampleCascade(float3 worldPosition, uint cascade, float3 grazingNormal, float2 receiverSlope)
{
    worldPosition += grazingNormal * cascadeNormalBias[cascade];
    float4 lightPosition = mul(float4(worldPosition, 1), cascadeViewProjections[cascade]);
    float3 projected = lightPosition.xyz / lightPosition.w;
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || projected.z < 0.0 || projected.z > 1.0) return 1.0;
    float3 location = float3(uv, cascade);
    float reference = projected.z - cascadeShadowBias[cascade];
    [branch] if (filterCascades)
    {
        // Compare each bilinear tap at the depth of the receiver plane at that tap.
        float2 step = receiverSlope * cascadeDepthPerTexel[cascade];
        return 0.25 * (cascadeShadowMaps.SampleCmpLevelZero(shadowSampler, location, reference - step.x - step.y, int2(-1, -1)) +
                       cascadeShadowMaps.SampleCmpLevelZero(shadowSampler, location, reference + step.x - step.y, int2( 1, -1)) +
                       cascadeShadowMaps.SampleCmpLevelZero(shadowSampler, location, reference - step.x + step.y, int2(-1,  1)) +
                       cascadeShadowMaps.SampleCmpLevelZero(shadowSampler, location, reference + step.x + step.y, int2( 1,  1)));
    }
    return cascadeShadowMaps.SampleCmpLevelZero(shadowSampler, location, reference);
}

float2 shadowReceiverSlope(float3 worldPosition)
{
    float3 plane = cross(ddx(worldPosition), ddy(worldPosition));
    float towardSun = dot(plane, toSun);
    // Clamp at grazing incidence where the receiver has little area in light space.
    float divisor = max(abs(towardSun), max(0.2 * length(plane), 1e-12));
    float3 lightUp = cross(toSun, lightRight.xyz);
    return float2(dot(plane, lightRight.xyz), -dot(plane, lightUp)) *
           ((towardSun < 0.0 ? -1.0 : 1.0) / divisor);
}

float cascadeVisibility(float4 worldPositionAndDepth, uint cascade, float3 worldNormal, float normalToSun,
                        float2 receiverSlope)
{
    if (cascade >= 3) return 1.0;
    float grazingWeight = sqrt(saturate(1.0 - normalToSun * normalToSun));
    float3 grazingNormal = (normalToSun < 0.0 ? -worldNormal : worldNormal) * grazingWeight;
    float visibility = sampleCascade(worldPositionAndDepth.xyz, cascade, grazingNormal, receiverSlope);
    float width = cascadeBlendWidths[cascade];
    float end = cascadeSplits[cascade];
    [branch] if (width > 0.0 && worldPositionAndDepth.w > end - width)
    {
        float nextVisibility = 1.0;
        [branch] if (cascade < 2)
            nextVisibility = sampleCascade(worldPositionAndDepth.xyz, cascade + 1, grazingNormal, receiverSlope);
        visibility = lerp(visibility, nextVisibility, smoothstep(end - width, end, worldPositionAndDepth.w));
    }
    return visibility;
}

float3 tintColor(uint id)
{
    id ^= id >> 16;
    id *= 0x7feb352d;
    id ^= id >> 15;
    id *= 0x846ca68b;
    id ^= id >> 16;
    return 0.22 + (0.73 / 255.0) * float3(id & 255, (id >> 8) & 255, (id >> 16) & 255);
}

float4 PSMain(PixelInput input, bool isFrontFace : SV_IsFrontFace, uint primitive : SV_PrimitiveID) : SV_Target
{
    const float2 uv = textureCoordinateSet == 0 ? input.texcoords.xy : input.texcoords.zw;
    if (shadowView >= 9)
    {
        if (alphaCutoff >= 0.0) applyCutout(sampleBaseColor(uv).a);
        if (shadowView == 9)
        {
            uint value = uint(round(saturate(input.worldPositionAndViewDepth.w / 1000.0) * 16777215.0));
            return float4(float3(value & 255,(value >> 8) & 255,(value >> 16) & 255) / 255.0,1);
        }
        return shadowView == 10 ? float4(0,1,0,1) : float4(1,0,0,1);
    }
    if (shadowView == 7 || shadowView == 8)
    {
        if (alphaCutoff >= 0.0) applyCutout(sampleBaseColor(uv).a);
        return shadowView == 7 ? 0 : float4(0.22, 0.63, 0.99, 1);
    }
    if (shadowView == 3)
    {
        if (alphaCutoff >= 0.0) applyCutout(sampleBaseColor(uv).a);
        return float4(0.15, 0.55, 0.32, 1);
    }
    float4 baseColor = sampleBaseColor(uv);
    applyCutout(baseColor.a);
    uint cascade = 0;
    if (useCascades)
    {
        cascade = cascadeForDepth(input.worldPositionAndViewDepth.w);
        if (showCascadeColors)
        {
            const float3 colors[] = {float3(231, 81, 72), float3(65, 185, 89),
                                      float3(64, 133, 239), float3(92, 96, 104)};
            return float4(colors[cascade] / 255.0, 1.0);
        }
    }
    if (shadowView == 4)
        return float4(hasColorTexture ? encodeSrgb(baseColor.rgb) : baseColor.rgb, 1.0);
    if (shadowView == 0 && (lodTint != 0 || highlightedLodGroup != 0))
    {
        if (lodTint == 5)
            return float4(tintColor(lodTags[firstLodTag + input.meshInstanceId].y), 1.0);
        if (lodTint == 6)
            return float4(1.0, 0.66, 0.13, 1.0);
        uint3 tag = 0;
        if (lodTint != 4 || highlightedLodGroup != 0)
            tag = lodTags[firstLodTag + primitive];
        if (highlightedLodGroup == tag.y + 1)
        {
            return float4(1.0, 0.58, 0.12, 1.0);
        }
        if (lodTint == 4)
        {
            if (lodLevel == 3) return float4(0.35, 0.82, 0.33, 1.0);
            if (lodLevel == 4) return float4(0.25, 0.75, 0.94, 1.0);
            if (lodLevel == 6) return float4(0.78, 0.48, 0.94, 1.0);
            return float4(tintColor(lodLevel), 1.0);
        }
        if (lodTint != 0)
        {
            return float4(tintColor(tag[lodTint - 1]), 1.0);
        }
    }
    float3 worldNormal = normalize(input.worldNormal);
    // For thin double-sided leaves, evaluate lighting from the visible side.
    // A mirrored node reverses winding relative to its transformed normal.
    if (twoSidedNormalSign != 0)
    {
        worldNormal *= (isFrontFace ? 1.0 : -1.0) * twoSidedNormalSign;
    }
    float normalToSun = dot(worldNormal, toSun);
    float diffuse = saturate(normalToSun) + transmission * saturate(-normalToSun);
    if (shadowView == 5) return float4(diffuse.xxx, 1.0);
    float visibility = 1.0;
    if (receivesShadows && lightingMode != skyOnly)
    {
        if (useCascades)
        {
            // Derivatives need to run before any cascade-dependent branch.
            float2 receiverSlope = filterCascades ? shadowReceiverSlope(input.worldPositionAndViewDepth.xyz) : 0.0;
            visibility = cascadeVisibility(input.worldPositionAndViewDepth, cascade, worldNormal, normalToSun, receiverSlope);
        }
        else
            visibility = sunVisibility(input.lightPosition, worldNormal, normalToSun);
    }
    if (shadowView == 6) return float4(visibility.xxx, 1.0);
    if (shadowView != 0)
    {
        // Alpha tags the receiver for measurement. The swap chain displays RGB.
        return float4(visibility.xxx, shadowView == 1 ? 0.25 : 0.75);
    }
    if (lightingMode == shadowVisibility) return float4(visibility.xxx, 1.0);

    // Each texture's selector is 0 for no texture, 1 for TEXCOORD_0, and 2 for TEXCOORD_1.
    [branch]
    if (transmissionFlags & 1)
    {
        const uint colorUv = (transmissionFlags >> 1) & 3;
        const uint strengthUv = (transmissionFlags >> 3) & 3;
        float3 tint = transmissionColor;
        float fraction = transmission;
        if (colorUv)
            tint *= transmissionColorTexture.Sample(colorSampler, colorUv == 1 ? input.texcoords.xy : input.texcoords.zw).rgb;
        if (strengthUv)
            fraction *= transmissionStrengthTexture.Sample(colorSampler, strengthUv == 1 ? input.texcoords.xy : input.texcoords.zw).a;
        LeafIncidentLight light = leafIncidentLight(worldNormal, normalToSun, directionalSkylight);
        localDiffuse(input.worldPositionAndViewDepth.xyz, worldNormal, probeOriginSpacing,
                     probeSize.xyz, true, light.skyFront, light.skyBack);
        return float4(shadeLeaf(light, visibility, lightingMode, baseColor.rgb, tint, fraction), 1.0);
    }
    if (lightingMode == transmissionOnly) return float4(0, 0, 0, 1);
    float3 ambient = lightingMode == sunOnly ? 0.0 : directionalSkylight ? evaluateSkylight(worldNormal) : 0.35;
    float3 unusedBack = 0;
    localDiffuse(input.worldPositionAndViewDepth.xyz, worldNormal, probeOriginSpacing, probeSize.xyz,
                 false, ambient, unusedBack);
    const float direct = lightingMode == skyOnly ? 0.0 : 0.65 * diffuse * visibility;
    float3 lit = baseColor.rgb * (ambient + direct);
    return float4(encodedCamera && hasColorTexture ? encodeSrgb(lit) : lit, 1.0);
}

ShadowInput ShadowVS(VertexInput input)
{
    ShadowInput output;
    float2 bendSlope;
    output.position = mul(bendPosition(input, bendSlope), lightViewProjection);
    output.uv = materialUv(input);
    return output;
}

void ShadowPS(ShadowInput input)
{
    if (alphaCutoff >= 0.0)
    {
        applyCutout(sampleBaseColor(input.uv).a);
    }
}
