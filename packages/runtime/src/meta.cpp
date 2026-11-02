#include <resonate/asset/meta.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <system_error>
#include <utility>

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

namespace resonate::asset
{
namespace
{

constexpr const char* kHeader =
    "# Resonate asset identity; references use the GUID. Do not edit.\n";

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

/* The stored form of a source hash is the lowercased hex; both the parser and
   the writer go through this, so a sidecar never holds what the other side
   would reject. */
bool normalizeSourceHash(std::string_view text, std::string& out)
{
    if (text.size() != 64U)
    {
        return false;
    }
    out.assign(text);
    for (char& character : out)
    {
        if (hexValue(character) < 0)
        {
            return false;
        }
        if (character >= 'A' && character <= 'F')
        {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return true;
}

std::string escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out.push_back(character);
            break;
        }
    }
    return out;
}

} // namespace

bool parseMeta(std::string_view text, AssetMeta& out, std::string& error)
{
    out = {};
    toml::parse_result result = toml::parse(text);
    if (!result)
    {
        error = "the sidecar does not parse";
        return false;
    }
    const toml::table& table = result.table();
    const std::string guid = table["guid"].value_or(std::string{});
    if (!parseGuid(guid, out.id) || (out.id.guid_low == 0U && out.id.guid_high == 0U))
    {
        error = "the sidecar needs a non-null guid";
        return false;
    }
    if (const toml::node* node = table.get("importer"); node != nullptr)
    {
        const std::optional<std::string> value = node->value<std::string>();
        if (!value || value->empty())
        {
            error = "importer must be a non-empty string";
            return false;
        }
        out.importer = *value;
    }
    if (const toml::node* node = table.get("source_hash"); node != nullptr)
    {
        const std::optional<std::string> value = node->value<std::string>();
        std::string hash;
        if (!value || !normalizeSourceHash(*value, hash))
        {
            error = "source_hash must be 64 hexadecimal digits";
            return false;
        }
        out.source_hash = std::move(hash);
    }
    if (const toml::node* node = table.get("settings"); node != nullptr)
    {
        const toml::table* settings = node->as_table();
        if (settings == nullptr)
        {
            error = "settings must be a table of strings";
            return false;
        }
        for (const auto& [key, value] : *settings)
        {
            /* The canonical form writes bare keys, so only bare-key characters
               may enter: a quoted key would round-trip into text the parser
               rejects. */
            const std::string name(key);
            const bool bare = !name.empty() &&
                              name.find_first_not_of(
                                  "abcdefghijklmnopqrstuvwxyz"
                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                                  std::string::npos;
            const std::optional<std::string> text = value.value<std::string>();
            if (!bare || !text)
            {
                error = "settings." + name + " must be a bare key naming a string";
                return false;
            }
            out.settings.emplace(name, *text);
        }
    }
    return true;
}

std::string formatMeta(const AssetMeta& meta)
{
    std::string text = kHeader;
    text += "guid = \"" + formatGuid(meta.id) + "\"\n";
    if (!meta.importer.empty())
    {
        text += "importer = \"" + escape(meta.importer) + "\"\n";
    }
    if (!meta.source_hash.empty())
    {
        text += "source_hash = \"" + meta.source_hash + "\"\n";
    }
    if (!meta.settings.empty())
    {
        /* std::map iterates sorted, so two serializations of one sidecar stay
           byte-identical whatever order the caller filled. */
        text += "\n[settings]\n";
        for (const auto& [key, value] : meta.settings)
        {
            text += key + " = \"" + escape(value) + "\"\n";
        }
    }
    return text;
}

bool readMetaFile(const std::string& path, AssetMeta& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot open " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (!parseMeta(text, out, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool writeMetaFile(const std::string& path, const AssetMeta& meta, std::string& error)
{
    /* The parser's own rule: a sidecar this build writes, this build reads. */
    std::string hash;
    if (!meta.source_hash.empty() && !normalizeSourceHash(meta.source_hash, hash))
    {
        error = "the sidecar's source_hash must be 64 hexadecimal digits";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), code);
    if (code)
    {
        error = "cannot create the directory for " + path;
        return false;
    }
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "cannot write " + path;
            return false;
        }
        const std::string text = formatMeta(meta);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "cannot write " + path;
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "cannot store " + path;
        return false;
    }
    return true;
}

AssetId generateGuid()
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    std::random_device device;
    const auto next = [&device]
    {
        return (static_cast<std::uint64_t>(device()) << 32U) |
               static_cast<std::uint64_t>(device());
    };
    const std::uint64_t first = next();
    const std::uint64_t second = next();

    AssetId id{};
    /* 48-bit timestamp, version 7, then 12 random bits. */
    id.guid_high = ((static_cast<std::uint64_t>(milliseconds) & 0xFFFFFFFFFFFFULL) << 16U) |
                   (0x7ULL << 12U) | (first & 0xFFFULL);
    /* The RFC 4122 variant in the top two bits, then 62 random bits. */
    id.guid_low = 0x8000000000000000ULL | (second & 0x3FFFFFFFFFFFFFFFULL);
    id.sub_id = 0U;
    return id;
}

} // namespace resonate::asset
