#include "Skylight.hlsli"

// Matches CaptureLighting in ViewerScene.h.
static const uint sunOnly = 1;
static const uint skyOnly = 2;
static const uint transmissionOnly = 3;
static const uint shadowVisibility = 4;

struct LeafIncidentLight
{
    float3 skyFront;
    float3 skyBack;
    float2 sunCosines;
};

LeafIncidentLight leafIncidentLight(float3 visibleNormal, float sunCosine, bool directionalSky)
{
    LeafIncidentLight light;
    light.skyFront = directionalSky ? evaluateSkylight(visibleNormal) : 0.35;
    light.skyBack = directionalSky ? evaluateSkylight(-visibleNormal) : 0.35;
    light.sunCosines = saturate(float2(sunCosine, -sunCosine));
    return light;
}

float3 shadeLeaf(LeafIncidentLight light, float visibility, uint mode, float3 baseColor,
                 float3 transmissionColor, float transmission)
{
    // Each side can return at most its share of the incoming diffuse energy.
    const float fraction = saturate(transmission);
    const float3 reflectance = saturate(baseColor) * (1.0 - fraction);
    const float3 transmittance = saturate(transmissionColor) * fraction;
    const float2 sun = mode == skyOnly ? 0.0 : (0.65 * visibility) * light.sunCosines;
    const float3 back = sun.y + (mode == sunOnly ? 0.0 : light.skyBack);
    const float3 throughLeaf = back * transmittance;
    if (mode == transmissionOnly) return throughLeaf;
    const float3 front = sun.x + (mode == sunOnly ? 0.0 : light.skyFront);
    return front * reflectance + throughLeaf;
}
