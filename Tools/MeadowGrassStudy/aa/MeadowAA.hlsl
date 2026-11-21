Texture2D<float4> current : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float4> history : register(t2);
Texture2DMS<float4, 4> multisampled : register(t3);

cbuffer Resolve : register(b0)
{
    row_major float4x4 inverseCurrent, previousClip;
    float2 windDelta;
    float historyValid;
    float exposure;
    float2 jitterDelta;
    float stationary;
};

float4 Weighted(float4 position : SV_Position) : SV_Target
{
    float3 sum = 0;
    float weights = 0;
    [unroll] for (uint s = 0; s < 4; ++s)
    {
        float3 rgb = multisampled.Load(int2(position.xy), s).rgb;
        // Reversible luminance compression. Identical samples retain their radiance.
        float weight = rcp(1 + exposure * dot(max(rgb, 0), float3(.2126,.7152,.0722)));
        sum += weight * rgb;
        weights += weight;
    }
    return float4(sum / weights, 1);
}

struct SeedOutput { float4 rgb : SV_Target; float z : SV_Depth; };
SeedOutput Seed(float4 position : SV_Position)
{
    int3 p = int3(position.xy, 0);
    SeedOutput o;
    o.rgb = current.Load(p);
    o.z = depth.Load(p);
    return o;
}

float4 Temporal(float4 position : SV_Position) : SV_Target
{
    uint w, h;
    current.GetDimensions(w,h);
    int2 p = int2(position.xy), limit = int2(w,h)-1;
    float4 now = current.Load(int3(p,0));
    float z = depth.Load(int3(p,0));
    float2 ndc = position.xy / float2(w,h) * float2(2,-2) + float2(-1,1);
    float4 world = mul(float4(ndc,z,1), inverseCurrent);
    float viewDepth = rcp(world.w);
    world *= viewDepth;
    // Alpha contains the mesh vertex shader's wind displacement weight.
    bool card = now.a < 0;
    float windWeight = card ? -1-now.a : now.a;
    if (z < 1) world.xz += windWeight * windDelta;
    float4 prev = mul(world, previousClip);
    float2 texel = (prev.xy / prev.w * float2(.5,-.5)+.5) * float2(w,h) - .5;
    // History is accumulated on the display grid. Remove the sample jitter from motion.
    texel += jitterDelta;
    float4 result = float4(now.rgb, card ? -viewDepth : viewDepth);
    if (!historyValid || prev.w <= 0 || any(texel < 0) || any(texel > limit)) return result;

    int2 base = int2(texel);
    float2 f = frac(texel);
    float3 sum = 0;
    float accepted = 0;
    [unroll] for (uint corner = 0; corner < 4; ++corner)
    {
        int2 offset = int2(corner & 1,corner >> 1);
        float2 weight = lerp(1-f,f,float2(offset));
        float4 old = history.Load(int3(min(base+offset,limit),0));
        float contribution = weight.x * weight.y;
        float separation = abs(abs(old.a) - prev.w);
        bool fineGrass = card || old.a < 0;
        // Cutouts alternately expose two depths even when the camera is perfectly still.
        // For motion, allow that alternation only within a local layer of grass.
        if (!stationary && separation > max(.06,.035 * prev.w) &&
            !(fineGrass && separation < max(.45,.12*prev.w) && length(texel-p)<3)) continue;
        sum += contribution * old.rgb;
        accepted += contribution;
    }
    if (accepted < .05) return result;
    float3 lo = now.rgb, hi = now.rgb;
    [unroll] for (int y=-1;y<=1;++y)
        [unroll] for (int x=-1;x<=1;++x)
        {
            float3 c = current.Load(int3(clamp(p+int2(x,y),0,limit),0)).rgb;
            lo = min(lo,c);
            hi = max(hi,c);
        }
    float3 prior = clamp(sum / accepted,lo,hi);
    float weight = .9 * saturate(accepted * 2);
    result.rgb = lerp(now.rgb,prior,weight);
    return result;
}
