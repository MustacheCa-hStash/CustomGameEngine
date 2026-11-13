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
};

Texture2D<float4> colorTexture : register(t0);
SamplerState colorSampler : register(s0);
Texture2D<float> shadowMap : register(t1);
SamplerComparisonState shadowSampler : register(s1);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 instance : INSTANCE;
};

struct PixelInput
{
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 lightPosition : TEXCOORD1;
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

float4 bendPosition(VertexInput input, out float2 bendSlope)
{
    float4 asset = mul(float4(input.position, 1.0), model);
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

PixelInput VSMain(VertexInput input)
{
    PixelInput output;
    float2 bendSlope;
    float4 world = bendPosition(input, bendSlope);
    output.position = mul(world, viewProjection);
    output.worldNormal = mul(float4(input.normal, 0.0), normalToWorld).xyz;
    output.worldNormal.xz = rotateXZ(output.worldNormal.xz, input.instance.z);
    // The normal's Y component changes under the horizontal shear.
    output.worldNormal.y -= dot(bendSlope, output.worldNormal.xz);
    output.uv = materialUv(input);
    output.lightPosition = mul(world, lightViewProjection);
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

float4 PSMain(PixelInput input) : SV_Target
{
    float4 baseColor = sampleBaseColor(input.uv);
    applyCutout(baseColor.a);
    float3 worldNormal = normalize(input.worldNormal);
    float normalToSun = dot(worldNormal, toSun);
    float diffuse = saturate(normalToSun) + transmission * saturate(-normalToSun);
    float visibility = receivesShadows ? sunVisibility(input.lightPosition, worldNormal, normalToSun) : 1.0;
    float3 lit = baseColor.rgb * (0.35 + 0.65 * diffuse * visibility);
    // Textures are sampled as linear RGB. The current render target stores display RGB.
    return float4(hasColorTexture ? encodeSrgb(lit) : lit, 1.0);
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
