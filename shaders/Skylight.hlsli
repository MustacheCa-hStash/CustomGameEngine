cbuffer Skylight : register(b2)
{
    float4 skyIrradiance[9];
}

float3 evaluateSkylight(float3 n)
{
    return max(0.0, skyIrradiance[0].rgb +
                    skyIrradiance[1].rgb * n.x + skyIrradiance[2].rgb * n.y + skyIrradiance[3].rgb * n.z +
                    skyIrradiance[4].rgb * (n.x * n.y) + skyIrradiance[5].rgb * (n.y * n.z) +
                    skyIrradiance[6].rgb * (3.0 * n.y * n.y - 1.0) + skyIrradiance[7].rgb * (n.x * n.z) +
                    skyIrradiance[8].rgb * (n.x * n.x - n.z * n.z));
}
