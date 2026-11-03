// The MSAA bindless-sample probe (R1): a fullscreen triangle whose fragment
// reads the table's texture slot at a fixed UV and writes the texel out. The
// raw-device test renders it into 1x and samples>1 passes of the same target
// and compares the readback, with no renderer in between.
// The scene push's exact shape: the fields the probe reads sit at the same
// offsets the scene shader reads them at.
struct Push
{
    float4x4 model;
    float4 tint;
    uint4 slots; // x = the frame-data storage slot
    uint4 slots2;
    uint texture_slot;
    float roughness;
    float metallic;
};

struct FrameData
{
    float4x4 view_proj;
    float4x4 camera_world;
    float4 camera_and_near;
    float4 direction_and_far;
    float4 directional_color_intensity;
    float4 light_count_and_pad;
    float4x4 light_view_proj[3];
    float4 split_distances;
    uint uniform_light_count;
    uint uniform_light_pad0;
    uint uniform_light_pad1;
    uint uniform_light_pad2;
};

[[vk::push_constant]] Push push;
[[vk::binding(0, 0)]] StructuredBuffer<FrameData> frame_table[7];
[[vk::binding(5, 0)]] Sampler2D image_table[64];

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : UV;
};

[shader("vertex")]
VertexOutput probeVertex(uint vertex : SV_VertexID)
{
    float2 uv = float2((vertex << 1U) & 2U, vertex & 2U);
    VertexOutput out;
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

[shader("fragment")]
float4 probeFragment(VertexOutput input) : SV_Target
{
    float3 tex = image_table[push.texture_slot].Sample(float2(0.5, 0.5)).rgb;
    float4 sun = frame_table[push.slots.x][0].directional_color_intensity;
    return float4(tex * sun.rgb * sun.a, 1.0);
}

// The post-pass stand-in: samples the resolved target (bound at the texture
// slot) and writes it on, 1x, the way the tonemap reads the HDR.
[shader("fragment")]
float4 probeCopy(VertexOutput input) : SV_Target
{
    return float4(image_table[1].Sample(float2(0.5, 0.5)).rgb, 1.0);
}

[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> probe_storage[7];

[shader("compute")]
[numthreads(1, 1, 1)]
void probeCompute(uint3 tid : SV_DispatchThreadID)
{
    probe_storage[push.texture_slot][tid.x] = uint4(tid, 1U);
}
