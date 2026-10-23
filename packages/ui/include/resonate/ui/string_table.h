#ifndef RESONATE_UI_STRING_TABLE_H
#define RESONATE_UI_STRING_TABLE_H

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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

    [[nodiscard]] const std::string* find(std::string_view key) const;
    [[nodiscard]] std::string_view locale() const noexcept { return locale_; }

  private:
    std::unordered_map<std::string, std::string> entries_;
    std::string locale_;
};

} // namespace resonate::ui

#endif /* RESONATE_UI_STRING_TABLE_H */
