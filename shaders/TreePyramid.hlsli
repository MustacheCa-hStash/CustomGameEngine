// Four levels of 4x4 reductions. Level zero stores the farthest sample in 4x4 pixels.
static const uint pyramidLevels = 4;

uint2 pyramidSize(uint2 viewportSize, uint level)
{
    const uint shift = 2 * (level + 1);
    return (viewportSize + (1u << shift) - 1) >> shift;
}

uint pyramidOffset(uint2 viewportSize, uint level)
{
    uint offset = 0;
    for (uint i = 0; i < level; ++i)
    {
        const uint2 size = pyramidSize(viewportSize, i);
        offset += size.x * size.y;
    }
    return offset;
}
