#ifndef RESONATE_UI_STRING_TABLE_H
#define RESONATE_UI_STRING_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <resonate/core/span.h>

namespace resonate::ui
{

/* One locale's key-to-text table loaded from a .rstrings document. UI markup
   references a key as "#key"; a key missing from the table falls back to the
   key text and reports a warning. */
class StringTable
{
  public:
    /* Parses the XML subset: a <strings> root with one optional locale
       attribute and <entry key="...">text</entry> children. Unknown elements
       or attributes, duplicate keys and malformed entities are errors. */
    [[nodiscard]] static std::unique_ptr<StringTable> parse(std::string_view xml,
                                                            std::string& error);

    /* True when the bytes begin with the cooked table magic. */
    [[nodiscard]] static bool isBinary(Span<const std::byte> bytes) noexcept;

    /* Parses either form: the XML source during development, the cooked binary
       a packaged build ships. */
    [[nodiscard]] static std::unique_ptr<StringTable> parseBytes(Span<const std::byte> bytes,
                                                                 std::string& error);

    /* The cooked form: locale once, then entries sorted by key so the bytes are
       deterministic. */
    [[nodiscard]] std::vector<std::byte> toBinary() const;

    [[nodiscard]] const std::string* find(std::string_view key) const;
    [[nodiscard]] std::string_view locale() const noexcept { return locale_; }

    /* Every key and text, in no particular order; tools that verify a cooked
       table walk this instead of trusting the codec. */
    [[nodiscard]] const std::unordered_map<std::string, std::string>& entries() const noexcept
    {
        return entries_;
    }

  private:
    std::unordered_map<std::string, std::string> entries_;
    std::string locale_;
};

} // namespace resonate::ui

#endif /* RESONATE_UI_STRING_TABLE_H */
