Texture2D<float4> sceneColor : register(t0);

cbuffer DisplayConstants : register(b0)
{
    float exposureScale;
}

float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    const float2 clip[] = {float2(-1, -1), float2(-1, 3), float2(3, -1)};
    return float4(clip[vertex], 0, 1);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    float3 rgb = max(sceneColor.Load(int3(int2(position.xy), 0)).rgb, 0.0) * exposureScale;
    const float peak = max(rgb.r, max(rgb.g, rgb.b));
    // Keep diffuse values below the knee unchanged and roll off bright values with a shared RGB scale.
    const float knee = 0.75;
    const float headroom = 1.0 - knee;
    if (peak > knee)
    {
        const float shoulder = 1.0 - headroom / (1.0 + (peak - knee) / headroom);
        rgb *= shoulder / peak;
    }
    const float3 encoded = lerp(12.92 * rgb, 1.055 * pow(rgb, 1.0 / 2.4) - 0.055, step(0.0031308, rgb));
    return float4(encoded, 1.0);
}
