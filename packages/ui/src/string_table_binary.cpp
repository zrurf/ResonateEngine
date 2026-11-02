/* The cooked string table codec: locale and key/text pairs as one packed table.
 *
 * Layout (little-endian):
 *
 *   "RSTR" | u32 codec | u32 locale length, bytes
 *   | u32 entry count, each: u32 key length, bytes, u32 text length, bytes
 *
 * Entries are written sorted by key, so the same table has the same bytes on
 * every platform. The decoder refuses duplicates, empty keys and trailing
 * bytes; the XML parser's rules are the ones the cooked form keeps.
 */

#include <resonate/ui/string_table.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace resonate::ui
{
namespace
{

constexpr char kMagic[4] = {'R', 'S', 'T', 'R'};
constexpr std::uint32_t kCodecVersion = 1U;

void appendU32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (int byte = 0; byte < 4; ++byte)
    {
        out.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
    }
}

void appendText(std::vector<std::byte>& out, std::string_view value)
{
    appendU32(out, static_cast<std::uint32_t>(value.size()));
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    out.insert(out.end(), first, first + value.size());
}

struct Reader
{
    Span<const std::byte> data;
    std::size_t at = 0;

    [[nodiscard]] bool u32(std::uint32_t& out)
    {
        if (4U > data.size() - at)
        {
            return false;
        }
        out = 0U;
        for (std::size_t byte = 0U; byte < 4U; ++byte)
        {
            out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[at + byte]))
                   << (8U * static_cast<unsigned>(byte));
        }
        at += 4U;
        return true;
    }

    [[nodiscard]] bool text(std::string& out)
    {
        std::uint32_t size = 0U;
        if (!u32(size) || size > data.size() - at)
        {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(data.data() + at), size);
        at += size;
        return true;
    }
};

} // namespace

bool StringTable::isBinary(Span<const std::byte> bytes) noexcept
{
    if (bytes.size() < sizeof(kMagic))
    {
        return false;
    }
    return std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

std::vector<std::byte> StringTable::toBinary() const
{
    std::vector<std::pair<std::string_view, std::string_view>> sorted;
    sorted.reserve(entries_.size());
    for (const auto& [key, value] : entries_)
    {
        sorted.emplace_back(key, value);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });

    std::vector<std::byte> bytes;
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(kMagic),
                 reinterpret_cast<const std::byte*>(kMagic) + sizeof(kMagic));
    appendU32(bytes, kCodecVersion);
    appendText(bytes, locale_);
    appendU32(bytes, static_cast<std::uint32_t>(sorted.size()));
    for (const auto& [key, value] : sorted)
    {
        appendText(bytes, key);
        appendText(bytes, value);
    }
    return bytes;
}

std::unique_ptr<StringTable> StringTable::parseBytes(Span<const std::byte> bytes,
                                                     std::string& error)
{
    if (!isBinary(bytes))
    {
        return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                     error);
    }
    if (bytes.size() < sizeof(kMagic) + 8U)
    {
        error = "the cooked string table is truncated";
        return nullptr;
    }
    Reader reader{bytes, sizeof(kMagic)};
    std::uint32_t codec = 0U;
    if (!reader.u32(codec))
    {
        error = "the cooked string table is truncated";
        return nullptr;
    }
    if (codec != kCodecVersion)
    {
        error = "the cooked string table codec " + std::to_string(codec) +
                " is not the one this build reads";
        return nullptr;
    }

    auto table = std::make_unique<StringTable>();
    if (!reader.text(table->locale_))
    {
        error = "the cooked string table's locale is truncated";
        return nullptr;
    }
    std::uint32_t count = 0U;
    if (!reader.u32(count) || count > (reader.data.size() - reader.at) / 8U)
    {
        error = "the cooked string table's entry list is truncated";
        return nullptr;
    }
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        std::string key;
        std::string value;
        if (!reader.text(key) || !reader.text(value))
        {
            error = "the cooked string table's entries are truncated";
            return nullptr;
        }
        if (key.empty())
        {
            error = "the cooked string table has an empty key";
            return nullptr;
        }
        if (!table->entries_.emplace(std::move(key), std::move(value)).second)
        {
            error = "the cooked string table repeats a key";
            return nullptr;
        }
    }
    if (reader.at != bytes.size())
    {
        error = "the cooked string table has trailing bytes";
        return nullptr;
    }
    return table;
}

} // namespace resonate::ui
