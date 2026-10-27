// The MSAA bindless-sample probe (R1): a fullscreen triangle whose fragment
// reads the table's texture slot at a fixed UV and writes the texel out. The
// raw-device test renders it into 1x and samples>1 passes of the same target
// and compares the readback, with no renderer in between.
struct Push
{
    uint4 slots; // shape-compatible with the scene push's head
    uint texture_slot;
};

[[vk::push_constant]] Push push;
[[vk::binding(5, 0)]] Sampler2D image_table[16];

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
    return float4(image_table[push.texture_slot].Sample(float2(0.5, 0.5)).rgb, 1.0);
}

[[vk::binding(0, 0)]] RWStructuredBuffer<uint4> probe_storage[7];

[shader("compute")]
[numthreads(1, 1, 1)]
void probeCompute(uint3 tid : SV_DispatchThreadID)
{
    probe_storage[push.texture_slot][tid.x] = uint4(tid, 1U);
}
