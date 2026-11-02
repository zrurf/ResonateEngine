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

/* Writes the manifest into a throwaway directory and names it, for cases that
   need custom manifest text. */
std::string storeOfManifest(const std::string& text)
{
    static const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "resonate_vfs_manifest";
    std::error_code code;
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root, code);
    writeFile(root / "products.toml", text);
    return (root / "products.toml").string();
}

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

TEST_CASE("vfs: a CAS store reads a product through its chunk manifest", "[asset][vfs]")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "resonate_vfs_cas";
    std::error_code code;
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root, code);

    resonate::asset::ContentKey key{};
    for (std::size_t index = 0; index < key.size(); ++index)
    {
        key[index] = static_cast<std::uint8_t>(index * 3U + 1U);
    }
    const std::string content = "a product that lives in chunks";
    resonate::asset::CasManifest chunks;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(
        root.string(), key,
        resonate::Span<const std::byte>(reinterpret_cast<const std::byte*>(content.data()),
                                        content.size()),
        chunks, error));
    writeFile(root / "product.cas.toml", resonate::asset::formatManifest(chunks));
    writeFile(root / "products.toml",
              "[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\n"
              "sub_id = 1\npath = \"product.cas.toml\"\n");

    const AssetId id = makeId("0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2", 1U);
    const std::unique_ptr<resonate::asset::CasVfs> vfs =
        resonate::asset::CasVfs::open(root.string(), key, error);
    REQUIRE(vfs != nullptr);
    std::vector<std::byte> bytes;
    REQUIRE(vfs->read(id, bytes, error));
    CHECK(textOf(bytes) == content);

    /* The key is the salt: another one reaches the same files and rejects
       every chunk. */
    resonate::asset::ContentKey other = key;
    other[0] = static_cast<std::uint8_t>(other[0] ^ 0xFFU);
    const std::unique_ptr<resonate::asset::CasVfs> foreign =
        resonate::asset::CasVfs::open(root.string(), other, error);
    REQUIRE(foreign != nullptr);
    CHECK_FALSE(foreign->read(id, bytes, error));
    CHECK(error.find("corrupt") != std::string::npos);
    /* A failed read leaves the caller's bytes as they were. */
    CHECK(textOf(bytes) == content);

    std::filesystem::remove_all(root, code);
}

TEST_CASE("vfs: a product path may not leave the store", "[asset][vfs]")
{
    const char* const escapes[] = {
        R"(path = "C:/elsewhere/secret.bin")",
        R"(path = "/etc/passwd")",
        R"(path = "C:escape.bin")",
    };
    for (const char* const path_line : escapes)
    {
        const std::string manifest =
            std::string("[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\n"
                        "sub_id = 1\n") +
            path_line + "\n";
        std::string error;
        const std::unique_ptr<resonate::asset::DirectoryVfs> vfs =
            resonate::asset::DirectoryVfs::open(storeOfManifest(manifest), error);
        CHECK(vfs == nullptr);
        CHECK(error.find("inside the store") != std::string::npos);
    }

    /* Climbing paths stay legal: the development manifest names the source
       tree the build directory sits inside. */
    const std::string climbing =
        std::string("[[product]]\nguid = \"0197d6e4-8a2f-7c31-9b40-5d6e7f8091a2\"\n"
                    "sub_id = 1\npath = \"../../content/animation/bob.rclip\"\n");
    std::string error;
    const std::unique_ptr<resonate::asset::DirectoryVfs> climbing_vfs =
        resonate::asset::DirectoryVfs::open(storeOfManifest(climbing), error);
    CHECK(climbing_vfs != nullptr);
}
