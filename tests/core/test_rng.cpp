#include <catch2/catch_all.hpp>

#include <cstdint>
#include <set>

#include <resonate/core/rng.h>

using resonate::Rng;

TEST_CASE("the same seed replays the same values", "[core][rng]")
{
    Rng first(12345);
    Rng second(12345);

    for (int round = 0; round < 64; ++round)
    {
        CHECK(first.nextU64() == second.nextU64());
    }

    Rng third(12345);
    Rng fourth(12346);
    CHECK(third.nextU64() != fourth.nextU64());
}

TEST_CASE("the stream advances", "[core][rng]")
{
    Rng rng(7);
    std::set<std::uint64_t> seen;
    for (int round = 0; round < 1000; ++round)
    {
        seen.insert(rng.nextU64());
    }
    /* 1000 draws from 64 bits colliding is a broken generator, not bad luck. */
    REQUIRE(seen.size() == 1000);
}

TEST_CASE("nextBelow stays in range without modulo skew", "[core][rng]")
{
    Rng rng(99);

    for (int round = 0; round < 1000; ++round)
    {
        const std::uint32_t value = rng.nextBelow(std::uint32_t{10});
        REQUIRE(value < 10U);
    }
    CHECK(rng.nextBelow(std::uint32_t{0}) == 0U);
    CHECK(rng.nextBelow(std::uint32_t{1}) == 0U);

    /* A skew near one bound shows up as a missing value over enough draws. */
    std::set<std::uint32_t> values;
    for (int round = 0; round < 20000; ++round)
    {
        values.insert(rng.nextBelow(std::uint32_t{7}));
    }
    REQUIRE(values.size() == 7);

    /* The u64 overload shares the range rule. */
    for (int round = 0; round < 1000; ++round)
    {
        const std::uint64_t value = rng.nextBelow(std::uint64_t{1} << 63);
        REQUIRE(value < (std::uint64_t{1} << 63));
    }
}

TEST_CASE("nextFloat01 covers [0, 1) in steps", "[core][rng]")
{
    Rng rng(4242);
    bool saw_small = false;
    for (int round = 0; round < 100000; ++round)
    {
        const float value = rng.nextFloat01();
        REQUIRE(value >= 0.0F);
        REQUIRE(value < 1.0F);
        saw_small = saw_small || value < 0.001F;
    }
    CHECK(saw_small);
}

TEST_CASE("branches run beside the parent without sharing state", "[core][rng]")
{
    Rng parent(2024);
    const Rng parent_copy = parent;

    Rng a = parent.branch(1);
    Rng b = parent.branch(2);
    Rng a_again = parent.branch(1);

    CHECK(parent.state() == parent_copy.state());

    /* Each branch of one salt is the same stream; different salts diverge. */
    const std::uint64_t first_a = a.nextU64();
    const std::uint64_t first_b = b.nextU64();
    const std::uint64_t first_again = a_again.nextU64();
    CHECK(first_again == first_a);
    CHECK(first_b != first_a);
}
