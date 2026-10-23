#include <resonate/ui/string_table.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace resonate::ui
{
namespace
{

bool isSpace(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

bool isNameChar(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-' ||
           character == '.' || character == ':';
}

struct Attribute
{
    std::string name;
    std::string value;
};

struct Tag
{
    std::string name;
    std::vector<Attribute> attributes;
    bool closing = false;
    bool self_closing = false;
};

class Reader
{
  public:
    explicit Reader(std::string_view text) : text_(text)
    {
        if (text_.substr(0U, 3U) == "\xEF\xBB\xBF")
        {
            pos_ = 3U;
        }
    }

    [[nodiscard]] bool atEnd() const { return pos_ >= text_.size(); }
    [[nodiscard]] int line() const { return line_; }

    /* Skips whitespace, comments and processing instructions. */
    bool skipMisc(std::string& error)
    {
        for (;;)
        {
            while (!atEnd() && isSpace(text_[pos_]))
            {
                advance();
            }
            if (startsWith("<!--"))
            {
                advance(4U);
                while (!startsWith("-->"))
                {
                    if (atEnd())
                    {
                        return fail("unterminated comment", error);
                    }
                    advance();
                }
                advance(3U);
                continue;
            }
            if (startsWith("<?"))
            {
                advance(2U);
                while (!startsWith("?>"))
                {
                    if (atEnd())
                    {
                        return fail("unterminated processing instruction", error);
                    }
                    advance();
                }
                advance(2U);
                continue;
            }
            return true;
        }
    }

    bool readTag(Tag& out, std::string& error)
    {
        if (!startsWith("<"))
        {
            return fail("expected '<'", error);
        }
        advance();
        if (startsWith("/"))
        {
            out.closing = true;
            advance();
        }
        if (!readName(out.name, error))
        {
            return false;
        }
        if (out.closing)
        {
            while (!atEnd() && isSpace(peek()))
            {
                advance();
            }
            if (!startsWith(">"))
            {
                return fail("a closing tag takes no attributes", error);
            }
            advance();
            return true;
        }
        for (;;)
        {
            while (!atEnd() && isSpace(peek()))
            {
                advance();
            }
            if (startsWith("/>"))
            {
                out.self_closing = true;
                advance(2U);
                return true;
            }
            if (startsWith(">"))
            {
                advance();
                return true;
            }
            Attribute attribute;
            if (!readAttribute(attribute, error))
            {
                return false;
            }
            for (const Attribute& existing : out.attributes)
            {
                if (existing.name == attribute.name)
                {
                    return fail("duplicate attribute '" + attribute.name + "'", error);
                }
            }
            out.attributes.push_back(std::move(attribute));
        }
    }

    /* Reads character content up to the matching `name` closing tag. */
    bool readText(std::string_view name, std::string& out, std::string& error)
    {
        for (;;)
        {
            if (atEnd())
            {
                return fail("missing </" + std::string(name) + ">", error);
            }
            if (startsWith("</"))
            {
                Tag closing;
                if (!readTag(closing, error))
                {
                    return false;
                }
                if (!closing.closing || closing.name != name)
                {
                    return fail("expected </" + std::string(name) + ">", error);
                }
                return true;
            }
            if (peek() == '<')
            {
                return fail("nested elements are not supported", error);
            }
            if (peek() == '&')
            {
                if (!readEntity(out, error))
                {
                    return false;
                }
                continue;
            }
            out.push_back(peek());
            advance();
        }
    }

  private:
    bool fail(std::string message, std::string& error) const
    {
        error = "strings:" + std::to_string(line_) + ": " + std::move(message);
        return false;
    }

    [[nodiscard]] char peek() const { return text_[pos_]; }
    [[nodiscard]] bool startsWith(std::string_view token) const
    {
        return text_.substr(pos_, token.size()) == token;
    }

    void advance(std::size_t count = 1U)
    {
        for (std::size_t step = 0U; step < count && pos_ < text_.size(); ++step)
        {
            if (text_[pos_] == '\n')
            {
                ++line_;
            }
            ++pos_;
        }
    }

    bool readName(std::string& out, std::string& error)
    {
        while (!atEnd() && isNameChar(peek()))
        {
            out.push_back(peek());
            advance();
        }
        if (out.empty())
        {
            return fail("expected a name", error);
        }
        return true;
    }

    bool readAttribute(Attribute& out, std::string& error)
    {
        if (!readName(out.name, error))
        {
            return false;
        }
        while (!atEnd() && isSpace(peek()))
        {
            advance();
        }
        if (!startsWith("="))
        {
            return fail("expected '=' after '" + out.name + "'", error);
        }
        advance();
        while (!atEnd() && isSpace(peek()))
        {
            advance();
        }
        if (atEnd() || (peek() != '"' && peek() != '\''))
        {
            return fail("expected a quoted value for '" + out.name + "'", error);
        }
        const char quote = peek();
        advance();
        while (!atEnd() && peek() != quote)
        {
            if (peek() == '<')
            {
                return fail("'<' is not allowed in an attribute value", error);
            }
            if (peek() == '&')
            {
                if (!readEntity(out.value, error))
                {
                    return false;
                }
                continue;
            }
            out.value.push_back(peek());
            advance();
        }
        if (atEnd())
        {
            return fail("unterminated attribute value", error);
        }
        advance();
        return true;
    }

    bool readEntity(std::string& out, std::string& error)
    {
        advance();
        std::string name;
        while (!atEnd() && peek() != ';')
        {
            if (name.size() >= 8U)
            {
                return fail("unknown entity", error);
            }
            name.push_back(peek());
            advance();
        }
        if (atEnd())
        {
            return fail("unterminated entity", error);
        }
        advance();
        if (name == "amp")
        {
            out.push_back('&');
        }
        else if (name == "lt")
        {
            out.push_back('<');
        }
        else if (name == "gt")
        {
            out.push_back('>');
        }
        else if (name == "quot")
        {
            out.push_back('"');
        }
        else if (name == "apos")
        {
            out.push_back('\'');
        }
        else
        {
            return fail("unknown entity '&" + name + ";'", error);
        }
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0U;
    int line_ = 1;
};

} // namespace

std::unique_ptr<StringTable> StringTable::parse(std::string_view xml, std::string& error)
{
    Reader reader(xml);
    if (!reader.skipMisc(error))
    {
        return nullptr;
    }
    Tag root;
    if (!reader.readTag(root, error))
    {
        return nullptr;
    }
    if (root.closing || root.name != "strings")
    {
        error = "strings:" + std::to_string(reader.line()) + ": expected <strings>";
        return nullptr;
    }
    auto table = std::make_unique<StringTable>();
    for (const Attribute& attribute : root.attributes)
    {
        if (attribute.name != "locale")
        {
            error = "strings:" + std::to_string(reader.line()) + ": unknown attribute '" +
                    attribute.name + "'";
            return nullptr;
        }
        table->locale_ = attribute.value;
    }
    if (!root.self_closing)
    {
        for (;;)
        {
            if (!reader.skipMisc(error))
            {
                return nullptr;
            }
            if (reader.atEnd())
            {
                error = "strings:" + std::to_string(reader.line()) + ": missing </strings>";
                return nullptr;
            }
            Tag tag;
            if (!reader.readTag(tag, error))
            {
                return nullptr;
            }
            if (tag.closing)
            {
                if (tag.name != "strings")
                {
                    error = "strings:" + std::to_string(reader.line()) + ": expected </strings>";
                    return nullptr;
                }
                break;
            }
            if (tag.name != "entry")
            {
                error = "strings:" + std::to_string(reader.line()) + ": unknown element '" +
                        tag.name + "'";
                return nullptr;
            }
            std::string key;
            for (const Attribute& attribute : tag.attributes)
            {
                if (attribute.name != "key")
                {
                    error = "strings:" + std::to_string(reader.line()) +
                            ": unknown attribute '" + attribute.name + "'";
                    return nullptr;
                }
                key = attribute.value;
            }
            if (key.empty())
            {
                error = "strings:" + std::to_string(reader.line()) + ": an entry needs a key";
                return nullptr;
            }
            if (table->entries_.find(key) != table->entries_.end())
            {
                error = "strings:" + std::to_string(reader.line()) + ": duplicate key '" + key +
                        "'";
                return nullptr;
            }
            std::string value;
            if (!tag.self_closing && !reader.readText("entry", value, error))
            {
                return nullptr;
            }
            table->entries_.emplace(std::move(key), std::move(value));
        }
    }
    if (!reader.skipMisc(error))
    {
        return nullptr;
    }
    if (!reader.atEnd())
    {
        error = "strings:" + std::to_string(reader.line()) + ": content after </strings>";
        return nullptr;
    }
    return table;
}

const std::string* StringTable::find(std::string_view key) const
{
    const auto iterator = entries_.find(std::string(key));
    return iterator == entries_.end() ? nullptr : &iterator->second;
}

} // namespace resonate::ui
