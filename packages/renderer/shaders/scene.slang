// The scene pass's shared vertex and the compat fragment: Blinn-Phong shading
// against the frame's clusters, the directional light shadowed by CSM, and
// depth fog. Renders into the HDR target; the post chain tonemaps.
// The per-pixel cascade lookup lives in scene_lit.slang: slangc carries every
// function of a source into each entry's module, so this file stays free of
// the NonUniform decoration the Mali-class driver refuses.
#include "scene_common.slangh"

[shader("vertex")]
VertexOutput sceneVertex(VertexInput input)
{
    VertexOutput output;
    FrameData frame = frame_block;
    float4 world = mul(push.model, float4(input.position, 1.0));
    output.position = mul(frame.view_proj, world);
    // The cofactor matrix is the inverse transpose up to the determinant, which
    // the normalize removes: normals stay perpendicular under non-uniform scale.
    float3x3 basis = (float3x3)push.model;
    float3x3 normal_matrix =
        float3x3(cross(basis[1], basis[2]), cross(basis[2], basis[0]), cross(basis[0], basis[1]));
    output.normal = normalize(mul(normal_matrix, input.normal));
    output.color = input.color;
    output.world_position = world.xyz;
    output.view_depth = mul(frame.view_proj, world).w;
    output.uv = input.uv;
    return output;
}

// A device without descriptor indexing cannot index the table per pixel. The
// three cascade slots are push constants, so each lookup is dynamically
// uniform; sample all three and select the one the pixel's cascade names.
// The compat tier renders a single full-range cascade (cascade 0), so one fetch
// set covers the whole shadow distance — no per-pixel cascade selection, which
// is exactly what this tier cannot do.
float shadowFactorCompat(float3 world_position, float view_depth, FrameData frame)
{
    float4 shadow_position = mul(frame.light_view_proj[0], float4(world_position, 1.0));
    float3 shadow_ndc = shadow_position.xyz / shadow_position.w;
    if (shadow_ndc.x < -1.0 || shadow_ndc.x > 1.0 || shadow_ndc.y < -1.0 || shadow_ndc.y > 1.0)
    {
        return 1.0;
    }
    float2 uv = float2(shadow_ndc.x * 0.5 + 0.5, shadow_ndc.y * 0.5 + 0.5);
    float receiver = shadow_ndc.z;
    return pcfOf(push.slots2[1], uv, receiver);
}

[shader("fragment")]
float4 sceneFragmentCompat(VertexOutput input) : SV_Target
{
    FrameData frame = frame_block;
    return shadeScene(input, frame,
                      shadowFactorCompat(input.world_position, input.view_depth, frame), true);
}
