/* Frame math the renderer owns: a projection for the device's clip space and
 * the inverse of a rigid transform. */

#include <cmath>

#include <resonate/core/math.h>

namespace resonate::renderer
{

/* Left-handed perspective for Vulkan's clip space: y is flipped (NDC points
   down), depth spans [0, 1]. Column vectors, row-major storage, like every
   matrix the engine passes to a shader: w_clip becomes the view-space z, so
   the perspective divide is by the distance forward. */
inline Mat4 perspectiveClip(float fov_y_radians, float aspect, float near_z, float far_z) noexcept
{
    const float height = 1.0F / tanf(fov_y_radians * 0.5F);
    const float width = height / aspect;
    const float range = far_z / (far_z - near_z);

    Mat4 result{};
    result.m[0][0] = width;
    result.m[1][1] = -height;
    result.m[2][2] = range;
    result.m[2][3] = -near_z * range;
    result.m[3][2] = 1.0F;
    return result;
}

/* Left-handed look-at: the camera sits at `eye` looking along `forward` (the
   engine's +z convention), `up` orients the roll. The rows are the camera
   basis vectors, which inverse-rotates world points into view space. */
inline Mat4 lookAtLh(const Vec3& eye, const Vec3& forward, const Vec3& up) noexcept
{
    const Vec3 z = normalize(forward);
    const Vec3 x = normalize(cross(up, z));
    const Vec3 y = cross(z, x);
    Mat4 result{};
    result.m[0][0] = x.x;
    result.m[0][1] = x.y;
    result.m[0][2] = x.z;
    result.m[1][0] = y.x;
    result.m[1][1] = y.y;
    result.m[1][2] = y.z;
    result.m[2][0] = z.x;
    result.m[2][1] = z.y;
    result.m[2][2] = z.z;
    result.m[0][3] = -dot(x, eye);
    result.m[1][3] = -dot(y, eye);
    result.m[2][3] = -dot(z, eye);
    result.m[3][3] = 1.0F;
    return result;
}

/* Left-handed orthographic for the light's view, depth mapped to [0, 1]. */
inline Mat4 orthographicClip(float width, float height, float near_z, float far_z) noexcept
{
    Mat4 result{};
    result.m[0][0] = 2.0F / width;
    result.m[1][1] = -2.0F / height;
    result.m[2][2] = 1.0F / (far_z - near_z);
    result.m[2][3] = -near_z / (far_z - near_z);
    result.m[3][3] = 1.0F;
    return result;
}

/* The view matrix of a camera whose world transform is rigid (rotation orthonormal,
   scale one): the rotation transposes, the translation negates through it. */
constexpr Mat4 rigidInverse(const Mat4& world) noexcept
{
    Mat4 result{};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            result.m[row][column] = world.m[column][row];
        }
    }
    result.m[0][3] = -(result.m[0][0] * world.m[0][3] + result.m[0][1] * world.m[1][3] +
                       result.m[0][2] * world.m[2][3]);
    result.m[1][3] = -(result.m[1][0] * world.m[0][3] + result.m[1][1] * world.m[1][3] +
                       result.m[1][2] * world.m[2][3]);
    result.m[2][3] = -(result.m[2][0] * world.m[0][3] + result.m[2][1] * world.m[1][3] +
                       result.m[2][2] * world.m[2][3]);
    result.m[3][3] = 1.0F;
    return result;
}

} // namespace resonate::renderer
