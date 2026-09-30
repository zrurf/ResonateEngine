#include <catch2/catch_all.hpp>

#include <cstdint>

#include <resonate/module/abi.h>
#include <resonate/module/capability_id.hpp>

namespace
{

/* The published ids, pinned.
 *
 * Renaming a capability or changing the hash invalidates every id a module was
 * built against, so these literals are what makes such a change deliberate rather
 * than silent. They are XXH3-128 of the name, which is also what a manifest read
 * at run time goes through. */
struct KnownId
{
    const char* name;
    std::uint64_t lo;
    std::uint64_t hi;
};

constexpr KnownId KNOWN[] = {
    {"Resonate.Physics.World", 0x5B20A463262F8A6AULL, 0x58A68AB38F4AC0FEULL},
    {"Resonate.Window", 0x2E3A517109CC3F33ULL, 0xB97172C83A84CD8FULL},
    {"Resonate.Input", 0x217847FC8118263BULL, 0x8670A56DBFCEB5C8ULL},
    {"Resonate.Render.Canvas", 0x9214EC0015CEF17BULL, 0xFA0C1B5E17A5704EULL},
    {"Resonate.Render.Device", 0x5BC9871AADE6670CULL, 0xA32836CF8AB8C314ULL},
    {"Resonate.UI.Context", 0x511B46584F147679ULL, 0xBB57E077B0FCA181ULL},
    {"Resonate.Test.One", 0xA6FC6E1277EF7898ULL, 0xED9B073171CF9236ULL},
    {"", 0x6001C324468D497FULL, 0x99AA06D3014798D8ULL},
};

} // namespace

TEST_CASE("a capability id is its name under XXH3-128", "[module][abi]")
{
    for (const KnownId& known : KNOWN)
    {
        INFO("name: '" << known.name << "'");

        /* Both entry points, against the literal: the header one a plugin uses
           and the C one a manifest read goes through. Comparing them with each
           other would prove nothing now that they share one implementation. */
        REQUIRE(resonate::Id(known.name).value().lo == known.lo);
        REQUIRE(resonate::Id(known.name).value().hi == known.hi);

        const ResonateId from_c = resonate_id_make(known.name);
        REQUIRE(from_c.lo == known.lo);
        REQUIRE(from_c.hi == known.hi);
    }
}

TEST_CASE("a null name hashes as empty rather than crashing", "[module][abi]")
{
    const ResonateId from_null = resonate_id_make(nullptr);
    const ResonateId from_empty = resonate_id_make("");
    REQUIRE(from_null == from_empty);
}

TEST_CASE("trivially different names hash differently", "[module][abi]")
{
    REQUIRE(resonate_id_make("Resonate.Window") != resonate_id_make("Resonate.Input"));
    REQUIRE(resonate_id_make("Resonate.Render.Device") !=
            resonate_id_make("Resonate.Render.Device.V2"));
}
