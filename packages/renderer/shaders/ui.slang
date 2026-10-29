// UI overlay: target-pixel quads written into the UI image, which the
// composite blends over the tonemapped frame. Solid quads sample the white
// texel, images the built-in checker, text the font atlas, and the tgfx text
// layer one fullscreen premultiplied quad.
// Entries: uiVertex, uiFragment.

struct UiPush
{
    float2 viewport;
    float2 pad;
    uint texture_slot;
    uint font_slot;
    uint overlay_slot;
    uint pad1;
};

[[vk::push_constant]] UiPush push;

[[vk::binding(5, 0)]] Sampler2D image_table[16];

struct VertexInput
{
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float4 shape : TEXCOORD1; // x: width, y: height, z: radius, w: border
};

struct VertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float4 shape : TEXCOORD1;
};

// Distance to a box of half extents `half_size` with rounded corners, centred
// on the origin; negative inside.
float roundedBoxDistance(float2 p, float2 half_size, float radius)
{
    float2 q = abs(p) - half_size + radius;
    return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

// Coverage of a solid quad. The local position comes from the interpolated uv:
// a solid quad samples no texture, so it is the normalized rect coordinate.
// A border keeps only the ring between the edge and the rect inset by its
// width, so a border pixel never blends twice. The ramp is one pixel wide
// because the rect is axis-aligned and p is already in target pixels.
float quadCoverage(float2 uv, float4 shape)
{
    if (shape.x <= 0.0 || shape.y <= 0.0)
    {
        return 1.0;
    }
    float2 half_size = shape.xy * 0.5;
    float2 p = (uv - 0.5) * shape.xy;
    float radius = clamp(shape.z, 0.0, min(half_size.x, half_size.y));
    float coverage = saturate(0.5 - roundedBoxDistance(p, half_size, radius));
    if (shape.w > 0.0)
    {
        float border = min(shape.w, min(half_size.x, half_size.y));
        float inner_radius = max(radius - border, 0.0);
        coverage *= saturate(0.5 + roundedBoxDistance(p, half_size - border, inner_radius));
    }
    return coverage;
}

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
    output.shape = input.shape;
    return output;
}

[shader("fragment")]
float4 uiFragment(VertexOutput input) : SV_Target
{
    if (push.texture_slot == 0xFFFFFFFFu)
    {
        return float4(input.color.rgb, input.color.a * quadCoverage(input.uv, input.shape));
    }
    float4 sampled = image_table[push.texture_slot].Sample(input.uv);
    if (push.texture_slot == push.overlay_slot)
    {
        // tgfx writes premultiplied color; the UI target accumulates straight
        // alpha, so undo the premultiply before this quad blends.
        float3 straight = sampled.a > 0.0001 ? sampled.rgb / sampled.a : float3(0.0, 0.0, 0.0);
        return float4(straight, sampled.a);
    }
    if (push.texture_slot == push.font_slot)
    {
        // The font atlas is a signed distance field; rebuild the edge from it.
        float w = max(fwidth(sampled.a), 0.001);
        float coverage = smoothstep(0.5 - w, 0.5 + w, sampled.a);
        return float4(input.color.rgb, input.color.a * coverage);
    }
    return input.color * sampled;
}
