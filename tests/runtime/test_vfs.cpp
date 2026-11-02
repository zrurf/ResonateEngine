#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/asset/identity.h>
#include <resonate/asset/vfs.h>

namespace
{

using resonate::asset::AssetId;

constexpr const char* kManifest = R"toml(
[[product]]
guid = "0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2"
sub_id = 1
path = "first.bin"

[[product]]
guid = "123e4567-e89b-42d3-a456-426614174000"
sub_id = 2
path = "nested/second.bin"
)toml";

AssetId makeId(const char* guid, std::uint32_t sub_id)
{
    AssetId id;
    REQUIRE(resonate::asset::parseGuid(guid, id));
    id.sub_id = sub_id;
    return id;
}

void writeFile(const std::filesystem::path& path, std::string_view text)
{
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string textOf(const std::vector<std::byte>& data)
{
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

struct Store
{
    std::filesystem::path root;

    explicit Store(const char* name)
        : root(std::filesystem::temp_directory_path() / ("resonate_vfs_" + std::string(name)))
    {
        std::error_code code;
        std::filesystem::remove_all(root, code);
        std::filesystem::create_directories(root, code);
        writeFile(root / "products.toml", kManifest);
        writeFile(root / "first.bin", "first product");
        writeFile(root / "nested" / "second.bin", "second product");
    }

    ~Store()
    {
        std::error_code code;
        std::filesystem::remove_all(root, code);
    }

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    [[nodiscard]] std::string manifest() const
    {
        return (root / "products.toml").string();
    }
};

} // namespace

TEST_CASE("vfs: a directory store resolves ids to files", "[asset][vfs]")
{
    Store store("dir");
    std::string error;
    const std::unique_ptr<resonate::asset::DirectoryVfs> vfs =
        resonate::asset::DirectoryVfs::open(store.manifest(), error);
    REQUIRE(vfs != nullptr);
    CHECK(vfs->products().size() == 2U);

    const AssetId first = makeId("0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2", 1U);
    std::vector<std::byte> bytes;
    REQUIRE(vfs->read(first, bytes, error));
    CHECK(textOf(bytes) == "first product");

    const std::optional<std::string> located = vfs->locate(first);
    REQUIRE(located.has_value());
    CHECK(std::filesystem::path(*located).filename().string() == "first.bin");

    const AssetId second = makeId("123e4567-e89b-42d3-a456-426614174000", 2U);
    REQUIRE(vfs->read(second, bytes, error));
    CHECK(textOf(bytes) == "second product");
}

TEST_CASE("vfs: unknown ids and missing files answer with a reason", "[asset][vfs]")
{
    Store store("miss");
    std::string error;
    const std::unique_ptr<resonate::asset::DirectoryVfs> vfs =
        resonate::asset::DirectoryVfs::open(store.manifest(), error);
    REQUIRE(vfs != nullptr);

    /* The same source, a sub id nobody cooked. */
    AssetId unknown = makeId("0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2", 9U);
    std::vector<std::byte> bytes;
    CHECK_FALSE(vfs->read(unknown, bytes, error));
    CHECK(error.find("no product") != std::string::npos);
    CHECK_FALSE(vfs->locate(unknown).has_value());

    /* A manifest entry whose file is gone names the path it tried. */
    std::error_code code;
    std::filesystem::remove(store.root / "first.bin", code);
    REQUIRE_FALSE(code);
    const AssetId first = makeId("0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2", 1U);
    CHECK_FALSE(vfs->read(first, bytes, error));
    CHECK(error.find("cannot read") != std::string::npos);
    CHECK(error.find("first.bin") != std::string::npos);
}

TEST_CASE("vfs: a missing manifest fails the open", "[asset][vfs]")
{
    std::string error;
    CHECK(resonate::asset::DirectoryVfs::open("no/such/products.toml", error) == nullptr);
    CHECK(error.find("cannot open") != std::string::npos);
}
