struct Node {
    float4 sphere;
    float3 center, extent;
    float minimum, maximum;
    uint parent, kind, geometry, instance;
};
struct Geometry { uint2 ranges[3]; uint first, assembly; };
struct Tier { float limit; uint geometry; };
struct Instance { float3 position; float4 rotation; float3 scale; };
struct View {
    row_major float4x4 world, view;
    float4 planes[6], windFocal, viewWindNear;
    float scale; uint visible, frustum, pad;
};
struct Draw { uint indices, instances, firstIndex, baseVertex, firstInstance; };
cbuffer Work : register(b0) {
    uint first, count, nodesPerTree, binsPerTree;
    uint instancesPerTree, tierCount, woodStart, trunkStart;
    uint packStride;
};
StructuredBuffer<Node> nodes : register(t0);
StructuredBuffer<Geometry> geometry : register(t1);
StructuredBuffer<Tier> tiers : register(t2);
StructuredBuffer<Instance> placements : register(t3);
StructuredBuffer<View> views : register(t4);
RWStructuredBuffer<uint> visibility : register(u0);
RWStructuredBuffer<uint> bins : register(u1);
RWStructuredBuffer<Instance> stream : register(u2);
RWStructuredBuffer<Draw> arguments : register(u3);
RWStructuredBuffer<uint> results : register(u4);
StructuredBuffer<uint> sourceIndices : register(t5);
RWStructuredBuffer<uint> packedIndices : register(u5);
RWStructuredBuffer<Draw> packedDraws : register(u6);

[numthreads(64,1,1)]
void Clear(uint3 id : SV_DispatchThreadID)
{
    if (id.x < binsPerTree) bins[id.y*binsPerTree+id.x] = 0;
    if (id.x < 6) results[6*id.y+id.x] = 0;
    if (packStride && id.x<3) {
        Draw draw={0,1,id.x==0?0:id.x==1?woodStart:trunkStart,0,0};
        packedDraws[3*id.y+id.x]=draw;
    }
}

uint frustumState(Node n, View v)
{
    float4 c = float4(mul(float4(n.center,1),v.world).xyz+v.windFocal.xyz,1);
    float3 e = mul(n.extent,abs((float3x3)v.world))+abs(v.windFocal.xyz);
    bool inside = true;
    [unroll] for (uint i=0; i<6; ++i) {
        float4 p = v.planes[i];
        float d = dot(c,p), r = dot(e,abs(p.xyz));
        if (d+r < -1.90734863e-6*(dot(abs(p),abs(c))+r)) return 0;
        inside = inside && d>=r;
    }
    return inside ? 2 : 1;
}

float2 edgeScales(Node n, View v)
{
    float3 c=mul(float4(n.sphere.xyz,1),v.view).xyz;
    float z=-c.z, r=n.sphere.w*v.scale, nearZ=v.viewWindNear.w;
    if (z+r<=nearZ) return 0;
    float x2=dot(c.xy,c.xy), x=sqrt(x2), d=x2+z*z;
    float t=sqrt(max(0,d-r*r));
    float2 cosines=(t*z+float2(-1,1)*r*x)/max(d,1e-18);
    float h=nearZ-z, rim=sqrt(max(0,r*r-h*h));
    float2 sides=float2(x+rim,max(0,x-rim));
    float2 clipped=nearZ/sqrt(sides*sides+nearZ*nearZ);
    float lo=t*cosines.x<nearZ ? clipped.x : cosines.x;
    float hi=r*r-x2>=max(h,0)*max(h,0) ? 1 : t*cosines.y<nearZ ? clipped.y : cosines.y;
    return max(float2(z-r,z+r),nearZ)*float2(lo,hi);
}

[numthreads(64,1,1)]
void Traverse(uint3 id : SV_DispatchThreadID)
{
    if (id.x>=count) return;
    uint index=first+id.x, tree=id.y, base=tree*nodesPerTree;
    Node n=nodes[index];
    View v=views[tree];
    uint state=n.parent==0xffffffff ? (v.visible==0 ? 0 : v.frustum!=0 ? 1 : 2) : visibility[base+n.parent];
    if (state==1) state=frustumState(n,v);
    uint bin=n.geometry;
    if (state!=0) {
        if (n.kind==3) {
            float3 c=mul(float4(n.center,1),v.view).xyz+v.viewWindNear.xyz;
            float3 e=mul(n.extent,abs((float3x3)v.view))+abs(v.viewWindNear.xyz);
            float z=-c.z-e.z;
            float2 xy=abs(c.xy)+e.xy;
            float pixels=z>v.viewWindNear.w ? v.windFocal.w/z*sqrt(1+dot(xy,xy)/(z*z)) : 3.402823e38;
            bin=tiers[tierCount-1].geometry;
            for (uint i=0;i<tierCount;++i)
                if (pixels<=tiers[i].limit*.9999) { bin=tiers[i].geometry; break; }
        } else {
            float2 s=edgeScales(n,v);
            float f=v.windFocal.w;
            if (n.kind<2) {
                if (s.x>f*n.maximum || (n.kind==1 && s.y<f*n.minimum)) state=0;
            } else if (s.x<=f*n.minimum && n.minimum>0) state=0;
        }
    }
    if (n.kind<2) visibility[base+index]=state;
    else if (state!=0) {
        uint offset;
        InterlockedAdd(bins[tree*binsPerTree+bin],1,offset);
        stream[tree*instancesPerTree+geometry[bin].first+offset]=placements[n.instance];
    }
}

void emitDraws(Geometry g, uint tree, uint n)
{
    [unroll] for (uint slot=0;slot<3;++slot) {
        uint2 r=g.ranges[slot];
        if (!r.y) continue;
        uint cmd;
        InterlockedAdd(results[6*tree+slot],1,cmd);
        InterlockedAdd(results[6*tree+3+slot],(r.y/3)*n);
        Draw draw={r.y,n,r.x,0,g.first};
        arguments[(tree*3+slot)*binsPerTree+cmd]=draw;
    }
}

[numthreads(64,1,1)]
void Encode(uint3 id : SV_DispatchThreadID)
{
    if (id.x>=binsPerTree) return;
    uint n=bins[id.y*binsPerTree+id.x];
    if (n) emitDraws(geometry[id.x],id.y,n);
}

// The source indices of an assembly-space cluster already address shared world-space
// vertices. One group gathers each selected cluster, with one indirect draw per material.
groupshared uint destination[3];
[numthreads(32,1,1)]
void Pack(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint bin=group.x+65535*group.z, tree=group.y;
    if (bin>=binsPerTree) return;
    uint n=bins[tree*binsPerTree+bin];
    if (!n) return;
    Geometry g=geometry[bin];
    if (!g.assembly || n!=1) {
        if (lane==0) emitDraws(g,tree,n);
        return;
    }
    if (lane<3) {
        uint2 r=g.ranges[lane];
        if (r.y) {
            uint offset;
            InterlockedAdd(packedDraws[3*tree+lane].indices,r.y,offset);
            destination[lane]=tree*packStride+(lane==0?0:lane==1?woodStart:trunkStart)+offset;
            InterlockedAdd(results[6*tree+3+lane],r.y/3);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint slot=0;slot<3;++slot) {
        uint2 r=g.ranges[slot];
        for (uint k=lane;k<r.y;k+=32) packedIndices[destination[slot]+k]=sourceIndices[r.x+k];
    }
}
