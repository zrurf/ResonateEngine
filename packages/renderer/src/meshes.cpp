#include "meshes.h"

#include <array>
#include <cstdio>
#include <vector>

namespace resonate::renderer
{
namespace
{

ResonateRenderBuffer uploadVertices(ResonateRenderDevice& device,
                                    const std::vector<Vertex>& vertices)
{
    ResonateRenderBufferDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.size_bytes = vertices.size() * sizeof(Vertex);
    desc.usage = RESONATE_RENDER_BUFFER_VERTEX;
    desc.host_visible = 1U;
    ResonateRenderBuffer buffer{};
    if (device.create_buffer(device.self, &desc, &buffer) != RESONATE_OK)
    {
        std::fprintf(stderr, "renderer: vertex create failed\n");
        return ResonateRenderBuffer{};
    }
    if (device.upload_buffer(device.self, buffer, 0, vertices.data(), desc.size_bytes) !=
        RESONATE_OK)
    {
        std::fprintf(stderr, "renderer: vertex upload failed\n");
        return ResonateRenderBuffer{};
    }
    return buffer;
}

ResonateRenderBuffer uploadIndices(ResonateRenderDevice& device,
                                   const std::vector<uint32_t>& indices)
{
    ResonateRenderBufferDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.size_bytes = indices.size() * sizeof(uint32_t);
    desc.usage = RESONATE_RENDER_BUFFER_INDEX;
    desc.host_visible = 1U;
    ResonateRenderBuffer buffer{};
    if (device.create_buffer(device.self, &desc, &buffer) != RESONATE_OK)
    {
        std::fprintf(stderr, "renderer: index create failed\n");
        return ResonateRenderBuffer{};
    }
    if (device.upload_buffer(device.self, buffer, 0, indices.data(), desc.size_bytes) !=
        RESONATE_OK)
    {
        std::fprintf(stderr, "renderer: index upload failed\n");
        return ResonateRenderBuffer{};
    }
    return buffer;
}

/* One cube face: four corners around a center, shaded along the axis. */
void addFace(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
             const std::array<std::array<float, 3>, 4>& corners, const std::array<float, 3>& normal,
             const std::array<float, 3>& color)
{
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    for (const std::array<float, 3>& corner : corners)
    {
        vertices.push_back(Vertex{{corner[0], corner[1], corner[2]},
                                  {normal[0], normal[1], normal[2]},
                                  {color[0], color[1], color[2]}});
    }
    const uint32_t order[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    for (uint32_t index : order)
    {
        indices.push_back(base + index);
    }
}

} // namespace

GpuMesh buildCube(ResonateRenderDevice& device)
{
    static constexpr float kHalf = 0.5F;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    const std::array<float, 3> px = {0.85F, 0.45F, 0.35F};
    const std::array<float, 3> nx = {0.45F, 0.62F, 0.85F};
    const std::array<float, 3> py = {0.85F, 0.75F, 0.40F};
    const std::array<float, 3> ny = {0.35F, 0.38F, 0.42F};
    const std::array<float, 3> pz = {0.40F, 0.78F, 0.55F};
    const std::array<float, 3> nz = {0.75F, 0.42F, 0.72F};

    const auto face = [](std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                         std::array<std::array<float, 3>, 4> corners,
                         const std::array<float, 3>& normal, const std::array<float, 3>& color)
    { addFace(vertices, indices, corners, normal, color); };

    /* +x, -x, +y, -y, +z, -z */
    face(vertices, indices,
         {{{kHalf, -kHalf, kHalf},
           {kHalf, -kHalf, -kHalf},
           {kHalf, kHalf, -kHalf},
           {kHalf, kHalf, kHalf}}},
         {1, 0, 0}, px);
    face(vertices, indices,
         {{{-kHalf, -kHalf, -kHalf},
           {-kHalf, -kHalf, kHalf},
           {-kHalf, kHalf, kHalf},
           {-kHalf, kHalf, -kHalf}}},
         {-1, 0, 0}, nx);
    face(vertices, indices,
         {{{-kHalf, kHalf, kHalf},
           {kHalf, kHalf, kHalf},
           {kHalf, kHalf, -kHalf},
           {-kHalf, kHalf, -kHalf}}},
         {0, 1, 0}, py);
    face(vertices, indices,
         {{{-kHalf, -kHalf, -kHalf},
           {kHalf, -kHalf, -kHalf},
           {kHalf, -kHalf, kHalf},
           {-kHalf, -kHalf, kHalf}}},
         {0, -1, 0}, ny);
    face(vertices, indices,
         {{{-kHalf, -kHalf, kHalf},
           {kHalf, -kHalf, kHalf},
           {kHalf, kHalf, kHalf},
           {-kHalf, kHalf, kHalf}}},
         {0, 0, 1}, pz);
    face(vertices, indices,
         {{{kHalf, -kHalf, -kHalf},
           {-kHalf, -kHalf, -kHalf},
           {-kHalf, kHalf, -kHalf},
           {kHalf, kHalf, -kHalf}}},
         {0, 0, -1}, nz);

    return GpuMesh{uploadVertices(device, vertices), uploadIndices(device, indices),
                   static_cast<uint32_t>(indices.size())};
}

GpuMesh buildGround(ResonateRenderDevice& device, float half_extent)
{
    const std::array<Vertex, 4> vertices = {
        Vertex{{-half_extent, 0.0F, half_extent}, {0, 1, 0}, {0.24F, 0.26F, 0.30F}},
        Vertex{{half_extent, 0.0F, half_extent}, {0, 1, 0}, {0.24F, 0.26F, 0.30F}},
        Vertex{{half_extent, 0.0F, -half_extent}, {0, 1, 0}, {0.16F, 0.18F, 0.22F}},
        Vertex{{-half_extent, 0.0F, -half_extent}, {0, 1, 0}, {0.16F, 0.18F, 0.22F}},
    };
    const std::vector<uint32_t> indices = {0U, 1U, 2U, 0U, 2U, 3U};

    return GpuMesh{uploadVertices(device, std::vector<Vertex>(vertices.begin(), vertices.end())),
                   uploadIndices(device, indices), static_cast<uint32_t>(indices.size())};
}

} // namespace resonate::renderer
