// The post chain's stages: bright extraction, the bloom pyramid's downsample
// and upsample steps, tonemap, FXAA over the scene only, the EASU upscale to
// the output, and the UI blend with RCAS sharpening. All draw a full-screen
// triangle from the vertex id and read their inputs through the sampled-image
// table.
// Entries: postVertex, brightFragment, bloomDownFragment, bloomUpFragment,
// tonemapFragment, fxaaFragment, upscaleFragment, magnifyFragment,
// blendFragment.

struct PostPush
{
    uint4 slots;   // x: input image, y: second input (the bloom image for the
                   // tonemap, this level's own image for the bloom upsample)
    float4 params; // bright: threshold in x, uv window in y/w; bloom down/up:
                   // source texel size in x/y; upscale: the scene extents in
                   // x/y and one output texel in z/w; blend: one output texel
                   // in x/y and the RCAS sharpness in z
};

[[vk::binding(5, 0)]] Sampler2D image_table[64];

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
float4 bloomDownFragment(VertexOutput input) : SV_Target
{
    // Four bilinear fetches on the corners of the destination texel: for an
    // exact 2x reduction they land on the four source texel centers, so this
    // is the box filter the chain's mip would apply.
    float2 t = push.params.xy;
    float3 sum = image_table[push.slots.x].Sample(input.uv + float2(-0.5, -0.5) * t).rgb;
    sum += image_table[push.slots.x].Sample(input.uv + float2(0.5, -0.5) * t).rgb;
    sum += image_table[push.slots.x].Sample(input.uv + float2(-0.5, 0.5) * t).rgb;
    sum += image_table[push.slots.x].Sample(input.uv + float2(0.5, 0.5) * t).rgb;
    return float4(sum * 0.25, 1.0);
}

[shader("fragment")]
float4 bloomUpFragment(VertexOutput input) : SV_Target
{
    // The smaller level lifted by a 3x3 tent and added to this level's own
    // image: the chain ends with every scale summed into the base, which is
    // what widens the halo without a separate blur per radius.
    float2 t = push.params.xy;
    float3 lifted = image_table[push.slots.x].Sample(input.uv).rgb * 4.0;
    lifted += (image_table[push.slots.x].Sample(input.uv + float2(-1.0, 0.0) * t).rgb +
               image_table[push.slots.x].Sample(input.uv + float2(1.0, 0.0) * t).rgb +
               image_table[push.slots.x].Sample(input.uv + float2(0.0, -1.0) * t).rgb +
               image_table[push.slots.x].Sample(input.uv + float2(0.0, 1.0) * t).rgb) *
              2.0;
    lifted += image_table[push.slots.x].Sample(input.uv + float2(-1.0, -1.0) * t).rgb +
              image_table[push.slots.x].Sample(input.uv + float2(1.0, -1.0) * t).rgb +
              image_table[push.slots.x].Sample(input.uv + float2(-1.0, 1.0) * t).rgb +
              image_table[push.slots.x].Sample(input.uv + float2(1.0, 1.0) * t).rgb;
    lifted *= 1.0 / 16.0;
    float3 own = image_table[push.slots.y].Sample(input.uv).rgb;
    return float4(own + lifted, 1.0);
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

// FidelityFX Super Resolution 1.0's EASU and RCAS, adapted from AMD's
// reference (MIT licence) to fragment passes: the twelve EASU taps are bilinear
// fetches at texel centers instead of gathers. The passes work on display
// values already, so FSR's input transform is the identity here.

float fsrLuma(float3 color)
{
    return color.b * 0.5 + (color.r * 0.5 + color.g);
}

// Adds one EASU tap: the offset is rotated into the gradient's frame, scaled by
// the anisotropic lengths, and weighted by a negative-lobe kernel.
void fsrEasuTap(inout float3 accum, inout float weight, float2 offset, float2 dir,
                float2 len, float lob, float clp, float3 color)
{
    float2 v;
    v.x = offset.x * dir.x + offset.y * dir.y;
    v.y = offset.x * (-dir.y) + offset.y * dir.x;
    v *= len;
    float d2 = min(dot(v, v), clp);
    float window = (2.0 / 5.0) * d2 - 1.0;
    float base = lob * d2 - 1.0;
    window *= window;
    base *= base;
    window = (25.0 / 16.0) * window - (25.0 / 16.0 - 1.0);
    float w = window * base;
    accum += color * w;
    weight += w;
}

// Accumulates the gradient and its length from one of the four bilinear
// corners of the EASU window.
void fsrEasuSet(inout float2 dir, inout float len, float w,
                float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1e-6);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    len += lenX * lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1e-6);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    len += lenY * lenY * w;
}

float3 easuTexel(float2 texel, float2 inv_target)
{
    return image_table[push.slots.x].Sample((texel + 0.5) * inv_target).rgb;
}

// EASU: an edge-directed 12-tap reconstruction, stretched along the local
// gradient, that reconstructs edges instead of smearing them the way a bilinear
// magnify does.
float3 fsrEasu(float2 uv, float2 src_size, float2 inv_target)
{
    float2 pos = uv * src_size - 0.5;
    float2 fp = floor(pos);
    float2 pp = pos - fp;

    float3 b = easuTexel(fp + float2(0.0, -1.0), inv_target);
    float3 c = easuTexel(fp + float2(1.0, -1.0), inv_target);
    float3 e = easuTexel(fp + float2(-1.0, 0.0), inv_target);
    float3 f = easuTexel(fp, inv_target);
    float3 g = easuTexel(fp + float2(1.0, 0.0), inv_target);
    float3 h = easuTexel(fp + float2(2.0, 0.0), inv_target);
    float3 i = easuTexel(fp + float2(-1.0, 1.0), inv_target);
    float3 j = easuTexel(fp + float2(0.0, 1.0), inv_target);
    float3 k = easuTexel(fp + float2(1.0, 1.0), inv_target);
    float3 l = easuTexel(fp + float2(2.0, 1.0), inv_target);
    float3 n = easuTexel(fp + float2(0.0, 2.0), inv_target);
    float3 o = easuTexel(fp + float2(1.0, 2.0), inv_target);

    const float bL = fsrLuma(b);
    const float cL = fsrLuma(c);
    const float eL = fsrLuma(e);
    const float fL = fsrLuma(f);
    const float gL = fsrLuma(g);
    const float hL = fsrLuma(h);
    const float iL = fsrLuma(i);
    const float jL = fsrLuma(j);
    const float kL = fsrLuma(k);
    const float lL = fsrLuma(l);
    const float nL = fsrLuma(n);
    const float oL = fsrLuma(o);

    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    fsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    fsrEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    fsrEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    fsrEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zero = dirR < (1.0 / 32768.0);
    dirR = rsqrt(max(dirR, 1e-12));
    dir = zero ? float2(1.0, 0.0) : dir * dirR;

    len *= 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + (0.21 - 0.5) * len;
    float clp = 1.0 / lob;

    float3 accum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    fsrEasuTap(accum, weight, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    fsrEasuTap(accum, weight, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    fsrEasuTap(accum, weight, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    fsrEasuTap(accum, weight, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    fsrEasuTap(accum, weight, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    fsrEasuTap(accum, weight, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    fsrEasuTap(accum, weight, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    fsrEasuTap(accum, weight, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    fsrEasuTap(accum, weight, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    fsrEasuTap(accum, weight, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    fsrEasuTap(accum, weight, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    fsrEasuTap(accum, weight, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    // The reconstruction is clamped to the four surrounding texels, which is
    // what keeps the negative lobe from ringing.
    float3 min4 = min(min(f, g), min(j, k));
    float3 max4 = max(max(f, g), max(j, k));
    return clamp(accum / max(weight, 1e-5), min4, max4);
}

[shader("fragment")]
float4 upscaleFragment(VertexOutput input) : SV_Target
{
    // x/y: the scene's rendered extent; z/w: one output texel in uv.
    return float4(fsrEasu(input.uv, push.params.xy, push.params.zw), 1.0);
}

[shader("fragment")]
float4 magnifyFragment(VertexOutput input) : SV_Target
{
    // The plain magnify for a device that cannot afford EASU: x/y carry the
    // uv window the scene occupies in its target.
    return float4(image_table[push.slots.x].Sample(input.uv * push.params.xy).rgb, 1.0);
}

// RCAS: a contrast-adaptive sharpen of the upscaled scene. The lobe grows where
// the neighbourhood is flat and shrinks on strong edges, and the result is
// clamped to the neighbourhood, so detail returns without haloes.
float3 fsrRcas(float2 uv, float2 inv_target, float sharpness)
{
    float3 b = image_table[push.slots.x].Sample(uv + float2(0.0, -1.0) * inv_target).rgb;
    float3 d = image_table[push.slots.x].Sample(uv + float2(-1.0, 0.0) * inv_target).rgb;
    float3 e = image_table[push.slots.x].Sample(uv).rgb;
    float3 f = image_table[push.slots.x].Sample(uv + float2(1.0, 0.0) * inv_target).rgb;
    float3 h = image_table[push.slots.x].Sample(uv + float2(0.0, 1.0) * inv_target).rgb;

    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, float3(1e-6, 1e-6, 1e-6));
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, float3(-1e-6, -1e-6, -1e-6));
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * sharpness;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    return (lobe * (b + d + h + f) + e) * rcpL;
}

[shader("fragment")]
float4 blendFragment(VertexOutput input) : SV_Target
{
    // The scene reaches this pass tonemapped and antialiased. On the EASU path
    // it was already magnified by the upscale pass, so z carries the RCAS
    // sharpness and RCAS is the last thing that touches it; only its result
    // meets the UI. The bilinear fallback magnifies here instead, with x/y
    // holding the uv window the scene occupies. Both UI layers hold
    // premultiplied color — tgfx draws into its target that way and the overlay
    // pass accumulates quads with source-alpha blending — so compositing over
    // it is a plain source-over, and full-resolution text never passes through
    // the sharpener.
    float3 scene = push.params.z > 0.0
                       ? fsrRcas(input.uv, push.params.xy, push.params.z)
                       : image_table[push.slots.x].Sample(input.uv).rgb;
    // The canvas quantizes to eight bits, so the dither belongs at output
    // resolution: dithering the scaled scene first would let the upscale and
    // the sharpen turn the noise into visible blocks. Only the scene is
    // dithered, so an opaque UI pixel keeps its exact color.
    scene += (ditherValue(input.position.xy) - 0.5) * (1.0 / 255.0);
    float4 ui = image_table[push.slots.z].Sample(input.uv);
    return float4(scene * (1.0 - ui.a) + ui.rgb, 1.0);
}
