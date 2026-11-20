struct Node
{
    float3 center;
    float3 extent;
    uint firstChild;
    uint childCount;
    uint indexCount;
    uint placements;
    float error;
    uint firstPlacement;
};

struct Placement
{
    float3 translation;
    float4 rotation;
    float3 scale;
    row_major float4x4 toAsset;
    uint3 tag;
    uint leaf;
};

struct Plan
{
    float error;
    uint first;
    uint count;
};

float3 halfExtent(float3 extent, row_major float4x4 transform)
{
    return extent.x * abs(transform[0].xyz) + extent.y * abs(transform[1].xyz) + extent.z * abs(transform[2].xyz);
}

float stretch(row_major float4x4 matrix)
{
    float3 a = matrix[0].xyz, b = matrix[1].xyz, c = matrix[2].xyz;
    float ab = abs(dot(a,b)), ac = abs(dot(a,c)), bc = abs(dot(b,c));
    return sqrt(max(dot(a,a) + ab + ac, max(ab + dot(b,b) + bc, ac + bc + dot(c,c))));
}

float shadowTexelsPerUnit(row_major float4x4 toClip, float halfResolution)
{
    row_major float4x4 m = transpose(toClip);
    float xx = dot(m[0].xyz,m[0].xyz), xy = dot(m[0].xyz,m[1].xyz), yy = dot(m[1].xyz,m[1].xyz);
    return halfResolution * sqrt(0.5 * (xx + yy + length(float2(xx - yy, 2 * xy))));
}

float pixelsPerUnit(float3 center, float3 extent, row_major float4x4 toView, float3 viewWind, float2 focalAndNear)
{
    float3 p = mul(float4(center, 1), toView).xyz + viewWind.xyz;
    float3 radius = halfExtent(extent, toView) + abs(viewWind.xyz);
    float z = -p.z - radius.z;
    if (z <= focalAndNear.y) return asfloat(0x7f800000);
    float2 side = (abs(p.xy) + radius.xy) / z;
    return focalAndNear.x * (stretch(toView) / z) * sqrt(1 + dot(side, side));
}

bool outsidePlane(float4 plane, float3 center, float3 extent)
{
    float4 p = float4(center, 1);
    float tolerance = 0.0000019073486328125 * (dot(abs(plane), abs(p)) + dot(abs(plane.xyz), extent));
    return dot(plane,p) + dot(abs(plane.xyz), extent) < -tolerance;
}

bool intersectsFrustum(float3 center, float3 extent, row_major float4x4 toClip, float4 wind)
{
    row_major float4x4 p = transpose(toClip);
    return !(outsidePlane(p[3] + p[0] + float4(0, 0, 0, max(0, wind.w + wind.x)), center, extent) ||
             outsidePlane(p[3] - p[0] + float4(0, 0, 0, max(0, wind.w - wind.x)), center, extent) ||
             outsidePlane(p[3] + p[1] + float4(0, 0, 0, max(0, wind.w + wind.y)), center, extent) ||
             outsidePlane(p[3] - p[1] + float4(0, 0, 0, max(0, wind.w - wind.y)), center, extent) ||
             outsidePlane(p[2] + float4(0, 0, 0, max(0, wind.z)), center, extent) ||
             outsidePlane(p[3] - p[2] + float4(0, 0, 0, max(0, wind.w - wind.z)), center, extent));
}

uint classifyFrustumPlane(float4 plane, float shift, float3 center, float3 extent)
{
    float distance = dot(plane, float4(center, 1));
    float radius = dot(abs(plane.xyz), extent);
    float tolerance = 0.000003814697265625 * (dot(abs(plane), float4(abs(center), 1)) + radius + abs(shift));
    if (distance + radius + max(0, shift) < -tolerance) return 0;
    return distance - radius + min(0, shift) < tolerance ? 1 : 2;
}

// Return the planes that cross the bounds, or all bits if the bounds are outside.
uint crossingPlanes(float3 center, float3 extent, row_major float4x4 toClip, float4 wind,
                    out float4 lastPlane, out float lastShift)
{
    row_major float4x4 p = transpose(toClip);
    float4 planes[6] = {p[3] + p[0], p[3] - p[0], p[3] + p[1], p[3] - p[1], p[2], p[3] - p[2]};
    float shifts[6] = {wind.w + wind.x, wind.w - wind.x, wind.w + wind.y, wind.w - wind.y, wind.z, wind.w - wind.z};
    uint mask = 0;
    lastPlane = 0;
    lastShift = 0;
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        uint side = classifyFrustumPlane(planes[i], shifts[i], center, extent);
        if (!side) return 0xffffffff;
        if (side == 1)
        {
            mask |= 1u << i;
            lastPlane = planes[i];
            lastShift = shifts[i];
        }
    }
    return mask;
}

// Wind ranges from zero to the full offset.
uint classifyFrustum(float3 center, float3 extent, row_major float4x4 toClip, float4 wind)
{
    float4 plane;
    float shift;
    uint mask = crossingPlanes(center, extent, toClip, wind, plane, shift);
    return mask == 0xffffffff ? 0 : mask ? 1 : 2;
}

uint selectPlan(StructuredBuffer<Plan> plans, uint start, uint count, float budget, float pixelScale)
{
    uint low = start;
    uint high = start + (isfinite(pixelScale) ? count : 0);
    float limit = budget / pixelScale;
    while (low < high)
    {
        uint middle = (low + high) >> 1;
        if (plans[middle].error <= limit) low = middle + 1;
        else high = middle;
    }
    return low == start ? start : low - 1;
}
