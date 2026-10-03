#ifndef RESONATE_CORE_MATH_H
#define RESONATE_CORE_MATH_H

#include <cmath>

namespace resonate
{

struct Vec2
{
    float x = 0.0F;
    float y = 0.0F;
};

struct Vec3
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Vec4
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
};

/* Row-major. */
struct Mat4
{
    float m[4][4] = {};
};

struct Quat
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

constexpr Vec3 operator+(const Vec3& a, const Vec3& b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 operator-(const Vec3& a, const Vec3& b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 operator*(const Vec3& v, float s) noexcept
{
    return {v.x * s, v.y * s, v.z * s};
}

constexpr bool operator==(const Vec3& a, const Vec3& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

constexpr bool operator!=(const Vec3& a, const Vec3& b) noexcept
{
    return !(a == b);
}

constexpr float dot(const Vec3& a, const Vec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(const Vec3& a, const Vec3& b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr float lengthSquared(const Vec3& v) noexcept
{
    return dot(v, v);
}

/* Summed in double: a float square overflows at around 1e19, which would leave a
   long vector with a zero direction instead of a unit one. */
inline float length(const Vec3& v) noexcept
{
    const double x = static_cast<double>(v.x);
    const double y = static_cast<double>(v.y);
    const double z = static_cast<double>(v.z);
    return static_cast<float>(std::sqrt((x * x) + (y * y) + (z * z)));
}

/* Returns zero for a zero-length input, never NaN. */
inline Vec3 normalize(const Vec3& v) noexcept
{
    const float len = length(v);
    return len > 0.0F ? v * (1.0F / len) : Vec3{0.0F, 0.0F, 0.0F};
}

inline Mat4 identity() noexcept
{
    Mat4 result;
    for (int i = 0; i < 4; ++i)
    {
        result.m[i][i] = 1.0F;
    }
    return result;
}

/* Row-major storage with column vectors: (A * B) applies B first, and a
   transform's translation lives in the last column (m[0][3], m[1][3], m[2][3]).
   Q21's left-handed axes are a reading of the coordinates, not a different
   matrix convention; camera-facing conventions land with the render work. */
constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept
{
    Mat4 result;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k)
            {
                sum += a.m[row][k] * b.m[k][column];
            }
            result.m[row][column] = sum;
        }
    }
    return result;
}

/* w = 1, no perspective divide: this is the point transform of an affine
   matrix, which is what transforms and their composition produce. */
constexpr Vec3 transformPoint(const Mat4& m, const Vec3& v) noexcept
{
    return {m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3],
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3],
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3]};
}

/* Translation * rotation * scale, the order the hierarchy composes: a point is
   scaled, then rotated, then translated. The quaternion is expected to be a
   unit one; a non-unit one scales the frame as a side effect. */
constexpr Mat4 composeTrs(const Vec3& translation, const Quat& rotation, const Vec3& scale) noexcept
{
    const float x = rotation.x;
    const float y = rotation.y;
    const float z = rotation.z;
    const float w = rotation.w;

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    Mat4 result;
    result.m[0][0] = (1.0F - 2.0F * (yy + zz)) * scale.x;
    result.m[1][0] = (2.0F * (xy + wz)) * scale.x;
    result.m[2][0] = (2.0F * (xz - wy)) * scale.x;
    result.m[3][0] = 0.0F;

    result.m[0][1] = (2.0F * (xy - wz)) * scale.y;
    result.m[1][1] = (1.0F - 2.0F * (xx + zz)) * scale.y;
    result.m[2][1] = (2.0F * (yz + wx)) * scale.y;
    result.m[3][1] = 0.0F;

    result.m[0][2] = (2.0F * (xz + wy)) * scale.z;
    result.m[1][2] = (2.0F * (yz - wx)) * scale.z;
    result.m[2][2] = (1.0F - 2.0F * (xx + yy)) * scale.z;
    result.m[3][2] = 0.0F;

    result.m[0][3] = translation.x;
    result.m[1][3] = translation.y;
    result.m[2][3] = translation.z;
    result.m[3][3] = 1.0F;
    return result;
}

} // namespace resonate

#endif /* RESONATE_CORE_MATH_H */
