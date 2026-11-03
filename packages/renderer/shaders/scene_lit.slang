// The scene pass's bindless fragment: the cascade slot is indexed per pixel,
// which needs the NonUniform decoration. slangc keeps unreferenced functions in
// every module it emits from a source, so this entry lives apart from the
// compat fragment and the Mali-class driver never loads it.
#include "scene_common.slangh"

// The per-pixel cascade index is marked non-uniform at the access site.
float pcfOfNonUniform(uint cascade, float2 uv, float receiver)
{
    float sum = 0.0;
    float2 texel = max(abs(ddx(uv)), abs(ddy(uv)));
    const float2 TAPS[4] = {float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5),
                            float2(0.5, 0.5)};
    for (uint tap = 0; tap < 4; ++tap)
    {
        sum += shadow_table[NonUniformResourceIndex(cascade)].SampleCmpLevelZero(
            uv + TAPS[tap] * texel, receiver - 0.003);
    }
    return sum * 0.25;
}

// The cascade covering the pixel differs, so the table lookup does too.
float shadowFactor(float3 world_position, float view_depth, FrameData frame)
{
    uint cascade = cascadeOf(view_depth, frame);
    float4 shadow_position = mul(frame.light_view_proj[cascade], float4(world_position, 1.0));
    float3 shadow_ndc = shadow_position.xyz / shadow_position.w;
    if (shadow_ndc.x < -1.0 || shadow_ndc.x > 1.0 || shadow_ndc.y < -1.0 || shadow_ndc.y > 1.0)
    {
        return 1.0; // outside every cascade: unshadowed
    }
    float2 uv = float2(shadow_ndc.x * 0.5 + 0.5, shadow_ndc.y * 0.5 + 0.5);
    float receiver = shadow_ndc.z;
    return pcfOfNonUniform(cascade, uv, receiver);
}

[shader("fragment")]
float4 sceneFragment(VertexOutput input) : SV_Target
{
    FrameData frame = frame_block;
    return shadeScene(input, frame,
                      shadowFactor(input.world_position, input.view_depth, frame), false);
}
