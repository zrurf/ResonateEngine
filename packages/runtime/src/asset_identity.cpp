#include <resonate/asset/identity.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

namespace resonate::asset
{
namespace
{

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

std::string formatGuid(const AssetId& id)
{
    static const char* kDigits = "0123456789abcdef";
    const std::uint64_t halves[2] = {id.guid_high, id.guid_low};
    /* The two halves render big-endian, each byte as two hex digits, with the
       canonical dashes after the 8th, 12th and 16th byte. */
    std::string text;
    text.reserve(36U);
    for (int half = 0; half < 2; ++half)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            const std::uint8_t byte =
                static_cast<std::uint8_t>((halves[half] >> shift) & 0xFFU);
            text.push_back(kDigits[byte >> 4U]);
            text.push_back(kDigits[byte & 0x0FU]);
            const int absolute = half * 8 + (7 - shift / 8);
            if (absolute == 3 || absolute == 5 || absolute == 7 || absolute == 9)
            {
                text.push_back('-');
            }
        }
    }
    return text;
}

bool parseGuid(std::string_view text, AssetId& out)
{
    /* Dashes are optional: the compact 32-hex form parses the same. */
    std::uint64_t halves[2] = {};
    int half = 0;
    int nibbles = 0;
    for (const char character : text)
    {
        if (character == '-')
        {
            continue;
        }
        if (hexValue(character) < 0 || half >= 2 || nibbles >= 16)
        {
            return false;
        }
        halves[half] = (halves[half] << 4U) | static_cast<std::uint64_t>(hexValue(character));
        ++nibbles;
        if (nibbles == 16)
        {
            ++half;
            nibbles = 0;
        }
    }
    if (half != 2 || nibbles != 0)
    {
        return false;
    }
    out.guid_high = halves[0];
    out.guid_low = halves[1];
    return true;
}

bool Identity::loadProducts(std::string_view toml, std::string& error)
{
    /* This toml++ version's parse_result is the table itself: false means the
       text did not parse, with no description to relay. */
    toml::parse_result result = toml::parse(toml);
    if (!result)
    {
        error = "the manifest does not parse";
        return false;
    }
    const toml::array* entries = result.table()["product"].as_array();
    if (entries == nullptr)
    {
        return true;
    }
    /* Parsed aside: a manifest with one bad entry leaves the identity exactly
       as it was, so a caller that reports the failure never serves half of it. */
    std::vector<Product> parsed;
    parsed.reserve(entries->size());
    for (const toml::node& entry : *entries)
    {
        const toml::table* product = entry.as_table();
        if (product == nullptr)
        {
            error = "a [[product]] entry is not a table";
            return false;
        }
        const std::string guid = product->at_path("guid").value_or(std::string{});
        const std::string path = product->at_path("path").value_or(std::string{});
        const std::int64_t sub_id = product->at_path("sub_id").value_or<std::int64_t>(0);
        Product record;
        if (!parseGuid(guid, record.id) || sub_id <= 0 || path.empty())
        {
            error = "a [[product]] entry needs a guid, a positive sub_id and a path";
            return false;
        }
        record.id.sub_id = static_cast<std::uint32_t>(sub_id);
        if (!record.id.valid())
        {
            error = "a [[product]] entry needs a guid, a positive sub_id and a path";
            return false;
        }
        const auto duplicates = [&record](const std::vector<Product>& known)
        {
            return std::any_of(known.begin(), known.end(),
                               [&record](const Product& item) { return item.id == record.id; });
        };
        if (duplicates(products_) || duplicates(parsed))
        {
            error = "the manifest names " + formatGuid(record.id) + " twice";
            return false;
        }
        record.path = path;
        parsed.push_back(std::move(record));
    }
    products_.insert(products_.end(), std::make_move_iterator(parsed.begin()),
                     std::make_move_iterator(parsed.end()));
    return true;
}

std::optional<std::string> Identity::locate(const AssetId& id) const
{
    for (const Product& product : products_)
    {
        if (product.id == id)
        {
            return product.path;
        }
    }
    return std::nullopt;
}

bool Identity::loadFile(const std::string& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot open " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return loadProducts(text, error);
}

std::optional<AssetId> Identity::idForPath(std::string_view path) const
{
    for (const Product& product : products_)
    {
        if (product.path == path)
        {
            return product.id;
        }
    }
    return std::nullopt;
}

} // namespace resonate::asset
