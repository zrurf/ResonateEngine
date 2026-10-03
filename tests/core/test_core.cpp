#include <catch2/catch_all.hpp>

#include <cmath>

#include <resonate/core/math.h>
#include <resonate/core/span.h>

using resonate::Span;

TEST_CASE("a span reports its bounds", "[core][span]")
{
    int values[] = {1, 2, 3, 4};
    const Span<int> span(values);

    REQUIRE(span.size() == 4);
    REQUIRE(span.data() == values);
    REQUIRE_FALSE(span.empty());
    REQUIRE(span[2] == 3);
}

TEST_CASE("a default span is empty and safe to query", "[core][span]")
{
    const Span<int> span;

    REQUIRE(span.empty());
    REQUIRE(span.data() == nullptr);
}

TEST_CASE("a subspan clamps to the remaining length", "[core][span]")
{
    int values[] = {1, 2, 3, 4};
    const Span<int> span(values);

    REQUIRE(span.subspan(1, 2).size() == 2);
    REQUIRE(span.subspan(1, 2)[0] == 2);
    REQUIRE(span.subspan(3, 99).size() == 1);
    REQUIRE(span.subspan(4, 1).empty());
}

TEST_CASE("vector cross products follow the right-hand rule", "[core][math]")
{
    const resonate::Vec3 x{1.0F, 0.0F, 0.0F};
    const resonate::Vec3 y{0.0F, 1.0F, 0.0F};

    REQUIRE(resonate::dot(x, y) == 0.0F);
    REQUIRE(resonate::cross(x, y) == resonate::Vec3{0.0F, 0.0F, 1.0F});
}

TEST_CASE("normalising a zero vector yields zero rather than NaN", "[core][math]")
{
    const resonate::Vec3 zero{};

    REQUIRE(resonate::normalize(zero) == resonate::Vec3{0.0F, 0.0F, 0.0F});
    REQUIRE(resonate::length(resonate::Vec3{3.0F, 4.0F, 0.0F}) == 5.0F);
}

TEST_CASE("normalising a long vector still yields a direction", "[core][math]")
{
    /* The squared length of this one overflows a float, which used to leave the
       result at zero instead of a unit vector. */
    const resonate::Vec3 far{1.0e20F, 0.0F, 0.0F};
    const resonate::Vec3 unit = resonate::normalize(far);

    REQUIRE(unit == resonate::Vec3{1.0F, 0.0F, 0.0F});

    const float magnitude = resonate::length(resonate::Vec3{0.0F, 3.0e19F, 4.0e19F});
    REQUIRE(magnitude > 0.0F);
    REQUIRE(magnitude / 5.0e19F > 0.99F);
    REQUIRE(magnitude / 5.0e19F < 1.01F);
}

namespace
{

constexpr bool nearlyEqual(float left, float right) noexcept
{
    const float difference = left - right;
    return difference < 1.0e-5F && difference > -1.0e-5F;
}

constexpr bool nearlyEqual(const resonate::Vec3& left, const resonate::Vec3& right) noexcept
{
    return nearlyEqual(left.x, right.x) && nearlyEqual(left.y, right.y) &&
           nearlyEqual(left.z, right.z);
}

/* A rotation of `radians` about +Z: the quaternion half-angle form, which is
   also how a test builds one without pulling in an axis-angle helper. */
resonate::Quat aboutZ(float radians) noexcept
{
    return resonate::Quat{0.0F, 0.0F, std::sin(radians * 0.5F), std::cos(radians * 0.5F)};
}

} // namespace

TEST_CASE("identity multiplies and transforms as neutral", "[core][math]")
{
    const resonate::Mat4 identity = resonate::identity();
    const resonate::Vec3 point{1.0F, 2.0F, 3.0F};

    REQUIRE(nearlyEqual(resonate::transformPoint(identity, point), point));

    const resonate::Mat4 scaled = resonate::composeTrs(
        resonate::Vec3{4.0F, 5.0F, 6.0F}, resonate::Quat{}, resonate::Vec3{2.0F, 3.0F, 4.0F});
    const resonate::Mat4 product = identity * scaled;
    REQUIRE(nearlyEqual(resonate::transformPoint(product, point),
                        resonate::transformPoint(scaled, point)));
}

TEST_CASE("composeTrs scales, then rotates, then translates", "[core][math]")
{
    const resonate::Mat4 trs =
        resonate::composeTrs(resonate::Vec3{10.0F, 20.0F, 30.0F}, aboutZ(3.14159265F * 0.5F),
                             resonate::Vec3{2.0F, 2.0F, 2.0F});

    /* +X scaled by 2, rotated a quarter turn about +Z, then translated. */
    REQUIRE(nearlyEqual(resonate::transformPoint(trs, resonate::Vec3{1.0F, 0.0F, 0.0F}),
                        resonate::Vec3{10.0F, 22.0F, 30.0F}));

    /* The origin lands on the translation itself. */
    REQUIRE(nearlyEqual(resonate::transformPoint(trs, resonate::Vec3{}),
                        resonate::Vec3{10.0F, 20.0F, 30.0F}));
}

TEST_CASE("multiplying transforms applies the right one first", "[core][math]")
{
    const resonate::Mat4 parent =
        resonate::composeTrs(resonate::Vec3{5.0F, 0.0F, 0.0F}, aboutZ(3.14159265F * 0.5F),
                             resonate::Vec3{1.0F, 1.0F, 1.0F});
    const resonate::Mat4 local = resonate::composeTrs(
        resonate::Vec3{1.0F, 0.0F, 0.0F}, resonate::Quat{}, resonate::Vec3{2.0F, 2.0F, 2.0F});

    const resonate::Mat4 combined = parent * local;
    const resonate::Vec3 point{0.0F, 1.0F, 0.0F};
    REQUIRE(nearlyEqual(resonate::transformPoint(combined, point),
                        resonate::transformPoint(parent, resonate::transformPoint(local, point))));
    REQUIRE(
        nearlyEqual(resonate::transformPoint(combined, point), resonate::Vec3{3.0F, 1.0F, 0.0F}));
}
