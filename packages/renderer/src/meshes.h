/* The procedural meshes the demo draws until meshes load from assets: a cube
 * and a ground plane, vertex-colored, uploaded once per device. */

#include <resonate/render/device.h>

#include <cstdint>

namespace resonate::renderer
{

enum MeshId : std::uint32_t
{
    kMeshNone = 0,
    kMeshCube = 1,
    kMeshGround = 2,
};

struct GpuMesh
{
    ResonateRenderBuffer vertices{};
    ResonateRenderBuffer indices{};
    std::uint32_t index_count = 0;
};

/* One vertex stream: position, normal, color, texture coordinate, 44 bytes. */
struct Vertex
{
    float position[3];
    float normal[3];
    float color[3];
    float uv[2];
};

GpuMesh buildCube(ResonateRenderDevice& device);
GpuMesh buildGround(ResonateRenderDevice& device, float half_extent);

} // namespace resonate::renderer
