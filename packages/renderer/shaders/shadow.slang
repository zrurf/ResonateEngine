// The shadow passes: depth-only rendering from the directional light's view,
// one cascade per pass. Entries: shadowVertex, shadowFragment.

struct FrameData
{
    float4x4 view_proj;
    float4x4 camera_world;
    float4 camera_and_near;             // xyz = world position, w = near
    float4 direction_and_far;           // xyz = directional direction, w = far
    float4 directional_color_intensity; // rgb = color, a = intensity
    float4 light_count_and_pad;         // x = light count
    float4x4 light_view_proj[3];
    float4 split_distances; // far view depth per cascade
};

struct Push
{
    float4x4 model;
    float4 tint;
    uint4 slots;  // frame data, cluster boxes, light grid, light indices
    uint4 slots2; // light list, cascade index in y
};

[[vk::binding(0, 0)]] StructuredBuffer<FrameData> frame_table[7];

[[vk::push_constant]] Push push;

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

[shader("vertex")]
float4 shadowVertex(VertexInput input) : SV_Position
{
    FrameData frame = frame_table[push.slots.x].Load(0);
    float4 world = mul(push.model, float4(input.position, 1.0));
    return mul(frame.light_view_proj[push.slots2.y], world);
}

// Depth-only: the pipeline has no color target, so the stage writes none.
[shader("fragment")]
void shadowFragment()
{
}
