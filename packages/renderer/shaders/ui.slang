// UI overlay: target-pixel quads written into the UI image, which the
// composite blends over the tonemapped frame. Solid quads sample the white
// texel, images the built-in checker and text the font atlas.
// Entries: uiVertex, uiFragment.

struct UiPush
{
    float2 viewport;
    float2 pad;
    uint texture_slot;
    uint font_slot;
};

[[vk::push_constant]] UiPush push;

[[vk::binding(5, 0)]] Sampler2D image_table[16];

struct VertexInput
{
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
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
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 uiFragment(VertexOutput input) : SV_Target
{
    if (push.texture_slot == 0xFFFFFFFFu)
    {
        return input.color;
    }
    float4 sampled = image_table[push.texture_slot].Sample(input.uv);
    if (push.texture_slot == push.font_slot)
    {
        // The font atlas is a signed distance field; rebuild the edge from it.
        float w = max(fwidth(sampled.a), 0.001);
        float coverage = smoothstep(0.5 - w, 0.5 + w, sampled.a);
        return float4(input.color.rgb, input.color.a * coverage);
    }
    return input.color * sampled;
}
