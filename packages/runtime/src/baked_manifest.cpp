#include "resonate/asset/baked_manifest.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iterator>
#include <type_traits>

namespace resonate::asset {
namespace {

constexpr std::uint32_t kMagic = 0x314D4252U; /* "RBM1", little-endian */
constexpr std::uint32_t kMaxDepth = 64U;
constexpr std::uint32_t kMaxEntries = 1U << 20U;

enum class Kind : std::uint8_t
{
    Table = 1,
    Array = 2,
    String = 3,
    Integer = 4,
    Boolean = 5,
    Real = 6,
};

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void putU64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    putU32(out, static_cast<std::uint32_t>(value));
    putU32(out, static_cast<std::uint32_t>(value >> 32U));
}

void putText(std::vector<std::uint8_t>& out, std::string_view text)
{
    putU32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

bool getU32(const std::vector<std::uint8_t>& in, std::size_t& at, std::uint32_t& out)
{
    if (at + 4U > in.size())
    {
        return false;
    }
    out = static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1U]) << 8U) |
          (static_cast<std::uint32_t>(in[at + 2U]) << 16U) |
          (static_cast<std::uint32_t>(in[at + 3U]) << 24U);
    at += 4U;
    return true;
}

bool getU64(const std::vector<std::uint8_t>& in, std::size_t& at, std::uint64_t& out)
{
    std::uint32_t low = 0U;
    std::uint32_t high = 0U;
    if (!getU32(in, at, low) || !getU32(in, at, high))
    {
        return false;
    }
    out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32U);
    return true;
}

bool getText(const std::vector<std::uint8_t>& in, std::size_t& at, std::string& out)
{
    std::uint32_t size = 0U;
    if (!getU32(in, at, size) || at + size > in.size())
    {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(in.data()) + at, size);
    at += size;
    return true;
}

bool writeNode(std::vector<std::uint8_t>& out, const toml::node& node, std::string& error);

bool writeTable(std::vector<std::uint8_t>& out, const toml::table& table, std::string& error)
{
    out.push_back(static_cast<std::uint8_t>(Kind::Table));
    putU32(out, static_cast<std::uint32_t>(table.size()));
    for (const auto& [key, value] : table)
    {
        putText(out, key.str());
        if (!writeNode(out, value, error))
        {
            return false;
        }
    }
    return true;
}

bool writeNode(std::vector<std::uint8_t>& out, const toml::node& node, std::string& error)
{
    const bool handled = node.visit(
        [&](auto&& value)
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, toml::table>)
            {
                return writeTable(out, value, error);
            }
            else if constexpr (std::is_same_v<Value, toml::array>)
            {
                out.push_back(static_cast<std::uint8_t>(Kind::Array));
                putU32(out, static_cast<std::uint32_t>(value.size()));
                for (const toml::node& element : value)
                {
                    if (!writeNode(out, element, error))
                    {
                        return false;
                    }
                }
                return true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<std::string>>)
            {
                out.push_back(static_cast<std::uint8_t>(Kind::String));
                putText(out, value.get());
                return true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<std::int64_t>>)
            {
                out.push_back(static_cast<std::uint8_t>(Kind::Integer));
                const std::int64_t raw = value.get();
                std::uint64_t bits = 0U;
                std::memcpy(&bits, &raw, sizeof(bits));
                putU64(out, bits);
                return true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<bool>>)
            {
                out.push_back(static_cast<std::uint8_t>(Kind::Boolean));
                out.push_back(value.get() ? 1U : 0U);
                return true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<double>>)
            {
                out.push_back(static_cast<std::uint8_t>(Kind::Real));
                const double raw = value.get();
                std::uint64_t bits = 0U;
                std::memcpy(&bits, &raw, sizeof(bits));
                putU64(out, bits);
                return true;
            }
            else
            {
                return false;
            }
        });
    if (!handled)
    {
        error = "a manifest value is of a kind the baked form does not carry";
    }
    return handled;
}

bool readValue(std::vector<std::uint8_t>& in, std::size_t& at, toml::table* into_table,
               toml::array* into_array, std::string_view key, std::uint32_t depth,
               std::string& error);

bool readTableBody(std::vector<std::uint8_t>& in, std::size_t& at, toml::table& out,
                   std::uint32_t depth, std::string& error)
{
    std::uint32_t count = 0U;
    if (!getU32(in, at, count) || count > kMaxEntries)
    {
        error = "baked manifest is corrupt (table size)";
        return false;
    }
    for (std::uint32_t entry = 0U; entry < count; ++entry)
    {
        std::string key;
        if (!getText(in, at, key))
        {
            error = "baked manifest is corrupt (key)";
            return false;
        }
        if (!readValue(in, at, &out, nullptr, key, depth, error))
        {
            return false;
        }
    }
    return true;
}

bool readValue(std::vector<std::uint8_t>& in, std::size_t& at, toml::table* into_table,
               toml::array* into_array, std::string_view key, std::uint32_t depth,
               std::string& error)
{
    if (depth > kMaxDepth)
    {
        error = "baked manifest nests too deeply";
        return false;
    }
    if (at + 1U > in.size())
    {
        error = "baked manifest is truncated";
        return false;
    }
    const auto kind = static_cast<Kind>(in[at++]);
    const auto place = [&](auto&& value)
    {
        if (into_table != nullptr)
        {
            into_table->insert_or_assign(key, std::move(value));
        }
        else
        {
            into_array->push_back(std::move(value));
        }
    };
    switch (kind)
    {
        case Kind::Table:
        {
            toml::table nested;
            if (!readTableBody(in, at, nested, depth + 1U, error))
            {
                return false;
            }
            place(std::move(nested));
            return true;
        }
        case Kind::Array:
        {
            std::uint32_t count = 0U;
            if (!getU32(in, at, count) || count > kMaxEntries)
            {
                error = "baked manifest is corrupt (array size)";
                return false;
            }
            toml::array array;
            for (std::uint32_t entry = 0U; entry < count; ++entry)
            {
                if (!readValue(in, at, nullptr, &array, {}, depth + 1U, error))
                {
                    return false;
                }
            }
            place(std::move(array));
            return true;
        }
        case Kind::String:
        {
            std::string text;
            if (!getText(in, at, text))
            {
                error = "baked manifest is corrupt (string)";
                return false;
            }
            place(std::move(text));
            return true;
        }
        case Kind::Integer:
        {
            std::uint64_t raw = 0U;
            if (!getU64(in, at, raw))
            {
                error = "baked manifest is truncated (integer)";
                return false;
            }
            std::int64_t value = 0U;
            std::memcpy(&value, &raw, sizeof(value));
            place(value);
            return true;
        }
        case Kind::Boolean:
        {
            if (at + 1U > in.size())
            {
                error = "baked manifest is truncated (boolean)";
                return false;
            }
            place(in[at++] != 0U);
            return true;
        }
        case Kind::Real:
        {
            std::uint64_t raw = 0U;
            if (!getU64(in, at, raw))
            {
                error = "baked manifest is truncated (real)";
                return false;
            }
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(value));
            place(value);
            return true;
        }
        default:
            error = "baked manifest carries an unknown value kind";
            return false;
    }
}

} // namespace

std::string bakedTwinOf(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    const std::size_t separator = path.find_last_of("/\\");
    if (dot == std::string::npos || (separator != std::string::npos && dot < separator))
    {
        return path + kBakedManifestExtension;
    }
    return path.substr(0U, dot) + kBakedManifestExtension;
}

std::string resolveManifestPath(const std::string& path)
{
    const std::string twin = bakedTwinOf(path);
    std::error_code error;
    if (std::filesystem::is_regular_file(twin, error))
    {
        return twin;
    }
    if (std::filesystem::is_regular_file(path, error))
    {
        return path;
    }
    return {};
}

bool bakeTable(const toml::table& in, std::vector<std::uint8_t>& out, std::string& error)
{
    error.clear();
    out.clear();
    out.push_back(static_cast<std::uint8_t>(kMagic));
    out.push_back(static_cast<std::uint8_t>(kMagic >> 8U));
    out.push_back(static_cast<std::uint8_t>(kMagic >> 16U));
    out.push_back(static_cast<std::uint8_t>(kMagic >> 24U));
    if (!writeTable(out, in, error))
    {
        out.clear();
        return false;
    }
    if (std::getenv("RESONATE_BAKE_TRACE") != nullptr)
    {
        std::fprintf(stderr, "baked: %zu bytes:", out.size());
        for (std::size_t i = 0; i < out.size() && i < 24U; ++i)
        {
            std::fprintf(stderr, " %02x", out[i]);
        }
        std::fprintf(stderr, "\n");
    }
    return true;
}

bool loadBaked(const std::string& path, toml::table& out, std::string& error)
{
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot open " + path;
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < 4U || bytes[0] != static_cast<std::uint8_t>(kMagic) ||
        bytes[1] != static_cast<std::uint8_t>(kMagic >> 8U) ||
        bytes[2] != static_cast<std::uint8_t>(kMagic >> 16U) ||
        bytes[3] != static_cast<std::uint8_t>(kMagic >> 24U))
    {
        error = path + " is not a baked manifest";
        return false;
    }
    /* The root is encoded like any table: one kind byte, then the body. */
    if (bytes.size() < 6U || bytes[4] != static_cast<std::uint8_t>(Kind::Table))
    {
        error = path + " is not a baked manifest";
        return false;
    }
    std::size_t at = 5U;
    out.clear();
    const bool ok = readTableBody(bytes, at, out, 0U, error);
    if (!ok)
    {
        std::fprintf(stderr, "baked: load failed at %zu of %zu: %s\n", at, bytes.size(),
                     error.c_str());
    }
    return ok;
}

} // namespace resonate::asset
