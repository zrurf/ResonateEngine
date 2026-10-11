// The first shader of the Vulkan backend: one centered triangle from
// SV_VertexID, no vertex buffers, vertex colors interpolated across it.
// Entries: vertexMain, fragmentMain.

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    // Vulkan's NDC points Y down, so -y is the top of the frame: the triangle
    // sits upright on the canvas's clear background.
    float2 positions[3] = {float2(-0.55, -0.5), float2(0.55, -0.5), float2(0.0, 0.55)};
    float3 colors[3] = {float3(0.22, 0.55, 0.95), float3(0.15, 0.85, 0.60),
                        float3(0.95, 0.45, 0.60)};

    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.color = colors[vertex_id];
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
