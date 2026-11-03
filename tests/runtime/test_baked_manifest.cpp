#include <catch2/catch_test_macros.hpp>

/* Every TU in this binary must agree on toml++'s exception mode: the runtime
   builds it with exceptions off. */
#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

#include <resonate/asset/baked_manifest.h>
#include <resonate/asset/identity.h>
#include <resonate/core/allocator.h>
#include <resonate/pal/io.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace
{

/* This toml++ version's parse_result is the table itself when exceptions are
   on, and a wrapper with an explicit conversion when they are off. */
toml::table parseTable(const char* text)
{
    toml::parse_result result = toml::parse(text);
    if (!result)
    {
        return toml::table{};
    }
    return toml::table(result.table());
}

std::string readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/* A scratch directory beside the test binary: on the build drive, never the
   system one. */
std::filesystem::path scratch()
{
    std::filesystem::path directory =
        std::filesystem::path(resonate_pal_io_executable_directory()) / "baked-manifests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

} // namespace

TEST_CASE("baked manifests round-trip through the binary form", "[runtime][baked]")
{
    const toml::table source = parseTable(R"(
[resonate.module]
id = "resonate.render-vulkan"
name = "Render Vulkan"
version = "0.1.0"
abi = 1
summary = "Render device and frame submission."
provides = ["Resonate.Render.Device"]
requires = ["Resonate.Render.Canvas", "Resonate.Window"]
strict = true
ratio = 0.5

[[product]]
guid = "15cc75a0-14ca-4c8e-b752-bf5ac010bc9a"
sub_id = 1
path = "bob.rclip"

[[product]]
guid = "075a7fcb-478e-4962-b2ef-0b37cdf9641a"
sub_id = 2
path = "spin.rclip"
)");

    std::vector<std::uint8_t> baked;
    std::string error;
    REQUIRE(resonate::asset::bakeTable(source, baked, error));
    REQUIRE_FALSE(baked.empty());

    const std::string path =
        (scratch() / "round-trip.rman").string();
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(baked.data()),
                  static_cast<std::streamsize>(baked.size()));
    }

    toml::table loaded;
    INFO("baked read: " << error);
    REQUIRE(resonate::asset::loadBaked(path, loaded, error));

    const toml::table* module = loaded["resonate"]["module"].as_table();
    REQUIRE(module != nullptr);
    CHECK(module->at_path("id").value_or(std::string{}) == "resonate.render-vulkan");
    CHECK(module->at_path("abi").value_or<std::int64_t>(0) == 1);
    CHECK(module->at_path("strict").value_or(false));
    CHECK(module->at_path("ratio").value_or(0.0) == 0.5);
    const toml::array* provides = module->at_path("provides").as_array();
    REQUIRE(provides != nullptr);
    REQUIRE(provides->size() == 1U);
    CHECK(provides->get(0)->value_or(std::string{}) == "Resonate.Render.Device");

    const toml::array* products = loaded["product"].as_array();
    REQUIRE(products != nullptr);
    REQUIRE(products->size() == 2U);
    const toml::table* first = products->get(0)->as_table();
    REQUIRE(first != nullptr);
    CHECK(first->at_path("path").value_or(std::string{}) == "bob.rclip");
    const toml::table* second = products->get(1)->as_table();
    REQUIRE(second != nullptr);
    CHECK(second->at_path("sub_id").value_or<std::int64_t>(0) == 2);
}

TEST_CASE("an identity reads the baked twin of its manifest", "[runtime][baked]")
{
    const std::filesystem::path directory = scratch();
    const std::string toml = R"(
# Written by the packager; the runtime's asset identity reads it.
[[product]]
source = "animation/bob.rclip"
guid = "15cc75a0-14ca-4c8e-b752-bf5ac010bc9a"
fingerprint = "4623eedc67c1a38c06489c9a4e8d036eb2e916e55cc43113b8c77db0408b4ee7"
sub_id = 1
path = "bob.rclip"
)";

    /* Only the baked twin ships: loadFile on the manifest's own path finds it
       without the text form being present. */
    toml::table parsed = parseTable(toml.c_str());
    std::vector<std::uint8_t> baked;
    std::string error;
    REQUIRE(resonate::asset::bakeTable(parsed, baked, error));
    const std::string manifest = (directory / "products.toml").string();
    const std::string twin = resonate::asset::bakedTwinOf(manifest);
    CHECK(twin == (directory / "products.rman").string());
    {
        std::ofstream out(twin, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(baked.data()),
                  static_cast<std::streamsize>(baked.size()));
    }

    resonate::asset::Identity identity;
    REQUIRE(identity.loadFile(manifest, error));
    CHECK(identity.size() == 1U);
    const auto id = identity.idForPath("bob.rclip");
    REQUIRE(id.has_value());
    /* The halves follow parseGuid's convention; what matters is that the baked
       path yields the same id the text manifest does. */
    resonate::asset::AssetId expected{};
    REQUIRE(resonate::asset::parseGuid("15cc75a0-14ca-4c8e-b752-bf5ac010bc9a", expected));
    CHECK(id->guid_low == expected.guid_low);
    CHECK(id->guid_high == expected.guid_high);
    CHECK(id->sub_id == 1U);
}
