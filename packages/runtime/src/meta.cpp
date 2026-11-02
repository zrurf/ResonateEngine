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
        if (!value || value->size() != 64U)
        {
            error = "source_hash must be 64 hexadecimal digits";
            return false;
        }
        std::string hash = *value;
        for (char& character : hash)
        {
            if (hexValue(character) < 0)
            {
                error = "source_hash must be 64 hexadecimal digits";
                return false;
            }
            if (character >= 'A' && character <= 'F')
            {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        out.source_hash = std::move(hash);
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
        if (!out.good())
        {
            error = "cannot write " + path;
            return false;
        }
    }
    std::filesystem::rename(temporary, path, code);
    if (code)
    {
        std::filesystem::remove(temporary, code);
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
