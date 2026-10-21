// The cluster passes: build the world-space AABB of every cluster, then cull
// the point lights against each box. The grid is CLUSTERS_X x CLUSTERS_Y x
// CLUSTERS_Z over the screen, one thread per cluster.
// Entries: clusterBuild, clusterCull.
//
// Resource-table convention (device.h): set 0 holds the storage arrays at
// bindings 0-4; every buffer here is reached at the slot its handle carries,
// named by the push constants.

struct FrameData
{
    float4x4 view_proj;
    float4x4 camera_world;
    float4 camera_and_near;             // xyz = world position, w = near
    float4 direction_and_far;           // xyz = directional direction, w = far
    float4 directional_color_intensity; // rgb = color, a = intensity
    float4 light_count_and_pad;         // x = light count
};

struct Light
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

struct ClusterAABB
{
    float4 minimum;
    float4 maximum;
};

struct ClusterLights
{
    uint offset;
    uint count;
};

struct Push
{
    float4x4 model;
    float4 tint;
    uint4 slots;  // frame data, cluster AABBs, light grid, light indices
    uint4 slots2; // light list
};

[[vk::binding(0, 0)]] StructuredBuffer<FrameData> frame_table[7];
[[vk::binding(1, 0)]] StructuredBuffer<Light> light_table[7];
[[vk::binding(2, 0)]] RWStructuredBuffer<ClusterAABB> cluster_boxes[7];
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> light_indices[7];
[[vk::binding(4, 0)]] RWStructuredBuffer<ClusterLights> cluster_lights[7];

[[vk::push_constant]] Push push;

static const uint CLUSTERS_X = 16;
static const uint CLUSTERS_Y = 9;
static const uint CLUSTERS_Z = 24;
static const uint MAX_LIGHTS_PER_CLUSTER = 128;

// The view-space depth of the slice boundary a cluster's near (or far) face
// sits at: slices spaced exponentially between near and far, so nearby
// clusters stay thin.
float sliceDepth(uint slice)
{
    FrameData frame = frame_table[push.slots.x].Load(0);
    float t = float(slice) / float(CLUSTERS_Z);
    return frame.camera_and_near.w *
           pow(frame.direction_and_far.w / frame.camera_and_near.w, t);
}

float3 viewToWorld(float3 view_point, FrameData frame)
{
    // The camera world matrix is rigid: the point rotates then translates.
    float3 world = frame.camera_world._m00_m01_m02 * view_point.x +
                   frame.camera_world._m10_m11_m12 * view_point.y +
                   frame.camera_world._m20_m21_m22 * view_point.z;
    return world + frame.camera_and_near.xyz;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void clusterBuild(uint3 tid : SV_DispatchThreadID)
{
    FrameData frame = frame_table[push.slots.x].Load(0);
    uint cluster = tid.x + tid.y * CLUSTERS_X + tid.z * CLUSTERS_X * CLUSTERS_Y;

    float2 tile_min = float2(float(tid.x) / float(CLUSTERS_X) * 2.0 - 1.0,
                             float(tid.y) / float(CLUSTERS_Y) * 2.0 - 1.0);
    float2 tile_max = float2(float(tid.x + 1) / float(CLUSTERS_X) * 2.0 - 1.0,
                             float(tid.y + 1) / float(CLUSTERS_Y) * 2.0 - 1.0);
    float near = sliceDepth(tid.z);
    float far = sliceDepth(tid.z + 1);

    float width_scale = frame.view_proj._m00;
    float height_scale = frame.view_proj._m11;

    float3 minimum = float3(1e30, 1e30, 1e30);
    float3 maximum = float3(-1e30, -1e30, -1e30);
    const uint CORNERS = 8;
    for (uint corner = 0; corner < CORNERS; ++corner)
    {
        float2 ndc = float2((corner & 1U) != 0U ? tile_max.x : tile_min.x,
                            (corner & 2U) != 0U ? tile_max.y : tile_min.y);
        float depth = (corner & 4U) != 0U ? far : near;
        // Undo the projection: x_ndc = x_view * width_scale / depth, and the
        // flipped y reads through the negative height scale.
        float3 view_point = float3(ndc.x * depth / width_scale, ndc.y * depth / height_scale,
                                   depth);
        float3 world_point = viewToWorld(view_point, frame);
        minimum = min(minimum, world_point);
        maximum = max(maximum, world_point);
    }

    cluster_boxes[push.slots.y][cluster].minimum = float4(minimum, 0.0);
    cluster_boxes[push.slots.y][cluster].maximum = float4(maximum, 0.0);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void clusterCull(uint3 tid : SV_DispatchThreadID)
{
    uint cluster = tid.x + tid.y * CLUSTERS_X + tid.z * CLUSTERS_X * CLUSTERS_Y;
    FrameData frame = frame_table[push.slots.x].Load(0);
    ClusterAABB box = cluster_boxes[push.slots.y].Load(cluster);

    // Light index storage is shared by every cluster: each takes its slice up
    // front, and the count fills what the sphere-box test accepted.
    uint offset = cluster * MAX_LIGHTS_PER_CLUSTER;
    uint count = 0;
    for (uint light = 0; light < frame.light_count_and_pad.x && count < MAX_LIGHTS_PER_CLUSTER;
         ++light)
    {
        Light l = light_table[push.slots2.x].Load(light);
        float3 closest = clamp(l.position, box.minimum.xyz, box.maximum.xyz);
        float distance_squared = dot(l.position - closest, l.position - closest);
        if (distance_squared <= l.radius * l.radius)
        {
            light_indices[push.slots.w][offset + count] = light;
            ++count;
        }
    }
    cluster_lights[push.slots.z][cluster].offset = offset;
    cluster_lights[push.slots.z][cluster].count = count;
}
