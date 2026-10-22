// UI overlay: target-pixel quads written into the UI image, which the
// composite blends over the tonemapped frame.
// Entries: uiVertex, uiFragment.

struct UiPush
{
    float2 viewport;
    float2 pad;
};

[[vk::push_constant]] UiPush push;

struct VertexInput
{
    float2 position : POSITION;
    float4 color : COLOR0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput uiVertex(VertexInput input)
{
    // Top-left pixel space. NDC y = -1 is the framebuffer's top row.
    float2 ndc = float2(input.position.x / push.viewport.x * 2.0 - 1.0,
                        input.position.y / push.viewport.y * 2.0 - 1.0);
    VertexOutput output;
    output.position = float4(ndc, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 uiFragment(VertexOutput input) : SV_Target
{
    return input.color;
}
