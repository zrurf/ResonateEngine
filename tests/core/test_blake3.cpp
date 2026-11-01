#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include <resonate/core/blake3.h>

namespace
{

std::string hex(const resonate::hash::Digest& digest)
{
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(64U);
    for (const std::uint8_t byte : digest)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0FU]);
    }
    return text;
}

} // namespace

/* The official test vectors (BLAKE3 1.8.7 test_vectors.json): the input for
   case N is the repeating byte sequence 0, 1, ..., 250, 0, ... */
TEST_CASE("blake3: the official vectors hold", "[core][hash]")
{
    using resonate::hash::blake3;
    using resonate::hash::blake3Keyed;

    CHECK(hex(blake3(nullptr, 0U)) ==
          "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");

    const std::uint8_t abc[3] = {0U, 1U, 2U};
    CHECK(hex(blake3(abc, sizeof(abc))) ==
          "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f");

    constexpr char kKey[] = "whats the Elvish word for friend";
    REQUIRE(std::strlen(kKey) == 32U);
    CHECK(hex(blake3Keyed(nullptr, 0U, kKey, 32U)) ==
          "92b2b75604ed3c761f9d6f62392c8a9227ad0ea3f09573e783f1498a4ed60d26");
    CHECK(hex(blake3Keyed(abc, sizeof(abc), kKey, 32U)) ==
          "39e67b76b5a007d4921969779fe666da67b5213b096084ab674742f0d5ec62b9");

    /* The empty-input digest is the one the constant names. */
    CHECK(resonate::hash::blake3(nullptr, 0U) == resonate::hash::kEmptyDigest);
}

TEST_CASE("blake3: a stream hashes the same as one buffer", "[core][hash]")
{
    std::array<std::uint8_t, 1000> data{};
    for (std::size_t index = 0; index < data.size(); ++index)
    {
        data[index] = static_cast<std::uint8_t>(index % 251U);
    }

    resonate::hash::Hasher split;
    split.update(data.data(), 1U);
    split.update(data.data() + 1U, 333U);
    split.update(data.data() + 334U, data.size() - 334U);
    CHECK(split.finalize() == resonate::hash::blake3(data.data(), data.size()));

    constexpr char kKey[] = "whats the Elvish word for friend";
    resonate::hash::Hasher keyed(kKey, 32U);
    keyed.update(data.data(), data.size());
    CHECK(keyed.finalize() == resonate::hash::blake3Keyed(data.data(), data.size(), kKey, 32U));

    /* A key of the wrong size falls back to the unkeyed mode. */
    resonate::hash::Hasher wrong(kKey, 8U);
    wrong.update(data.data(), data.size());
    CHECK(wrong.finalize() == resonate::hash::blake3(data.data(), data.size()));
}
