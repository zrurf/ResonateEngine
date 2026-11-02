#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <resonate/asset/cas.h>

namespace
{

using resonate::Span;
using resonate::asset::CasManifest;
using resonate::asset::ContentKey;

std::vector<std::byte> pattern(std::size_t size, std::uint32_t seed)
{
    std::vector<std::byte> data(size);
    std::uint32_t state = seed;
    for (std::byte& byte : data)
    {
        state = state * 1664525U + 1013904223U;
        byte = std::byte{static_cast<std::uint8_t>(state >> 24U)};
    }
    return data;
}

Span<const std::byte> view(const std::vector<std::byte>& data)
{
    return Span<const std::byte>(data.data(), data.size());
}

ContentKey keyA()
{
    ContentKey key{};
    for (std::size_t index = 0; index < key.size(); ++index)
    {
        key[index] = static_cast<std::uint8_t>(index * 7U + 1U);
    }
    return key;
}

/* A fresh store directory per test; it is removed with the fixture. */
struct Store
{
    std::filesystem::path root;

    explicit Store(const char* name)
        : root(std::filesystem::temp_directory_path() / ("resonate_cas_" + std::string(name)))
    {
        std::error_code code;
        std::filesystem::remove_all(root, code);
        std::filesystem::create_directories(root, code);
    }

    ~Store()
    {
        std::error_code code;
        std::filesystem::remove_all(root, code);
    }

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    [[nodiscard]] std::string path() const
    {
        return root.string();
    }
};

} // namespace

TEST_CASE("cas: content-defined chunks are stable and bounded", "[asset][cas]")
{
    const auto data = pattern(1024U * 1024U + 7U, 1U);
    const std::vector<Span<const std::byte>> chunks = resonate::asset::splitChunks(view(data));
    REQUIRE(chunks.size() > 1U);
    CHECK(chunks.front().data() == data.data());

    std::size_t total = 0U;
    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
        CHECK(chunks[index].size() > 0U);
        CHECK(chunks[index].size() <= resonate::asset::kChunkMaxBytes);
        if (index + 1U < chunks.size())
        {
            /* Only the final chunk may be short: every earlier cut happened at
               or after the minimum. */
            CHECK(chunks[index].size() >= resonate::asset::kChunkMinBytes);
        }
        total += chunks[index].size();
    }
    CHECK(total == data.size());

    /* The same bytes cut in the same places. */
    const std::vector<Span<const std::byte>> again = resonate::asset::splitChunks(view(data));
    REQUIRE(again.size() == chunks.size());
    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
        CHECK(again[index].size() == chunks[index].size());
    }
}

TEST_CASE("cas: a stored product reads back", "[asset][cas]")
{
    Store store("roundtrip");
    const auto data = pattern(700U * 1024U + 123U, 2U);
    CasManifest manifest;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(store.path(), keyA(), view(data), manifest, error));
    CHECK(manifest.total_bytes == data.size());
    REQUIRE_FALSE(manifest.chunks.empty());

    std::vector<std::byte> out;
    REQUIRE(resonate::asset::readBuffer(store.path(), keyA(), manifest, out, error));
    CHECK(out == data);
}

TEST_CASE("cas: a damaged chunk is caught by its hash", "[asset][cas]")
{
    Store store("corrupt");
    const auto data = pattern(400U * 1024U, 5U);
    CasManifest manifest;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(store.path(), keyA(), view(data), manifest, error));
    REQUIRE_FALSE(manifest.chunks.empty());

    const std::string path =
        resonate::asset::chunkPath(store.path(), manifest.chunks.front().hash);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(file.is_open());
        char byte = 0;
        file.read(&byte, 1);
        REQUIRE(file.good());
        byte = static_cast<char>(byte ^ 0x5A);
        file.seekp(0);
        file.write(&byte, 1);
        REQUIRE(file.good());
    }

    std::vector<std::byte> out;
    CHECK_FALSE(resonate::asset::readBuffer(store.path(), keyA(), manifest, out, error));
    CHECK(error.find("corrupt") != std::string::npos);
}

TEST_CASE("cas: an insertion keeps almost every chunk", "[asset][cas]")
{
    Store store("reuse");
    const ContentKey key = keyA();
    const auto first = pattern(4U * 1024U * 1024U, 7U);
    CasManifest first_manifest;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(store.path(), key, view(first), first_manifest, error));

    std::vector<std::byte> second = first;
    second.insert(second.begin() + 12345, std::byte{0x5AU});
    CasManifest second_manifest;
    REQUIRE(resonate::asset::storeBuffer(store.path(), key, view(second), second_manifest, error));

    std::set<std::string> known;
    for (const resonate::asset::ChunkRef& chunk : first_manifest.chunks)
    {
        known.insert(resonate::asset::formatDigest(chunk.hash));
    }
    std::size_t shared = 0U;
    for (const resonate::asset::ChunkRef& chunk : second_manifest.chunks)
    {
        shared += known.contains(resonate::asset::formatDigest(chunk.hash)) ? 1U : 0U;
    }
    INFO("v1 chunks=" << first_manifest.chunks.size() << " v2 chunks="
                      << second_manifest.chunks.size() << " shared=" << shared);
    CHECK(shared * 100U >= second_manifest.chunks.size() * 95U);

    /* Both versions still read back through the one store. */
    std::vector<std::byte> out;
    REQUIRE(resonate::asset::readBuffer(store.path(), key, first_manifest, out, error));
    CHECK(out == first);
    REQUIRE(resonate::asset::readBuffer(store.path(), key, second_manifest, out, error));
    CHECK(out == second);
}

TEST_CASE("cas: a different key reads nothing", "[asset][cas]")
{
    Store store("keys");
    const auto data = pattern(300U * 1024U, 9U);
    ContentKey other = keyA();
    other[0] = static_cast<std::uint8_t>(other[0] ^ 0xFFU);

    CasManifest stored;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(store.path(), keyA(), view(data), stored, error));
    CasManifest foreign;
    REQUIRE(resonate::asset::storeBuffer(store.path(), other, view(data), foreign, error));
    REQUIRE_FALSE(stored.chunks.empty());
    REQUIRE_FALSE(foreign.chunks.empty());
    CHECK(stored.chunks.front().hash != foreign.chunks.front().hash);

    std::vector<std::byte> out;
    CHECK_FALSE(resonate::asset::readBuffer(store.path(), other, stored, out, error));
    CHECK(error.find("corrupt") != std::string::npos);
}

TEST_CASE("cas: the manifest round trips and refuses broken data", "[asset][cas]")
{
    Store store("manifest");
    const auto data = pattern(200U * 1024U, 11U);
    CasManifest manifest;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(store.path(), keyA(), view(data), manifest, error));

    const std::string text = resonate::asset::formatManifest(manifest);
    CasManifest parsed;
    REQUIRE(resonate::asset::parseManifest(text, parsed, error));
    CHECK(parsed.total_bytes == manifest.total_bytes);
    REQUIRE(parsed.chunks.size() == manifest.chunks.size());
    for (std::size_t index = 0; index < parsed.chunks.size(); ++index)
    {
        CHECK(parsed.chunks[index].hash == manifest.chunks[index].hash);
        CHECK(parsed.chunks[index].size == manifest.chunks[index].size);
    }
    CHECK(resonate::asset::formatManifest(parsed) == text);

    const std::string digest = resonate::asset::formatDigest(manifest.chunks.front().hash);
    CHECK_FALSE(resonate::asset::parseManifest("total_bytes = 1\n[[chunk]]\nhash = \"" +
                                                   digest + "\"\nsize = 0\n",
                                               parsed, error));
    CHECK_FALSE(resonate::asset::parseManifest("total_bytes = 1\n[[chunk]]\nhash = \"" +
                                                   digest.substr(0U, 62U) +
                                                   "\"\nsize = 1\n",
                                               parsed, error));
    CHECK_FALSE(resonate::asset::parseManifest("total_bytes = 0\n[[chunk]]\nhash = \"" + digest +
                                                   "\"\nsize = 1\n",
                                               parsed, error));
    CHECK_FALSE(resonate::asset::parseManifest("not toml [", parsed, error));
}

TEST_CASE("cas: an empty product stores no chunks", "[asset][cas]")
{
    Store store("empty");
    CasManifest manifest;
    std::string error;
    REQUIRE(resonate::asset::storeBuffer(store.path(), keyA(), Span<const std::byte>(), manifest,
                                         error));
    CHECK(manifest.total_bytes == 0U);
    CHECK(manifest.chunks.empty());

    std::vector<std::byte> out{std::byte{1}};
    REQUIRE(resonate::asset::readBuffer(store.path(), keyA(), manifest, out, error));
    CHECK(out.empty());
}
