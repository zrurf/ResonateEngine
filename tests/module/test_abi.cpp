#include <catch2/catch_all.hpp>

#include <resonate/module/module.hpp>

using resonate::Id;

TEST_CASE("distinct capability names produce distinct ids", "[module][abi]")
{
    REQUIRE(Id("Resonate.Window") != Id("Resonate.Input"));
    REQUIRE(Id("Resonate.Render.Device") != Id("Resonate.Render.Device.V2"));
}

TEST_CASE("the ABI version is encoded major-first", "[module][abi]")
{
    REQUIRE((RESONATE_ABI_VERSION >> 16) == RESONATE_ABI_VERSION_MAJOR);
    REQUIRE((RESONATE_ABI_VERSION & 0xFFFF) == RESONATE_ABI_VERSION_MINOR);
}

TEST_CASE("status codes are distinct", "[module][abi]")
{
    constexpr unsigned codes[] = {
        RESONATE_OK,        RESONATE_E_UNSUPPORTED, RESONATE_E_INVALID,     RESONATE_E_MISSING,
        RESONATE_E_VERSION, RESONATE_E_STATE,       RESONATE_E_UNAVAILABLE, RESONATE_E_INTERNAL};

    for (unsigned a = 0; a < sizeof(codes) / sizeof(codes[0]); ++a)
    {
        for (unsigned b = a + 1; b < sizeof(codes) / sizeof(codes[0]); ++b)
        {
            REQUIRE(codes[a] != codes[b]);
        }
    }

    REQUIRE(RESONATE_OK == 0u);
}
