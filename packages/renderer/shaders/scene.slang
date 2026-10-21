// The scene pass: Blinn-Phong shading against the frame's clusters, the
// directional light shadowed by CSM, and depth fog. Renders into the HDR
// target; the post chain tonemaps.
// Entries: sceneVertex, sceneFragment, sceneFragmentCompat.
//
// Resource-table convention (device.h): set 0 holds the storage arrays at
// bindings 0-4 and the sampled-image array at binding 5; slots arrive in the
// push constants.

struct FrameData
{
    float4x4 view_proj;
    float4x4 camera_world;
    float4 camera_and_near;             // xyz = world position, w = near
    float4 direction_and_far;           // xyz = directional direction, w = far
    float4 directional_color_intensity; // rgb = color, a = intensity
    float4 light_count_and_pad;         // x = light count
    float4x4 light_view_proj[3];
    float4 split_distances; // far view-space depth per cascade
};

struct Light
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

struct ClusterLights
{
    uint offset;
    uint count;
};

struct Push
{
    float4x4 model;
    float4 tint;          // rgb = albedo tint, a = unused
    uint4 slots;          // frame data, cluster boxes, light grid, light indices
    uint4 slots2;         // light list, shadow map slots 0..2
    uint texture_slot;    // albedo texture (0 = white)
    float roughness;      // metal-rough parameters of the material
    float metallic;
};

[[vk::binding(0, 0)]] StructuredBuffer<FrameData> frame_table[7];
[[vk::binding(1, 0)]] StructuredBuffer<Light> light_table[7];
[[vk::binding(2, 0)]] StructuredBuffer<uint> light_indices[7];
[[vk::binding(3, 0)]] StructuredBuffer<ClusterLights> cluster_lights[7];
[[vk::binding(5, 0)]] Sampler2D image_table[16];

[[vk::push_constant]] Push push;

static const uint CLUSTERS_X = 16;
static const uint CLUSTERS_Y = 9;
static const uint CLUSTERS_Z = 24;

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color;
    float3 normal;
    float3 world_position;
    float view_depth;
    float2 uv;
};

[shader("vertex")]
VertexOutput sceneVertex(VertexInput input)
{
    VertexOutput output;
    FrameData frame = frame_table[push.slots.x].Load(0);
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

// The pixel's cluster: CLUSTERS_X x CLUSTERS_Y tiles of the target's extent,
// and the depth slice by inverting the exponential spacing the build pass used.
uint clusterOf(float2 pixel, float view_depth, FrameData frame)
{
    float2 tile_size = frame.light_count_and_pad.yz;
    uint2 tile = min(uint2(pixel / tile_size), uint2(CLUSTERS_X - 1, CLUSTERS_Y - 1));
    float slice_t = log(view_depth / frame.camera_and_near.w) /
                    log(frame.direction_and_far.w / frame.camera_and_near.w);
    uint slice = uint(saturate(slice_t) * float(CLUSTERS_Z));
    return tile.x + tile.y * CLUSTERS_X + slice * CLUSTERS_X * CLUSTERS_Y;
}

// The cascade covering this view depth: the first whose split exceeds it.
uint cascadeOf(float view_depth, FrameData frame)
{
    for (uint cascade = 0; cascade < 2; ++cascade)
    {
        if (view_depth < frame.split_distances[cascade])
        {
            return cascade;
        }
    }
    return 2;
}

// Four-tap PCF against one cascade's shadow map; the map was rendered from the
// light's view, so the shadow coordinates come from the same matrix.
float pcfOf(uint slot, float2 uv, float receiver, float texel)
{
    float sum = 0.0;
    const float2 TAPS[4] = {float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5),
                            float2(0.5, 0.5)};
    for (uint tap = 0; tap < 4; ++tap)
    {
        float depth = image_table[slot].Sample(uv + TAPS[tap] * texel).r;
        sum += receiver - 0.003 > depth ? 0.0 : 1.0;
    }
    return sum * 0.25;
}

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
    float texel = 1.0 / 2048.0;

    // The cascade differs per pixel, so the table lookup does too.
    uint shadow_slot = NonUniformResourceIndex(push.slots2[cascade + 1]);
    return pcfOf(shadow_slot, uv, receiver, texel);
}

// A device without descriptor indexing cannot index the table per pixel. The
// three cascade slots are push constants, so each lookup is dynamically
// uniform; sample all three and select the one the pixel's cascade names.
float shadowFactorCompat(float3 world_position, float view_depth, FrameData frame)
{
    uint cascade = cascadeOf(view_depth, frame);
    float4 shadow_position = mul(frame.light_view_proj[cascade], float4(world_position, 1.0));
    float3 shadow_ndc = shadow_position.xyz / shadow_position.w;
    if (shadow_ndc.x < -1.0 || shadow_ndc.x > 1.0 || shadow_ndc.y < -1.0 || shadow_ndc.y > 1.0)
    {
        return 1.0;
    }
    float2 uv = float2(shadow_ndc.x * 0.5 + 0.5, shadow_ndc.y * 0.5 + 0.5);
    float receiver = shadow_ndc.z;
    float texel = 1.0 / 2048.0;

    float cascades0 = pcfOf(push.slots2[1], uv, receiver, texel);
    float cascades1 = pcfOf(push.slots2[2], uv, receiver, texel);
    float cascades2 = pcfOf(push.slots2[3], uv, receiver, texel);
    return cascade == 0 ? cascades0 : (cascade == 1 ? cascades1 : cascades2);
}

static const float PI = 3.14159265;

float distributionGGX(float3 n, float3 h, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float n_dot_h = saturate(dot(n, h));
    float denominator = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-5);
}

float geometrySchlick(float n_dot_v, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return n_dot_v / max(n_dot_v * (1.0 - k) + k, 1e-5);
}

float3 fresnelSchlick(float cos_theta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cos_theta), 5.0);
}

// Metal-rough GGX for one light; `radiance` already carries the shadow and
// attenuation factors.
float3 shade(float3 albedo, float3 normal, float3 view_direction, float3 light_direction,
             float3 radiance, float roughness, float metallic)
{
    float n_dot_l = saturate(dot(normal, light_direction));
    if (n_dot_l <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float3 half_vector = normalize(view_direction + light_direction);
    float n_dot_v = saturate(dot(normal, view_direction));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float d = distributionGGX(normal, half_vector, roughness);
    float g = geometrySchlick(n_dot_v, roughness) * geometrySchlick(n_dot_l, roughness);
    float3 f = fresnelSchlick(saturate(dot(half_vector, view_direction)), f0);
    float3 specular = (d * g * f) / max(4.0 * n_dot_v * n_dot_l, 1e-4);
    float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * n_dot_l;
}

float4 shadeScene(VertexOutput input, bool compat)
{
    FrameData frame = frame_table[push.slots.x].Load(0);
    ClusterLights lights = cluster_lights[push.slots.z].Load(
        clusterOf(input.position.xy, input.view_depth, frame));

    float3 albedo = input.color * push.tint.rgb *
                    image_table[push.texture_slot].Sample(input.uv).rgb;
    float3 normal = normalize(input.normal);
    float3 view_direction = normalize(frame.camera_and_near.xyz - input.world_position);

    float3 color = albedo * 0.12; // ambient floor
    float3 directional_color = frame.directional_color_intensity.rgb *
                               frame.directional_color_intensity.a;
    float shadow = compat ? shadowFactorCompat(input.world_position, input.view_depth, frame)
                          : shadowFactor(input.world_position, input.view_depth, frame);
    const float roughness = clamp(push.roughness, 0.04, 1.0);
    color += shade(albedo, normal, view_direction, -frame.direction_and_far.xyz,
                   directional_color * shadow, roughness, push.metallic);

    for (uint i = 0; i < lights.count; ++i)
    {
        uint light_index = light_indices[push.slots.w].Load(lights.offset + i);
        Light light = light_table[push.slots2.x].Load(light_index);
        float3 to_light = light.position - input.world_position;
        float distance = length(to_light);
        float attenuation = saturate(1.0 - distance / light.radius);
        color += shade(albedo, normal, view_direction, to_light / distance,
                       light.color * light.intensity * attenuation, roughness, push.metallic);
    }

    // Depth fog: the last tenth of the view range fades into the sky tone.
    float fog = saturate((input.view_depth - frame.direction_and_far.w * 0.85) /
                         (frame.direction_and_far.w * 0.15));
    color = lerp(color, float3(0.05, 0.06, 0.09), fog);

    return float4(color, 1.0);
}

[shader("fragment")]
float4 sceneFragment(VertexOutput input) : SV_Target
{
    return shadeScene(input, false);
}

[shader("fragment")]
float4 sceneFragmentCompat(VertexOutput input) : SV_Target
{
    return shadeScene(input, true);
}
