#include <catch2/catch_all.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <resonate/asset/identity.h>
#include <resonate/asset/meta.h>

namespace
{

constexpr const char* kMinimal =
    "# Resonate asset identity; references use the GUID. Do not edit.\n"
    "guid = \"15cc75a0-14ca-4c8e-b752-bf5ac010bc9a\"\n";

constexpr const char* kRich =
    "# Resonate asset identity; references use the GUID. Do not edit.\n"
    "guid = \"15cc75a0-14ca-4c8e-b752-bf5ac010bc9a\"\n"
    "importer = \"texture.ppm\"\n"
    "source_hash = \"b2f0c20ebf365c80b2f0c20ebf365c80b2f0c20ebf365c80b2f0c20ebf365c80\"\n";

} // namespace

TEST_CASE("asset meta: a minimal sidecar round trips byte for byte", "[asset][meta]")
{
    resonate::asset::AssetMeta meta;
    std::string error;
    REQUIRE(resonate::asset::parseMeta(kMinimal, meta, error));
    CHECK(meta.importer.empty());
    CHECK(meta.source_hash.empty());
    CHECK(resonate::asset::formatGuid(meta.id) == "15cc75a0-14ca-4c8e-b752-bf5ac010bc9a");
    CHECK(resonate::asset::formatMeta(meta) == kMinimal);
}

TEST_CASE("asset meta: importer and source hash are carried", "[asset][meta]")
{
    resonate::asset::AssetMeta meta;
    std::string error;
    REQUIRE(resonate::asset::parseMeta(kRich, meta, error));
    CHECK(meta.importer == "texture.ppm");
    CHECK(meta.source_hash.size() == 64U);
    CHECK(resonate::asset::formatMeta(meta) == kRich);

    /* An upper-case hash normalizes, and unknown keys from a newer writer are
       ignored. */
    std::string upper = kRich;
    const std::size_t hash_at = upper.find("source_hash = \"") + 15U;
    for (std::size_t index = hash_at; index < upper.size() && upper[index] != '"'; ++index)
    {
        if (upper[index] >= 'a' && upper[index] <= 'f')
        {
            upper[index] = static_cast<char>(upper[index] - 'a' + 'A');
        }
    }
    upper += "future_key = true\n";
    REQUIRE(resonate::asset::parseMeta(upper, meta, error));
    CHECK(meta.source_hash ==
          "b2f0c20ebf365c80b2f0c20ebf365c80b2f0c20ebf365c80b2f0c20ebf365c80");
}

TEST_CASE("asset meta: malformed sidecars are refused", "[asset][meta]")
{
    resonate::asset::AssetMeta meta;
    std::string error;
    CHECK_FALSE(resonate::asset::parseMeta("not toml [", meta, error));
    CHECK_FALSE(resonate::asset::parseMeta("guid = \"\"\n", meta, error));
    CHECK_FALSE(resonate::asset::parseMeta(
        "guid = \"00000000-0000-0000-0000-000000000000\"\n", meta, error));
    CHECK_FALSE(resonate::asset::parseMeta(
        "guid = \"15cc75a0-14ca-4c8e-b752-bf5ac010bc9a\"\nimporter = 3\n", meta, error));
    CHECK_FALSE(resonate::asset::parseMeta(
        "guid = \"15cc75a0-14ca-4c8e-b752-bf5ac010bc9a\"\nsource_hash = \"xyz\"\n", meta,
        error));
}

TEST_CASE("asset meta: a generated GUID is version 7 and unique", "[asset][meta]")
{
    const resonate::asset::AssetId first = resonate::asset::generateGuid();
    const resonate::asset::AssetId second = resonate::asset::generateGuid();
    CHECK((first.guid_low != second.guid_low || first.guid_high != second.guid_high));

    const std::string text = resonate::asset::formatGuid(first);
    REQUIRE(text.size() == 36U);
    /* The version nibble follows the third dash, the variant the fourth. */
    CHECK(text[14] == '7');
    CHECK((text[19] == '8' || text[19] == '9' || text[19] == 'a' || text[19] == 'b'));

    resonate::asset::AssetId parsed;
    REQUIRE(resonate::asset::parseGuid(text, parsed));
    CHECK(parsed.guid_low == first.guid_low);
    CHECK(parsed.guid_high == first.guid_high);
}

TEST_CASE("asset meta: a sidecar file writes and reads", "[asset][meta]")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "resonate_meta_test";
    std::error_code code;
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root, code);
    const std::string path = (root / "thing.bin.meta").string();

    resonate::asset::AssetMeta meta;
    meta.id = resonate::asset::generateGuid();
    meta.importer = "texture.ppm";
    meta.source_hash.assign(64U, 'a');
    std::string error;
    REQUIRE(resonate::asset::writeMetaFile(path, meta, error));

    resonate::asset::AssetMeta loaded;
    REQUIRE(resonate::asset::readMetaFile(path, loaded, error));
    CHECK(loaded.id.guid_low == meta.id.guid_low);
    CHECK(loaded.id.guid_high == meta.id.guid_high);
    CHECK(loaded.importer == meta.importer);
    CHECK(loaded.source_hash == meta.source_hash);

    CHECK_FALSE(resonate::asset::readMetaFile((root / "missing.meta").string(), loaded, error));
    std::filesystem::remove_all(root, code);
}
