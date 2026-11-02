#include <resonate/asset/cas.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

namespace resonate::asset
{
namespace
{

/* A fixed table, derived once with splitmix64: every machine, debuggable build
   included, cuts at the same bytes. */
const std::array<std::uint64_t, 256>& gearTable()
{
    static const std::array<std::uint64_t, 256> table = []
    {
        std::array<std::uint64_t, 256> values{};
        std::uint64_t state = 0x243F6A8885A308D3ULL;
        for (std::uint64_t& value : values)
        {
            state += 0x9E3779B97F4A7C15ULL;
            std::uint64_t mixed = state;
            mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
            value = mixed ^ (mixed >> 31U);
        }
        return values;
    }();
    return table;
}

bool readFile(const std::string& path, std::uint32_t expected_size,
              std::vector<std::byte>& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot read the chunk " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) != expected_size)
    {
        error = "the chunk " + path + " has the wrong size";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(length));
    if (!out.empty() &&
        !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())))
    {
        error = "cannot read the chunk " + path;
        return false;
    }
    return true;
}

bool writeFile(const std::string& path, Span<const std::byte> data, std::string& error)
{
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), code);
    if (code)
    {
        error = "cannot create the chunk directory for " + path;
        return false;
    }
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "cannot write the chunk " + path;
            return false;
        }
        if (!data.empty())
        {
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        out.close();
        if (!out)
        {
            /* The close is what flushes; a failure here means the temporary
               is not the bytes this call was given. */
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "cannot write the chunk " + path;
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code)
    {
        /* Another writer may have stored the same bytes between the exists
           check and this rename. The chunk is content-addressed, so a full
           winner is just as good; a truncated one, left by an interrupted
           store, has to be replaced. */
        std::error_code size_code;
        const std::uintmax_t size = std::filesystem::file_size(path, size_code);
        if (!size_code && size == data.size())
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return true;
        }
        std::error_code removed;
        std::filesystem::remove(path, removed);
        if (!removed)
        {
            code.clear();
            std::filesystem::rename(temporary, path, code);
            if (!code)
            {
                return true;
            }
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "cannot store the chunk " + path;
        return false;
    }
    return true;
}

int hexValue(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string formatDigest(const hash::Digest& digest)
{
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest)
    {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0FU]);
    }
    return text;
}

bool parseDigest(std::string_view text, hash::Digest& out)
{
    if (text.size() != out.size() * 2U)
    {
        return false;
    }
    for (std::size_t index = 0; index < out.size(); ++index)
    {
        const int high = hexValue(text[index * 2U]);
        const int low = hexValue(text[index * 2U + 1U]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        out[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

std::string chunkPath(const std::string& root, const hash::Digest& digest)
{
    const std::string text = formatDigest(digest);
    return root + "/chunks/" + text.substr(0U, 2U) + "/" + text;
}

std::vector<Span<const std::byte>> splitChunks(Span<const std::byte> data)
{
    std::vector<Span<const std::byte>> chunks;
    if (data.empty())
    {
        return chunks;
    }
    const std::array<std::uint64_t, 256>& gear = gearTable();
    std::size_t start = 0U;
    while (start < data.size())
    {
        const std::size_t limit = std::min(data.size(), start + kChunkMaxBytes);
        std::size_t end = limit;
        if (data.size() - start > kChunkMinBytes)
        {
            /* Restart the rolling hash at every chunk, so the boundary depends
               on the bytes that follow the previous cut, not on the file's
               offset. */
            std::uint64_t rolling = 0U;
            std::size_t position = start;
            for (; position < limit; ++position)
            {
                rolling = (rolling << 1U) + gear[static_cast<std::uint8_t>(data[position])];
                const std::size_t length = position - start + 1U;
                if (length >= kChunkMinBytes && (rolling & kChunkBoundaryMask) == 0U)
                {
                    ++position;
                    break;
                }
            }
            end = position;
        }
        chunks.push_back(data.subspan(start, end - start));
        start = end;
    }
    return chunks;
}

bool storeBuffer(const std::string& root, const ContentKey& key, Span<const std::byte> data,
                 CasManifest& out_manifest, std::string& error)
{
    out_manifest = {};
    out_manifest.total_bytes = data.size();
    const std::vector<Span<const std::byte>> chunks = splitChunks(data);
    out_manifest.chunks.reserve(chunks.size());
    for (const Span<const std::byte> chunk : chunks)
    {
        ChunkRef reference;
        reference.size = static_cast<std::uint32_t>(chunk.size());
        reference.hash = hash::blake3Keyed(chunk.data(), chunk.size(), key.data(), key.size());
        out_manifest.chunks.push_back(reference);

        const std::string path = chunkPath(root, reference.hash);
        std::error_code code;
        if (std::filesystem::exists(path, code) && !code)
        {
            /* A size check catches a truncated store from an interrupted
               cook. */
            std::error_code size_code;
            const std::uintmax_t size = std::filesystem::file_size(path, size_code);
            if (!size_code && size == reference.size)
            {
                continue;
            }
        }
        if (!writeFile(path, chunk, error))
        {
            return false;
        }
    }
    return true;
}

bool readBuffer(const std::string& root, const ContentKey& key, const CasManifest& manifest,
                std::vector<std::byte>& out, std::string& error)
{
    out.clear();
    out.reserve(static_cast<std::size_t>(manifest.total_bytes));
    std::vector<std::byte> chunk;
    for (const ChunkRef& reference : manifest.chunks)
    {
        const std::string path = chunkPath(root, reference.hash);
        if (!readFile(path, reference.size, chunk, error))
        {
            return false;
        }
        const hash::Digest actual =
            hash::blake3Keyed(chunk.data(), chunk.size(), key.data(), key.size());
        if (!(actual == reference.hash))
        {
            error = "the chunk " + path + " is corrupt";
            return false;
        }
        out.insert(out.end(), chunk.begin(), chunk.end());
    }
    if (out.size() != manifest.total_bytes)
    {
        error = "the manifest's total does not match its chunks";
        return false;
    }
    return true;
}

std::string formatManifest(const CasManifest& manifest)
{
    std::string text = "total_bytes = " + std::to_string(manifest.total_bytes) + "\n";
    for (const ChunkRef& chunk : manifest.chunks)
    {
        text += "\n[[chunk]]\nhash = \"" + formatDigest(chunk.hash) + "\"\n";
        text += "size = " + std::to_string(chunk.size) + "\n";
    }
    return text;
}

bool parseManifest(std::string_view text, CasManifest& out, std::string& error)
{
    out = {};
    toml::parse_result result = toml::parse(text);
    if (!result)
    {
        error = "the manifest does not parse";
        return false;
    }
    const std::int64_t total = result.table()["total_bytes"].value_or<std::int64_t>(-1);
    if (total < 0)
    {
        error = "the manifest needs a non-negative total_bytes";
        return false;
    }
    out.total_bytes = static_cast<std::uint64_t>(total);

    const toml::array* chunks = result.table()["chunk"].as_array();
    std::uint64_t sum = 0U;
    if (chunks != nullptr)
    {
        out.chunks.reserve(chunks->size());
        for (const toml::node& entry : *chunks)
        {
            const toml::table* table = entry.as_table();
            if (table == nullptr)
            {
                error = "a [[chunk]] entry is not a table";
                return false;
            }
            const std::string hash_text =
                table->at_path("hash").value_or(std::string{});
            const std::int64_t size = table->at_path("size").value_or<std::int64_t>(0);
            ChunkRef reference;
            if (!parseDigest(hash_text, reference.hash) || size <= 0 ||
                static_cast<std::uint64_t>(size) > kChunkMaxBytes)
            {
                error = "a [[chunk]] entry needs a 64-digit hash and a size between 1 and " +
                        std::to_string(kChunkMaxBytes);
                return false;
            }
            reference.size = static_cast<std::uint32_t>(size);
            sum += reference.size;
            out.chunks.push_back(reference);
        }
    }
    if (sum != out.total_bytes)
    {
        error = "the manifest's total_bytes does not match its chunks";
        return false;
    }
    return true;
}

} // namespace resonate::asset
