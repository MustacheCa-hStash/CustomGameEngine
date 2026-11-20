struct ProbeLobe
{
    float3 correction;
    uint distanceMoments;
};

StructuredBuffer<ProbeLobe> diffuseProbes : register(t6);

uint3 probeLobes(float3 direction)
{
    return uint3(0, 2, 4) + uint3(direction < 0);
}

float probeVisibility(uint probe, float3 offset, float spacing)
{
    const float3 direction = abs(offset);
    const uint3 lobes = probeLobes(offset);
    const uint axis = direction.x >= max(direction.y, direction.z) ? lobes.x :
                      direction.y >= direction.z ? lobes.y : lobes.z;
    const uint bits = diffuseProbes[probe + axis].distanceMoments;
    const float mean = f16tof32(bits & 65535);
    const float variance = max(f16tof32(bits >> 16) - mean * mean, 0.0001);
    const float beyond = max(0, length(offset) / (4 * spacing) - mean - 0.055);
    const float visibility = variance / (variance + beyond * beyond);
    return max(0.025, visibility * visibility * visibility);
}

void localDiffuse(float3 position, float3 normal, float4 originSpacing, uint3 gridSize,
                  bool bothSides, inout float3 front, inout float3 back)
{
    if (!gridSize.x) return;
    const float spacing = originSpacing.w;
    const float3 biasedPosition = position + normal * (0.12 * spacing);
    const float3 grid = (biasedPosition - originSpacing.xyz) / spacing;
    const float3 boundary = min(grid, float3(gridSize - 1) - grid);
    const float blend = saturate(min(boundary.x, boundary.z)) *
                        saturate(min(grid.y + 1, boundary.y + max(0, -grid.y)));
    if (blend <= 0) return;

    const float3 clamped = clamp(grid, 0, float3(gridSize - 1));
    const uint3 base = min(uint3(clamped), gridSize - 2);
    const float3 f = clamped - base;
    const uint3 lobes = probeLobes(normal);
    const float3 cosine = normal * normal;
    float3 reflected = 0;
    float3 transmitted = 0;
    float sum = 0;
    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        const uint3 delta = uint3(corner & 1, (corner >> 1) & 1, corner >> 2);
        const uint3 coord = base + delta;
        const float3 factor = lerp(1 - f, f, float3(delta));
        float weight = factor.x * factor.y * factor.z;
        const uint probe = ((coord.z * gridSize.y + coord.y) * gridSize.x + coord.x) * 6;
        weight *= probeVisibility(probe, biasedPosition - (originSpacing.xyz + coord * spacing), spacing);
        const float3 a = diffuseProbes[probe + lobes.x].correction;
        const float3 b = diffuseProbes[probe + lobes.y].correction;
        const float3 c = diffuseProbes[probe + lobes.z].correction;
        reflected += weight * (a * cosine.x + b * cosine.y + c * cosine.z);
        if (bothSides)
        {
            const float3 d = diffuseProbes[probe + (lobes.x ^ 1)].correction;
            const float3 e = diffuseProbes[probe + (lobes.y ^ 1)].correction;
            const float3 g = diffuseProbes[probe + (lobes.z ^ 1)].correction;
            transmitted += weight * (d * cosine.x + e * cosine.y + g * cosine.z);
        }
        sum += weight;
    }
    const float strength = blend / max(sum, 1e-5);
    front = max(0, front + reflected * strength);
    back = max(0, back + transmitted * strength);
}
