// Local and unobstructed irradiance are hemispherical averages. Their ratio
// estimates sky access, not a literal count of the leaves above this point.
float skyAccess(float3 localSky, float3 openSky)
{
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    return saturate(dot(localSky, luma) / max(dot(openSky, luma), 0.0001));
}

float3 studyLeaf(LeafIncidentLight light, LeafIncidentLight openLight, float visibility,
                 float3 base, float3 tint, float fraction, float3 n, float3 v, float3 sun, uint mode)
{
    // Increase effective optical depth for indirect light in a covered hemisphere.
    // Retain some bounce even in dense shade. Open-sky leaves keep their ambient.
    light.skyFront *= lerp(0.65, 1.0, skyAccess(light.skyFront, openLight.skyFront));
    light.skyBack *= lerp(0.45, 1.0, skyAccess(light.skyBack, openLight.skyBack));
    float3 result = shadeLeaf(light, visibility, mode, base, tint, fraction);
    if (mode == skyOnly) return result;

    // Redistribute direct transmission toward the incident ray. This phase
    // term has a unit spherical mean; only directly lit backfaces contribute.
    const float g = 0.50;
    const float phase = (1.0 - g * g) * pow(max(0.01, 1.0 + g * g - 2.0 * g * dot(-v, sun)), -1.5);
    const float direct = 0.65 * visibility;
    result += direct * light.sunCosines.y * saturate(tint) * saturate(fraction) *
              (lerp(1.0, phase, 0.80) - 1.0);
    if (mode == transmissionOnly) return result;

    // Broad dielectric highlight on the sun-facing side of the leaf cuticle.
    const float3 h = v + sun;
    const float3 halfway = h * rsqrt(max(dot(h,h), 0.0001));
    const float fresnel = 0.035 + 0.965 * pow(1.0 - saturate(dot(v, halfway)), 5.0);
    return result + direct * light.sunCosines.x * fresnel *
                    (18.0 / 6.2831853) * pow(saturate(dot(n, halfway)), 16.0);
}

float3 studyTwig(float3 base, float3 ambient, float direct, float normalToSun, float visibility)
{
    // The textured bark still catches direct light. Reduce cool diffuse fill
    // on the unlit side of thin wood, which should silhouette against a lit leaf.
    const float fill = lerp(0.38, 0.85, visibility * saturate(normalToSun));
    return base * float3(0.77, 0.69, 0.61) * (ambient * fill + direct);
}
