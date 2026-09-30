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

} // namespace resonate

#endif /* RESONATE_CORE_MATH_H */
