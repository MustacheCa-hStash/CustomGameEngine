#include "TreeCull.hlsli"
#include "TreePyramid.hlsli"

struct Part
{
    float4 sprayCenter, sprayExtent;
    uint nodeFirst, nodeCount, placementFirst, placementCount;
    uint planFirst, localPlans, wholePlans, firstBin;
};

struct Pose
{
    float4 placement;
    row_major float4x4 toWorld, toView, toClip;
    float4 wind, viewWind, clipWind;
};

struct Job
{
    uint part, pose, first, count, bin;
    // A proxy bin uses count as its frustum-crossing flag.
};

struct Record
{
    uint pose, placement, bin, rank;
};

struct ClusterNode
{
    float3 center, extent;
    uint next, bin;
};

cbuffer View : register(b0)
{
    row_major float4x4 worldToClip;
    float4 projection; // budget, focal pixels, near plane, half shadow-map resolution
    uint4 sizes; // bins, parts, pose count, record capacity
    uint4 options; // flags, enabled part mask, unused
    uint4 instanceAddress;
    row_major float4x4 previousClip;
    float4 previousWind;
    uint4 viewport; // width and height in render pixels
};

cbuffer OcclusionPhase : register(b1) { uint recovery; }

StructuredBuffer<Part> parts : register(t0);
StructuredBuffer<Node> nodes : register(t1);
StructuredBuffer<Placement> placements : register(t2);
StructuredBuffer<Plan> plans : register(t3);
StructuredBuffer<uint> planBins : register(t4);
ByteAddressBuffer templates : register(t5);
StructuredBuffer<Pose> poses : register(t6);
StructuredBuffer<ClusterNode> clusterNodes : register(t7);
ByteAddressBuffer pyramid : register(t8);

RWByteAddressBuffer state : register(u0);
RWByteAddressBuffer dispatchArgs : register(u1);
RWStructuredBuffer<Job> jobs : register(u2);
RWStructuredBuffer<Record> records : register(u3);
RWByteAddressBuffer commands : register(u4);
RWByteAddressBuffer instances : register(u5);
RWStructuredBuffer<uint> deferred : register(u6);

static const uint invalid = 0xffffffff;
static const uint selectThreads = 64;
static const uint scatterThreads = 128;
static const uint encodeThreads = 256;
static const uint deferredCount = 40;
static const uint depthTestCount = 44;
static const uint depthRejectCount = 48;
static const uint depthRecoverCount = 52;
static const uint candidateCount = 56;
static const uint stateHeader = 64;
static const uint commandStride = 104;
static const uint templateStride = 120;
static const uint sparseBins = 16;
static const uint historyOcclusion = 32;

uint binCount(uint bin) { return stateHeader + bin * 4; }
uint binOffset(uint bin) { return stateHeader + (sizes.x + bin) * 4; }
uint drawCount(uint batch) { return stateHeader + (2 * sizes.x + batch) * 4; }
uint nodeCount(uint part) { return stateHeader + (3 * sizes.x + part) * 4; }
uint visibleSprayCount(uint part) { return stateHeader + (3 * sizes.x + sizes.y + part) * 4; }
uint parentCount(uint part) { return stateHeader + (3 * sizes.x + 2 * sizes.y + part) * 4; }
uint parentSprayCount(uint part) { return stateHeader + (3 * sizes.x + 3 * sizes.y + part) * 4; }
uint activeBin(uint index) { return stateHeader + (3 * sizes.x + 4 * sizes.y + index) * 4; }

uint reserveBin(uint bin)
{
    uint rank;
    state.InterlockedAdd(binCount(bin), 1, rank);
    if (!rank && (options.x & sparseBins))
    {
        uint index;
        state.InterlockedAdd(16, 1, index);
        state.Store(activeBin(index), bin);
    }
    return rank;
}

uint initialRank(uint bin)
{
    // The history pass bins the visible cut after it decides what to defer.
    if (options.x & historyOcclusion) return 0;
    return reserveBin(bin);
}

void queueClusterTests(uint endJob)
{
    commands.InterlockedMax(0, (endJob + scatterThreads - 1) / scatterThreads);
}

uint crossesNode(Node node, Pose pose)
{
    if (pose.wind.w == 0) return 0;
    return classifyFrustum(node.center, node.extent, pose.toClip, pose.clipWind) == 1 ? 1 : 0;
}

groupshared uint frontier[2048];
groupshared uint frontierEnd;

void emitJob(uint part, uint pose, uint first, uint count, uint bin)
{
    Job job = {part, pose, first, count, bin};
    uint index;
    dispatchArgs.InterlockedAdd(0, 1, index);
    jobs[index] = job;
}

// A coarse group takes one tree part. The terminal ranges become independent fine jobs.
[numthreads(64, 1, 1)]
void Traverse(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint partId = group.y;
    if (!(options.y & (1u << partId))) return;
    Part part = parts[partId];
    Pose pose = poses[group.x];
    if (lane == 0)
    {
        frontier[0] = 0;
        frontierEnd = 1;
    }
    GroupMemoryBarrierWithGroupSync();
    uint first = 0, last = 1;
    const bool shadow = (options.x & 2) != 0;
    const float shadowScale = shadow ? shadowTexelsPerUnit(pose.toClip, projection.w) : 0;
    while (first < last)
    {
        for (uint item = first + lane; item < last; item += 64)
        {
            const uint id = frontier[item];
            Node node = nodes[part.nodeFirst + id];
            if (options.x & 1)
            {
                float3 center = mul(float4(node.center, 1), pose.toWorld).xyz + pose.wind.xyz;
                float3 extent = halfExtent(node.extent, pose.toWorld) + abs(pose.wind.xyz);
                if (!intersectsFrustum(center, extent, worldToClip, 0)) continue;
            }
            if (node.indexCount)
            {
                float scale = shadow ? shadowScale
                    : pixelsPerUnit(node.center, node.extent, pose.toView, pose.viewWind.xyz, projection.yz);
                if (node.error == 0 || node.error * scale <= projection.x * 0.9999)
                {
                    if (options.x & sparseBins)
                    {
                        state.InterlockedAdd(parentCount(partId), 1);
                        state.InterlockedAdd(parentSprayCount(partId), node.placements);
                    }
                    emitJob(partId, group.x, 0, crossesNode(node, pose), part.firstBin + id);
                    continue;
                }
            }
            if (node.childCount)
            {
                uint destination;
                InterlockedAdd(frontierEnd, node.childCount, destination);
                for (uint child = 0; child < node.childCount; ++child)
                    frontier[destination + child] = node.firstChild + child;
            }
            else if (part.placementCount)
            {
                for (uint start = 0; start < node.placements; start += selectThreads)
                    emitJob(partId, group.x, node.firstPlacement + start,
                            min(selectThreads, node.placements - start), invalid);
            }
            else
                emitJob(partId, group.x, 0, crossesNode(node, pose), part.firstBin + part.nodeCount);
        }
        GroupMemoryBarrierWithGroupSync();
        first = last;
        last = frontierEnd;
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) state.InterlockedAdd(nodeCount(partId), last);
}

groupshared uint numRecords, recordBase, visibleSprays, pendingClusters, pendingBase;

[numthreads(selectThreads, 1, 1)]
void Select(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    Job job = jobs[group.x];
    if (job.bin != invalid)
    {
        if (lane == 0)
        {
            uint destination;
            state.InterlockedAdd(4, 1, destination);
            uint rank = invalid;
            if (job.count)
            {
                uint pending;
                state.InterlockedAdd(12, 1, pending);
                instances.Store(pending * 4, destination);
                queueClusterTests(pending + 1);
            }
            else rank = initialRank(job.bin);
            Record record = {job.pose, invalid, job.bin, rank};
            records[destination] = record;
        }
        return;
    }
    if (lane == 0)
    {
        numRecords = 0;
        visibleSprays = 0;
        pendingClusters = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    Part part = parts[job.part];
    uint source = part.placementFirst + job.first + lane;
    Plan plan = (Plan)0;
    bool clip = false;
    if (lane < job.count)
    {
        Placement spray = placements[source];
        Pose pose = poses[job.pose];
        row_major float4x4 toClip = mul(spray.toAsset, pose.toClip);
        if (!(options.x & 1) || intersectsFrustum(part.sprayCenter.xyz, part.sprayExtent.xyz, toClip, pose.clipWind))
        {
            float scale = options.x & 2 ? shadowTexelsPerUnit(toClip, projection.w)
                : pixelsPerUnit(part.sprayCenter.xyz, part.sprayExtent.xyz, mul(spray.toAsset, pose.toView),
                                pose.viewWind.xyz, projection.yz);
            const bool local = (options.x & 4) && part.localPlans;
            const uint begin = part.planFirst + (local ? 0 : part.localPlans);
            const uint count = local ? part.localPlans : part.wholePlans;
            plan = plans[selectPlan(plans, begin, count, projection.x, scale)];
            if (pose.wind.w != 0)
                clip = classifyFrustum(part.sprayCenter.xyz, part.sprayExtent.xyz, toClip, pose.clipWind) == 1;
        }
    }
    uint offset = 0, clipOffset = 0;
    if (plan.count)
    {
        InterlockedAdd(numRecords, plan.count, offset);
        InterlockedAdd(visibleSprays, 1);
        if (clip) InterlockedAdd(pendingClusters, plan.count, clipOffset);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0 && numRecords)
    {
        state.InterlockedAdd(4, numRecords, recordBase);
        state.InterlockedAdd(visibleSprayCount(job.part), visibleSprays);
        if (pendingClusters)
        {
            state.InterlockedAdd(12, pendingClusters, pendingBase);
            queueClusterTests(pendingBase + pendingClusters);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = 0; i < plan.count; ++i)
    {
        uint bin = part.firstBin + part.nodeCount + planBins[plan.first + i];
        uint rank = invalid;
        if (clip) instances.Store((pendingBase + clipOffset + i) * 4, recordBase + offset + i);
        else rank = initialRank(bin);
        Record record = {job.pose, source, bin, rank};
        records[recordBase + offset + i] = record;
    }
}

bool reserveExtraRecords(uint count, uint capacity, out uint offset)
{
    offset = 0;
    if (!count) return true;
    if (state.Load(36) >= capacity) return false;
    state.InterlockedAdd(36, count, offset);
    if (offset >= capacity || count > capacity - offset) return false;
    state.InterlockedMax(8, offset + count);
    return true;
}

[numthreads(scatterThreads, 1, 1)]
void Clip(uint job : SV_DispatchThreadID)
{
    if (job >= state.Load(12)) return;
    // Select uses the unwritten instance arena as a compact list of boundary records.
    uint index = instances.Load(job * 4);
    uint baseRecords = state.Load(4);
    Record record = records[index];
    uint root = templates.Load(record.bin * templateStride + 112);
    if (root == invalid)
    {
        record.rank = initialRank(record.bin);
        records[index] = record;
        return;
    }
    Pose pose = poses[record.pose];
    row_major float4x4 projectionMatrix = pose.toClip;
    if (record.placement != invalid) projectionMatrix = mul(placements[record.placement].toAsset, pose.toClip);
    ClusterNode rootBounds = clusterNodes[root];
    uint end = rootBounds.next;
    float4 borderPlane;
    float borderShift;
    uint mask = crossingPlanes(rootBounds.center, rootBounds.extent, projectionMatrix, pose.clipWind, borderPlane, borderShift);
    if (mask == invalid)
    {
        state.InterlockedAdd(20, 1);
        state.InterlockedAdd(24, 1);
        record.bin = invalid;
        records[index] = record;
        return;
    }
    if (!mask || end == root + 1)
    {
        state.InterlockedAdd(20, 1);
        record.rank = initialRank(record.bin);
        records[index] = record;
        return;
    }
    const bool singleBorder = countbits(mask) == 1;
    uint tests = 1, rejected = 0, accepted = 0;
    for (uint i = root + 1; i < end;)
    {
        ClusterNode cluster = clusterNodes[i];
        uint visibility;
        [branch]
        if (singleBorder) visibility = classifyFrustumPlane(borderPlane, borderShift, cluster.center, cluster.extent);
        else visibility = classifyFrustum(cluster.center, cluster.extent, projectionMatrix, pose.clipWind);
        ++tests;
        if (!visibility) ++rejected;
        else if (visibility == 2 || cluster.next == i + 1) ++accepted;
        i = visibility == 1 && cluster.next != i + 1 ? i + 1 : cluster.next;
    }
    state.InterlockedAdd(20, tests);
    if (!accepted)
    {
        state.InterlockedAdd(24, rejected);
        record.bin = invalid;
        records[index] = record;
        return;
    }
    uint extra = 0;
    bool split = false;
    if (rejected) split = reserveExtraRecords(accepted - 1, sizes.w - baseRecords, extra);
    if (!split)
    {
        if (rejected) state.InterlockedAdd(32, 1);
        record.rank = initialRank(record.bin);
        records[index] = record;
        return;
    }

    state.InterlockedAdd(24, rejected);
    state.InterlockedAdd(28, 1);
    uint destination = index;
    for (uint i = root + 1; i < end;)
    {
        ClusterNode cluster = clusterNodes[i];
        uint visibility;
        [branch]
        if (singleBorder) visibility = classifyFrustumPlane(borderPlane, borderShift, cluster.center, cluster.extent);
        else visibility = classifyFrustum(cluster.center, cluster.extent, projectionMatrix, pose.clipWind);
        if (visibility && (visibility == 2 || cluster.next == i + 1))
        {
            record.bin = cluster.bin;
            record.rank = initialRank(record.bin);
            records[destination] = record;
            destination = baseRecords + extra++;
        }
        i = visibility == 1 && cluster.next != i + 1 ? i + 1 : cluster.next;
    }
}

// Project the entire swept bounds. Fail open at the near plane or viewport edge.
bool depthOccluded(ClusterNode bounds, row_major float4x4 toClip, float4 clipWind)
{
    const float4 center = mul(float4(bounds.center, 1), toClip);
    const float4 dx = bounds.extent.x * toClip[0];
    const float4 dy = bounds.extent.y * toClip[1];
    const float4 dz = bounds.extent.z * toClip[2];
    float2 lower = 1, upper = -1;
    float nearest = 1;
    [unroll]
    for (uint corner = 0; corner < 8; ++corner)
    {
        const float4 start = center + (corner & 1 ? dx : -dx) + (corner & 2 ? dy : -dy) + (corner & 4 ? dz : -dz);
        const float4 end = start + clipWind;
        if (min(start.w, end.w) <= 0 || min(start.z, end.z) <= 0) return false;
        const float3 a = start.xyz / start.w, b = end.xyz / end.w;
        lower = min(lower, min(a.xy, b.xy));
        upper = max(upper, max(a.xy, b.xy));
        nearest = min(nearest, min(a.z, b.z));
    }
    const float2 begin = (float2(lower.x, -upper.y) * 0.5 + 0.5) * viewport.xy - 1;
    const float2 end = (float2(upper.x, -lower.y) * 0.5 + 0.5) * viewport.xy + 1;
    if (any(begin < 0) || any(end >= viewport.xy)) return false;

    for (uint level = 0; level < pyramidLevels; ++level)
    {
        const uint shift = 2 * (level + 1);
        const uint2 first = uint2(begin) >> shift, last = uint2(end) >> shift;
        if (any(last - first > 3)) continue;
        const uint stride = pyramidSize(viewport.xy, level).x;
        const uint offset = pyramidOffset(viewport.xy, level);
        for (uint y = first.y; y <= last.y; ++y)
            for (uint x = first.x; x <= last.x; ++x)
                if (asfloat(pyramid.Load(4 * (offset + y * stride + x))) + 0.000004 >= nearest)
                    return false;
        return true;
    }
    return false;
}

[numthreads(1, 1, 1)]
void PrepareOcclusion(uint lane : SV_GroupIndex)
{
    uint count = state.Load(deferredCount);
    if (!recovery)
    {
        count = state.Load(4) + state.Load(8);
        state.Store(candidateCount, count);
    }
    dispatchArgs.Store(12, (count + scatterThreads - 1) / scatterThreads);
}

void routeRecord(uint index, Record record, bool hidden)
{
    if (hidden)
    {
        uint slot;
        state.InterlockedAdd(deferredCount, 1, slot);
        deferred[slot] = index;
        record.rank = invalid;
    }
    else record.rank = reserveBin(record.bin);
    records[index] = record;
}

[numthreads(scatterThreads, 1, 1)]
void CullHistory(uint index : SV_DispatchThreadID)
{
    if (index >= state.Load(candidateCount)) return;
    Record record = records[index];
    if (record.bin == invalid) return;
    const uint root = templates.Load(record.bin * templateStride + 112);
    if (root == invalid)
    {
        routeRecord(index, record, false);
        return;
    }
    const Pose pose = poses[record.pose];
    row_major float4x4 toClip = mul(pose.toWorld, previousClip);
    if (record.placement != invalid) toClip = mul(placements[record.placement].toAsset, toClip);
    const float4 clipWind = mul(float4(previousWind.xyz * pose.placement.w, 0), previousClip);
    const ClusterNode bounds = clusterNodes[root];
    if (depthOccluded(bounds, toClip, clipWind))
    {
        state.InterlockedAdd(depthTestCount, 1);
        routeRecord(index, record, true);
        return;
    }

    // Split a selected range only when some children are likely hidden. If the
    // partition will not fit, keep the original range in the early draw.
    uint mask = 0, children = 0;
    for (uint child = root + 1; child < bounds.next;)
    {
        const ClusterNode cluster = clusterNodes[child];
        if (depthOccluded(cluster, toClip, clipWind)) mask |= 1u << children;
        ++children;
        child = cluster.next;
    }
    state.InterlockedAdd(depthTestCount, 1 + children);
    if (!mask || countbits(mask) == children)
    {
        routeRecord(index, record, mask != 0);
        return;
    }
    const uint baseRecords = state.Load(4);
    uint extra;
    if (!reserveExtraRecords(children - 1, sizes.w - baseRecords, extra))
    {
        routeRecord(index, record, false);
        return;
    }
    uint destination = index, bit = 0;
    for (uint child = root + 1; child < bounds.next;)
    {
        const ClusterNode cluster = clusterNodes[child];
        record.bin = cluster.bin;
        routeRecord(destination, record, (mask & (1u << bit)) != 0);
        destination = baseRecords + extra++;
        ++bit;
        child = cluster.next;
    }
}

[numthreads(scatterThreads, 1, 1)]
void RetestOcclusion(uint item : SV_DispatchThreadID)
{
    if (item >= state.Load(deferredCount)) return;
    const uint index = deferred[item];
    Record record = records[index];
    const uint root = templates.Load(record.bin * templateStride + 112);
    const Pose pose = poses[record.pose];
    row_major float4x4 toClip = pose.toClip;
    if (record.placement != invalid) toClip = mul(placements[record.placement].toAsset, toClip);
    bool hidden = depthOccluded(clusterNodes[root], toClip, pose.clipWind);
    if (!hidden)
    {
        record.rank = reserveBin(record.bin);
        records[index] = record;
    }
    state.InterlockedAdd(hidden ? depthRejectCount : depthRecoverCount, 1);
}

uint2 bufferAddress(uint byteOffset)
{
    const uint low = instanceAddress.x + byteOffset;
    return uint2(low, instanceAddress.y + (low < instanceAddress.x ? 1 : 0));
}

groupshared uint prefix[encodeThreads];

// Bin counts are also the per-bin instance counts used by the indirect draws.
[numthreads(encodeThreads, 1, 1)]
void Encode(uint lane : SV_GroupIndex)
{
    if (lane == 0)
    {
        if (!recovery) state.Store(0, dispatchArgs.Load(0));
        const uint count = recovery ? state.Load(deferredCount) : state.Load(4) + state.Load(8);
        dispatchArgs.Store(12, (count + scatterThreads - 1) / scatterThreads);
    }
    const bool clustered = (options.x & sparseBins) != 0;
    const uint countBins = clustered ? state.Load(16) : sizes.x;
    uint stride = (countBins + encodeThreads - 1) / encodeThreads;
    uint begin = stride * lane, end = min(countBins, begin + stride);
    uint total = 0;
    for (uint index = begin; index < end; ++index)
        total += state.Load(binCount(clustered ? state.Load(activeBin(index)) : index));
    prefix[lane] = total;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1; step < encodeThreads; step <<= 1)
    {
        uint prior = lane >= step ? prefix[lane - step] : 0;
        GroupMemoryBarrierWithGroupSync();
        prefix[lane] += prior;
        GroupMemoryBarrierWithGroupSync();
    }
    uint start = prefix[lane] - total;
    for (uint index = begin; index < end; ++index)
    {
        uint bin = clustered ? state.Load(activeBin(index)) : index;
        uint count = state.Load(binCount(bin));
        if (!count) continue;
        state.Store(binOffset(bin), start);
        uint base = bin * templateStride;
        uint2 batch = templates.Load2(base + commandStride);
        uint command;
        state.InterlockedAdd(drawCount(batch.x), 1, command);
        uint destination = (batch.y + command) * commandStride;
        commands.Store4(destination, templates.Load4(base));
        commands.Store4(destination + 16, uint4(bufferAddress(sizes.w * 16 + start * 40), count * 40, 40));
        commands.Store4(destination + 32, templates.Load4(base + 32));
        commands.Store4(destination + 48, uint4(start, templates.Load(base + 52), bufferAddress(sizes.w * 56)));
        commands.Store4(destination + 64, uint4(bufferAddress(start * 16), count * 16, 16));
        uint4 draw = templates.Load4(base + 80);
        draw.y = count;
        commands.Store4(destination + 80, draw);
        commands.Store2(destination + 96, 0);
        start += count;
    }
}

[numthreads(scatterThreads, 1, 1)]
void Scatter(uint index : SV_DispatchThreadID)
{
    const uint count = recovery ? state.Load(deferredCount) : state.Load(4) + state.Load(8);
    if (index >= count) return;
    Record record = records[recovery ? deferred[index] : index];
    if (record.bin == invalid || record.rank == invalid) return;
    uint target = state.Load(binOffset(record.bin)) + record.rank;
    instances.Store4(target * 16, asuint(poses[record.pose].placement));
    uint offset = sizes.w * 16 + target * 40;
    if (record.placement != invalid)
    {
        Placement spray = placements[record.placement];
        instances.Store3(offset, asuint(spray.translation));
        instances.Store4(offset + 12, asuint(spray.rotation));
        instances.Store3(offset + 28, asuint(spray.scale));
        if (options.x & 8) instances.Store3(sizes.w * 56 + target * 12, spray.tag);
    }
    else
    {
        instances.Store3(offset, 0);
        instances.Store4(offset + 12, asuint(float4(0, 0, 0, 1)));
        instances.Store3(offset + 28, asuint(float3(1, 1, 1)));
    }
}
