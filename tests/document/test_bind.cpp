#include <catch2/catch_all.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include <resonate/document_bind/bind.h>
#include <resonate/dom/schema.h>

#include "resonate.gameplay/nodes.gen.h"

namespace
{

const resonate::dom::ComponentFieldDesc* findField(const resonate::dom::ComponentDesc& component,
                                                   const char* name)
{
    for (const resonate::dom::ComponentFieldDesc& field : component.fields)
    {
        if (std::string_view(field.name) == name)
        {
            return &field;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("document bind: a named enum field takes a value name", "[document][bind]")
{
    const resonate::dom::ComponentDesc& health =
        resonate::gameplay::generated::kComponentDescs[0];
    REQUIRE(std::string_view(health.name) == "Health");
    const resonate::dom::ComponentFieldDesc* flags = findField(health, "flags");
    REQUIRE(flags != nullptr);
    REQUIRE(flags->enum_values != nullptr);

    resonate::gameplay::Health value;
    std::string error;
    REQUIRE(resonate::dom_bind::parseField(*flags, "Invulnerable", &value, error));
    CHECK(value.flags == resonate::gameplay::HealthFlags::Invulnerable);
    REQUIRE(resonate::dom_bind::parseField(*flags, "Poisoned", &value, error));
    CHECK(value.flags == resonate::gameplay::HealthFlags::Poisoned);

    CHECK_FALSE(resonate::dom_bind::parseField(*flags, "Bogus", &value, error));
    CHECK(error.find("enum") != std::string::npos);
}

TEST_CASE("document bind: integer fields reject signs where they do not belong", "[document][bind]")
{
    resonate::dom::ComponentFieldDesc byte{"v", "u8", 0U, 1U};
    std::uint8_t narrow = 0U;
    std::string error;
    REQUIRE(resonate::dom_bind::parseField(byte, "255", &narrow, error));
    CHECK(narrow == 255U);
    CHECK_FALSE(resonate::dom_bind::parseField(byte, "-1", &narrow, error));
    CHECK_FALSE(resonate::dom_bind::parseField(byte, "256", &narrow, error));

    resonate::dom::ComponentFieldDesc wide{"v", "u64", 0U, 8U};
    std::uint64_t large = 0U;
    REQUIRE(resonate::dom_bind::parseField(wide, "18446744073709551615", &large, error));
    CHECK(large == UINT64_MAX);
    CHECK_FALSE(resonate::dom_bind::parseField(wide, "-1", &large, error));
}
