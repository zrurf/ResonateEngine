#include <catch2/catch_all.hpp>

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
