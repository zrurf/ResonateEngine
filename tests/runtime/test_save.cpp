#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <resonate/application/save.h>

namespace
{

using resonate::Span;
using resonate::save::SaveFile;

SaveFile makeSave(std::uint32_t version, const std::string& text)
{
    SaveFile file;
    file.version = version;
    file.payload.resize(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        file.payload[index] = std::byte{static_cast<std::uint8_t>(text[index])};
    }
    return file;
}

std::string textOf(const std::vector<std::byte>& payload)
{
    return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

struct Root
{
    std::filesystem::path path;

    explicit Root(const char* name)
        : path(std::filesystem::temp_directory_path() / ("resonate_save_" + std::string(name)))
    {
        std::error_code code;
        std::filesystem::remove_all(path, code);
        std::filesystem::create_directories(path, code);
    }

    ~Root()
    {
        std::error_code code;
        std::filesystem::remove_all(path, code);
    }

    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;

    [[nodiscard]] std::string name() const
    {
        return path.string();
    }
};

} // namespace

TEST_CASE("save: the envelope round trips and rejects damage", "[save]")
{
    const SaveFile file = makeSave(3U, "the payload");
    const std::vector<std::byte> bytes = resonate::save::encodeSave(file);
    REQUIRE(bytes.size() > file.payload.size());

    SaveFile decoded;
    std::string error;
    REQUIRE(resonate::save::decodeSave(Span<const std::byte>(bytes.data(), bytes.size()), decoded,
                                       error));
    CHECK(decoded.version == 3U);
    CHECK(decoded.payload == file.payload);

    SECTION("a changed byte is corrupt")
    {
        std::vector<std::byte> damaged = bytes;
        damaged[20] = std::byte{static_cast<std::uint8_t>(
            std::to_integer<std::uint8_t>(damaged[20]) ^ 0x01U)};
        CHECK_FALSE(resonate::save::decodeSave(
            Span<const std::byte>(damaged.data(), damaged.size()), decoded, error));
        CHECK(error.find("corrupt") != std::string::npos);
    }
    SECTION("a truncated file is refused")
    {
        CHECK_FALSE(resonate::save::decodeSave(
            Span<const std::byte>(bytes.data(), bytes.size() - 1U), decoded, error));
    }
    SECTION("another file is refused")
    {
        const char* not_a_save = "ordinary text";
        CHECK_FALSE(resonate::save::decodeSave(
            Span<const std::byte>(reinterpret_cast<const std::byte*>(not_a_save),
                                  std::strlen(not_a_save)),
            decoded, error));
    }
}

TEST_CASE("save: a store replaces files atomically and reads them back", "[save]")
{
    Root root("store");
    resonate::save::SaveStore store(root.name());

    SaveFile first = makeSave(1U, "first save");
    std::string error;
    REQUIRE(store.write("slot1", first, error));
    const std::optional<std::string> path = store.pathOf("slot1");
    REQUIRE(path.has_value());
    CHECK(std::filesystem::path(*path).extension() == ".sav");

    SaveFile loaded;
    REQUIRE(store.read("slot1", loaded, error));
    CHECK(textOf(loaded.payload) == "first save");

    SaveFile second = makeSave(2U, "a longer second save");
    REQUIRE(store.write("slot1", second, error));
    REQUIRE(store.read("slot1", loaded, error));
    CHECK(loaded.version == 2U);
    CHECK(textOf(loaded.payload) == "a longer second save");
    /* The temporary never survives a successful write. */
    CHECK_FALSE(std::filesystem::exists(*path + ".tmp"));

    REQUIRE(store.remove("slot1", error));
    CHECK_FALSE(store.read("slot1", loaded, error));
    /* Removing a save that is not there is a no-op. */
    CHECK(store.remove("slot1", error));
}

TEST_CASE("save: names cannot escape the save directory", "[save]")
{
    Root root("names");
    resonate::save::SaveStore store(root.name());
    const SaveFile file = makeSave(1U, "x");
    std::string error;
    for (const char* name : {"", "../escape", "a/b", "a\\b", ".hidden", "a..b"})
    {
        INFO("name: " << name);
        CHECK_FALSE(store.pathOf(name).has_value());
        CHECK_FALSE(store.write(name, file, error));
    }
    /* A plain stem still works. */
    CHECK(store.write("slot-1_ok", file, error));
}
