// The post chain's stages: bright extraction, separable blur, tonemap, FXAA
// over the scene only, and the UI blend. All draw a full-screen triangle from
// the vertex id and read their inputs through the sampled-image table.
// Entries: postVertex, brightFragment, blurFragment, tonemapFragment,
// fxaaFragment, blendFragment.

struct PostPush
{
    uint4 slots;   // x: input image, y: second input (bloom for the composite)
    float4 params; // bright: threshold in x; blur: texel step in x/y;
                   // composite: x is 1 when the pass applies the sRGB encode
};

[[vk::binding(5, 0)]] Sampler2D image_table[16];

[[vk::push_constant]] PostPush push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv;
};

struct PostVertexInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput postVertex(PostVertexInput input)
{
    // A triangle larger than the clip volume covers the frame; UVs follow.
    // NDC y = -1 is the framebuffer's top row, where v = 0 samples.
    VertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

[shader("fragment")]
float4 brightFragment(VertexOutput input) : SV_Target
{
    // The threshold is in HDR units: what exceeds it contributes by its excess.
    float2 uv = input.uv * push.params.yw;
    float3 color = image_table[push.slots.x].Sample(uv).rgb;
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    float contribution = max(luma - push.params.x, 0.0);
    return float4(color * contribution, 1.0);
}

[shader("fragment")]
float4 blurFragment(VertexOutput input) : SV_Target
{
    // Nine-tap gaussian along the direction push.params carries in xy (texel
    // steps), offset by one texel between taps.
    float3 sum = image_table[push.slots.x].Sample(input.uv).rgb * 0.227;
    float3 s1 = image_table[push.slots.x].Sample(input.uv + push.params.xy * 1.3846).rgb;
    float3 s2 = image_table[push.slots.x].Sample(input.uv - push.params.xy * 1.3846).rgb;
    float3 s3 = image_table[push.slots.x].Sample(input.uv + push.params.xy * 3.2308).rgb;
    float3 s4 = image_table[push.slots.x].Sample(input.uv - push.params.xy * 3.2308).rgb;
    sum += (s1 + s2) * 0.316 + (s3 + s4) * 0.070;
    return float4(sum, 1.0);
}

// ACES approximation (Krzysztof Narkowicz's fit).
float3 tonemap(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

// One LSB of positional noise: 8-bit output bands on smooth gradients, and a
// half-LSB dither dissolves the steps without changing the average.
float ditherValue(float2 position)
{
    return frac(sin(dot(position, float2(12.9898, 78.233))) * 43758.5453);
}

// The sRGB transfer function, for a surface that does not encode on write.
float3 encodeSrgb(float3 color)
{
    const float3 high = 1.055 * pow(max(color, float3(0.0, 0.0, 0.0)), 1.0 / 2.4) - 0.055;
    return lerp(high, 12.92 * color, step(color, float3(0.0031308, 0.0031308, 0.0031308)));
}

[shader("fragment")]
float4 tonemapFragment(VertexOutput input) : SV_Target
{
    // The scene may have been rendered into a sub-rectangle of its target
    // (dynamic resolution): y/w carry the uv window the tonemap reads.
    float2 uv = input.uv * push.params.yw;
    float3 hdr = image_table[push.slots.x].Sample(uv).rgb;
    // z: 1 when the stack has bloom, so the second slot is sampled only when
    // the feature exists.
    // The bloom chain covers the whole target, so it is sampled unscaled.
    float3 bloom = push.params.z > 0.5
                       ? image_table[push.slots.y].Sample(input.uv).rgb
                       : float3(0.0, 0.0, 0.0);
    float3 ldr = tonemap(hdr + bloom * 0.35);
    ldr += (ditherValue(input.position.xy) - 0.5) * (1.0 / 255.0);
    // x: 1 when the surface presents linear and this pass owns the encode.
    // Encoding here puts FXAA on perceptual luma and leaves the blend a
    // plain composite of display-ready values.
    if (push.params.x > 0.5)
    {
        ldr = encodeSrgb(ldr);
    }
    return float4(ldr, 1.0);
}

[shader("fragment")]
float4 fxaaFragment(VertexOutput input) : SV_Target
{
    // Console-style FXAA: blend toward the edge-perpendicular average when the
    // local luma contrast exceeds the threshold.
    float2 texel = push.params.xy;
    // z/w carry the uv window the scene was rendered into (dynamic resolution).
    float2 uv = input.uv * push.params.zw;
    float3 rgb_m = image_table[push.slots.x].Sample(uv).rgb;
    float3 rgb_n = image_table[push.slots.x].Sample(uv + float2(0.0, -texel.y)).rgb;
    float3 rgb_s = image_table[push.slots.x].Sample(uv + float2(0.0, texel.y)).rgb;
    float3 rgb_e = image_table[push.slots.x].Sample(uv + float2(texel.x, 0.0)).rgb;
    float3 rgb_w = image_table[push.slots.x].Sample(uv + float2(-texel.x, 0.0)).rgb;

    float luma_m = dot(rgb_m, float3(0.299, 0.587, 0.114));
    float luma_n = dot(rgb_n, float3(0.299, 0.587, 0.114));
    float luma_s = dot(rgb_s, float3(0.299, 0.587, 0.114));
    float luma_e = dot(rgb_e, float3(0.299, 0.587, 0.114));
    float luma_w = dot(rgb_w, float3(0.299, 0.587, 0.114));

    float luma_min = min(luma_m, min(min(luma_n, luma_s), min(luma_e, luma_w)));
    float luma_max = max(luma_m, max(max(luma_n, luma_s), max(luma_e, luma_w)));
    if (luma_max - luma_min < max(0.05, luma_max * 0.125))
    {
        return float4(rgb_m, 1.0);
    }

    float2 blend_direction = normalize(float2(-((luma_n + luma_s) - (luma_e + luma_w)),
                                              (luma_n + luma_s) + (luma_e + luma_w)));
    blend_direction = normalize(
        float2(-blend_direction.y, blend_direction.x) *
        min(texel.x, texel.y));
    float3 blended = 0.5 * (image_table[push.slots.x].Sample(uv + blend_direction).rgb +
                            image_table[push.slots.x].Sample(input.uv - blend_direction).rgb);
    return float4(blended, 1.0);
}

[shader("fragment")]
float4 blendFragment(VertexOutput input) : SV_Target
{
    // The scene is tonemapped and already antialiased. Both UI layers hold
    // premultiplied color — tgfx draws into its target that way and the overlay
    // pass accumulates quads with source-alpha blending — so compositing here
    // is a plain source-over.
    // x names the scene, z the UI layer; the scene was rendered into a
    // sub-rectangle (dynamic resolution) and is magnified here, the UI is
    // full-size and sampled as is.
    float3 scene = image_table[push.slots.x].Sample(input.uv * push.params.zw).rgb;
    float4 ui = image_table[push.slots.z].Sample(input.uv);
    return float4(scene * (1.0 - ui.a) + ui.rgb, 1.0);
}
