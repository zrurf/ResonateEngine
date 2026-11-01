#include <catch2/catch_all.hpp>

#include <string>

#include <resonate/asset/identity.h>

/*
 * Asset identity: the GUID a sidecar carries is what a reference is made of,
 * and a products manifest is how a cook maps an id back to bytes. The id's
 * sub id is what keeps two products of one source apart, so a manifest that
 * names an id twice is broken data.
 */

namespace
{

using resonate::asset::AssetId;
using resonate::asset::Identity;

/* A fixed identity: 0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2. */
AssetId fixedId()
{
    AssetId id;
    REQUIRE(resonate::asset::parseGuid("0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2", id));
    id.sub_id = 1U;
    return id;
}

} // namespace

TEST_CASE("asset identity: a GUID round trips through its canonical form", "[asset]")
{
    const AssetId id = fixedId();
    CHECK(resonate::asset::formatGuid(id) == "0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2");

    /* The compact form parses the same: dashes are optional. A GUID carries no
       sub id, so the parsed one stays null until the reference sets it. */
    AssetId compact;
    REQUIRE(resonate::asset::parseGuid("0197d6e48a2f7c319b405d6e7f8091a2", compact));
    compact.sub_id = id.sub_id;
    CHECK(compact == id);

    /* Wrong length, bad digits and the null id are refused. A GUID has exactly
       two 16-digit halves; a longer string must not silently keep the last
       group. */
    AssetId broken = id;
    CHECK_FALSE(resonate::asset::parseGuid("0197d6e4-8a2f-7c31-9b40-5d6e7f8091a", broken));
    CHECK_FALSE(resonate::asset::parseGuid("0197d6e4-8a2f-7c31-9b40-5d6e7f8091zz", broken));
    CHECK_FALSE(resonate::asset::parseGuid("", broken));
    CHECK_FALSE(resonate::asset::parseGuid("0197d6e48a2f7c319b405d6e7f8091a201", broken));
    CHECK_FALSE(resonate::asset::parseGuid(
        "0197d6e48a2f7c319b405d6e7f8091a20197d6e48a2f7c31", broken));

    AssetId null_id;
    CHECK_FALSE(null_id.valid());
    CHECK(id.valid());
}

TEST_CASE("asset identity: a products manifest maps ids to paths", "[asset]")
{
    const char* const manifest = R"toml(
# Written by the cook; source and fingerprint are its bookkeeping.
[[product]]
source = "checker.ppm"
guid = "0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2"
fingerprint = "1a2b3c4d5e6f7081"
sub_id = 1
path = "checker.ktx2"

[[product]]
source = "checker.ppm"
guid = "0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2"
fingerprint = "1a2b3c4d5e6f7081"
sub_id = 2
path = "checker.etc1.ktx2"
)toml";
    Identity identity;
    std::string error;
    const bool loaded = identity.loadProducts(manifest, error);
    INFO("loaded=" << loaded << " error=" << error);
    REQUIRE(loaded);
    CHECK(identity.size() == 2U);

    AssetId first = fixedId();
    AssetId second = fixedId();
    second.sub_id = 2U;
    CHECK(identity.locate(first).value_or("") == "checker.ktx2");
    CHECK(identity.locate(second).value_or("") == "checker.etc1.ktx2");

    /* A sub id nobody cooked, and the null id, answer nothing. */
    AssetId third = fixedId();
    third.sub_id = 3U;
    CHECK_FALSE(identity.locate(third).has_value());
    CHECK_FALSE(identity.locate(AssetId{}).has_value());
}

TEST_CASE("asset identity: a broken manifest is refused with a reason", "[asset]")
{
    struct Case
    {
        const char* manifest;
        const char* why;
    };
    const Case cases[] = {
        {"not toml at all [[", "does not parse"},
        {"[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\nsub_id = 1\n",
         "needs a guid, a positive sub_id and a path"},
        {"[[product]]\nsub_id = 1\npath = \"checker.ktx2\"\n",
         "needs a guid, a positive sub_id and a path"},
        {"[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\nsub_id = 0\n"
         "path = \"checker.ktx2\"\n",
         "needs a guid, a positive sub_id and a path"},
        {"[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\nsub_id = 1\n"
         "path = \"checker.ktx2\"\n\n"
         "[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\nsub_id = 1\n"
         "path = \"again.ktx2\"\n",
         "twice"},
        /* A valid entry before the bad one must not be committed either. */
        {"[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\nsub_id = 1\n"
         "path = \"ok.ktx2\"\n\n[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\n"
         "sub_id = 0\npath = \"bad.ktx2\"\n",
         "needs a guid, a positive sub_id and a path"},
    };
    for (const Case& scenario : cases)
    {
        INFO("manifest: " << scenario.manifest);
        Identity identity;
        std::string error;
        CHECK_FALSE(identity.loadProducts(scenario.manifest, error));
        CHECK(error.find(scenario.why) != std::string::npos);
        CHECK(identity.size() == 0U);
        CHECK_FALSE(identity.locate(fixedId()).has_value());
    }
}

TEST_CASE("asset identity: the products iterate in manifest order", "[asset]")
{
    const char* const manifest = R"toml(
[[product]]
guid = "0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2"
sub_id = 1
path = "first.ktx2"

[[product]]
guid = "0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2"
sub_id = 2
path = "second.ktx2"
)toml";
    resonate::asset::Identity identity;
    std::string error;
    REQUIRE(identity.loadProducts(manifest, error));
    REQUIRE(identity.size() == 2U);
    const auto* first = identity.at(0U);
    const auto* second = identity.at(1U);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->path == "first.ktx2");
    CHECK(first->id.sub_id == 1U);
    CHECK(second->path == "second.ktx2");
    CHECK(second->id.sub_id == 2U);
    CHECK(identity.at(2U) == nullptr);
}
