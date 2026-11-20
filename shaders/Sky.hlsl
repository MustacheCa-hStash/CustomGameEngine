cbuffer SkyConstants : register(b0)
{
    row_major float4x4 viewRayTransform;
    float3 toSun;
    uint encodedCamera;
    uint skyOnly;
}

Texture2D<float4> panorama : register(t0);
SamplerState skySampler : register(s0);

struct SkyVertex
{
    float4 position : SV_Position;
    float3 worldRay : TEXCOORD0;
};

SkyVertex VSMain(uint vertex : SV_VertexID)
{
    const float2 clip[] = {float2(-1, -1), float2(-1, 3), float2(3, -1)};
    const float2 corner = clip[vertex];
    SkyVertex output;
    output.position = float4(corner, 1, 1);
    output.worldRay = mul(float3(corner, -1), (float3x3)viewRayTransform);
    return output;
}

float4 PSMain(SkyVertex input) : SV_Target
{
    const float3 ray = normalize(input.worldRay);
    const float2 uv = float2(0.5 + atan2(ray.x, ray.z + 1e-8) * 0.1591549431,
                             0.5 - asin(clamp(ray.y, -1.0, 1.0)) * 0.3183098862);
    // Use the short distance across the longitude seam when choosing a mip.
    float2 dx = ddx(uv);
    float2 dy = ddy(uv);
    dx.x -= round(dx.x);
    dy.x -= round(dy.x);
    const float3 sky = panorama.SampleGrad(skySampler, uv, dx, dy).rgb;

    const float sun = skyOnly ? 0.0 : smoothstep(0.999986, 0.999992, dot(ray, toSun));
    const float3 linearColor = lerp(sky, float3(1.0, 0.96, 0.87), sun);
    if (!encodedCamera) return float4(linearColor, 1);
    const float3 display = lerp(12.92 * linearColor, 1.055 * pow(linearColor, 1.0 / 2.4) - 0.055,
                                step(0.0031308, linearColor));
    return float4(display, 1);
}
