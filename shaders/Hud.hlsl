cbuffer Viewport : register(b0)
{
    float2 inverseSize;
};

struct PixelInput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

PixelInput VSMain(float4 rect : RECT, float4 color : COLOR, uint vertex : SV_VertexID)
{
    float2 corner = float2(vertex & 1, vertex >> 1);
    float2 screen = rect.xy + corner * rect.zw;
    PixelInput output;
    output.position = float4(screen * inverseSize * float2(2, -2) + float2(-1, 1), 0, 1);
    output.color = color;
    return output;
}

float4 PSMain(PixelInput input) : SV_Target
{
    return input.color;
}
