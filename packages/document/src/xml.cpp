#include <resonate/dom/document.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace resonate::dom
{
namespace
{

struct Attribute
{
    std::string name;
    std::string value;
};

struct Tag
{
    std::string name;
    std::vector<Attribute> attributes;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    bool closing = false;
    bool self_closing = false;
};

/* The XML subset the document grammar accepts: elements, attributes and
   comments. There is no text content, no DTD, no namespace and no external
   entity; the five predefined entities and character references are decoded
   because string values need them. */
class Reader
{
  public:
    enum class Next
    {
        Tag,
        End,
        Error,
    };

    Reader(std::string_view text, std::vector<Diagnostic>& diagnostics)
        : text_(text), diagnostics_(&diagnostics)
    {
    }

    /* The XML declaration is the only processing instruction allowed, and only
       at the very start. */
    bool skipDeclaration()
    {
        if (text_.size() >= 3U && static_cast<unsigned char>(text_[0]) == 0xEFU &&
            static_cast<unsigned char>(text_[1]) == 0xBBU &&
            static_cast<unsigned char>(text_[2]) == 0xBFU)
        {
            return fail(1U, 1U, "a UTF-8 BOM is not allowed");
        }
        if (text_.compare(0U, 5U, "<?xml") != 0)
        {
            return true;
        }
        const std::size_t close = text_.find("?>", 5U);
        if (close == std::string_view::npos)
        {
            return fail(1U, 1U, "unterminated XML declaration");
        }
        advance(close + 2U);
        return true;
    }

    Next next(Tag& out)
    {
        if (!skipMisc())
        {
            return Next::Error;
        }
        if (pos_ >= text_.size())
        {
            return Next::End;
        }
        if (text_[pos_] != '<')
        {
            return failNext(line_, column_, "text content is not allowed");
        }
        return parseTag(out);
    }

    [[nodiscard]] std::uint32_t line() const noexcept
    {
        return line_;
    }

    [[nodiscard]] std::uint32_t column() const noexcept
    {
        return column_;
    }

  private:
    bool fail(std::uint32_t line, std::uint32_t column, std::string message)
    {
        diagnostics_->push_back(
            Diagnostic{Diagnostic::Severity::Error, line, column, std::move(message)});
        return false;
    }

    Next failNext(std::uint32_t line, std::uint32_t column, std::string message)
    {
        fail(line, column, std::move(message));
        return Next::Error;
    }

    void advance(std::size_t count)
    {
        const std::size_t end = std::min(pos_ + count, text_.size());
        for (; pos_ < end; ++pos_)
        {
            if (text_[pos_] == '\n')
            {
                ++line_;
                column_ = 1U;
            }
            else
            {
                ++column_;
            }
        }
    }

    bool skipMisc()
    {
        for (;;)
        {
            while (pos_ < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                advance(1U);
            }
            if (text_.compare(pos_, 4U, "<!--") == 0)
            {
                const std::size_t close = text_.find("-->", pos_ + 4U);
                if (close == std::string_view::npos)
                {
                    return fail(line_, column_, "unterminated comment");
                }
                advance(close + 3U - pos_);
                continue;
            }
            if (text_.compare(pos_, 2U, "<?") == 0)
            {
                return fail(line_, column_, "processing instructions are not allowed");
            }
            if (text_.compare(pos_, 2U, "<!") == 0)
            {
                return fail(line_, column_, "Dtd and declarations are not allowed");
            }
            return true;
        }
    }

    static bool isNameCharacter(char character)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '_' || character == '-' ||
               character == '.';
    }

    bool parseName(std::string& out)
    {
        const std::size_t begin = pos_;
        while (pos_ < text_.size() && isNameCharacter(text_[pos_]))
        {
            advance(1U);
        }
        if (pos_ == begin)
        {
            return fail(line_, column_, "expected a name");
        }
        out = std::string(text_.substr(begin, pos_ - begin));
        return true;
    }

    bool appendCodepoint(std::uint32_t codepoint, std::string& out)
    {
        if (codepoint == 0U || codepoint > 0x10FFFFU ||
            (codepoint >= 0xD800U && codepoint <= 0xDFFFU))
        {
            return false;
        }
        if (codepoint <= 0x7FU)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint <= 0x7FFU)
        {
            out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
        else if (codepoint <= 0xFFFFU)
        {
            out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
        return true;
    }

    bool decode(std::string_view raw, std::string& out, std::uint32_t line, std::uint32_t column)
    {
        for (std::size_t index = 0; index < raw.size(); ++index)
        {
            const char character = raw[index];
            if (character == '<')
            {
                return fail(line, column, "'<' is not allowed in an attribute value");
            }
            if (character != '&')
            {
                out.push_back(character);
                continue;
            }

            const std::size_t semicolon = raw.find(';', index + 1U);
            if (semicolon == std::string_view::npos)
            {
                return fail(line, column, "unterminated entity reference");
            }
            const std::string_view entity = raw.substr(index + 1U, semicolon - index - 1U);
            if (entity == "amp")
            {
                out.push_back('&');
            }
            else if (entity == "lt")
            {
                out.push_back('<');
            }
            else if (entity == "gt")
            {
                out.push_back('>');
            }
            else if (entity == "quot")
            {
                out.push_back('"');
            }
            else if (entity == "apos")
            {
                out.push_back('\'');
            }
            else if (!entity.empty() && entity[0] == '#')
            {
                std::uint32_t codepoint = 0;
                const bool hex = entity.size() > 1U && (entity[1] == 'x' || entity[1] == 'X');
                const std::string_view digits = entity.substr(hex ? 2U : 1U);
                if (digits.empty())
                {
                    return fail(line, column, "empty character reference");
                }
                const auto result =
                    std::from_chars(digits.data(), digits.data() + digits.size(), codepoint,
                                    hex ? 16 : 10);
                if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
                {
                    return fail(line, column, "invalid character reference");
                }
                if (!appendCodepoint(codepoint, out))
                {
                    return fail(line, column, "character reference is out of range");
                }
            }
            else
            {
                return fail(line, column, "unknown entity '&" + std::string(entity) + ";'");
            }
            index = semicolon;
        }
        return true;
    }

    Next parseTag(Tag& out)
    {
        out = Tag{};
        out.line = line_;
        out.column = column_;
        advance(1U); /* '<' */

        if (pos_ < text_.size() && text_[pos_] == '/')
        {
            out.closing = true;
            advance(1U);
            if (!parseName(out.name))
            {
                return Next::Error;
            }
            while (pos_ < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                advance(1U);
            }
            if (pos_ >= text_.size() || text_[pos_] != '>')
            {
                return failNext(line_, column_, "expected '>' after a closing tag");
            }
            advance(1U);
            return Next::Tag;
        }

        if (!parseName(out.name))
        {
            return Next::Error;
        }
        if (out.name == "xmlns")
        {
            return failNext(out.line, out.column, "namespaces are not allowed");
        }

        for (;;)
        {
            while (pos_ < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                advance(1U);
            }
            if (pos_ >= text_.size())
            {
                return failNext(out.line, out.column, "unterminated tag");
            }
            if (text_[pos_] == '>')
            {
                advance(1U);
                return Next::Tag;
            }
            if (text_[pos_] == '/')
            {
                advance(1U);
                if (pos_ >= text_.size() || text_[pos_] != '>')
                {
                    return failNext(line_, column_, "expected '>' after '/'");
                }
                advance(1U);
                out.self_closing = true;
                return Next::Tag;
            }

            const std::uint32_t attribute_line = line_;
            const std::uint32_t attribute_column = column_;
            Attribute attribute;
            if (!parseName(attribute.name))
            {
                return Next::Error;
            }
            if (attribute.name == "xmlns")
            {
                return failNext(attribute_line, attribute_column, "namespaces are not allowed");
            }
            for (const Attribute& existing : out.attributes)
            {
                if (existing.name == attribute.name)
                {
                    return failNext(attribute_line, attribute_column,
                                "attribute '" + attribute.name + "' is repeated");
                }
            }

            while (pos_ < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                advance(1U);
            }
            if (pos_ >= text_.size() || text_[pos_] != '=')
            {
                return failNext(attribute_line, attribute_column,
                            "attribute '" + attribute.name + "' has no value");
            }
            advance(1U);
            while (pos_ < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                advance(1U);
            }
            if (pos_ >= text_.size() || (text_[pos_] != '"' && text_[pos_] != '\''))
            {
                return failNext(attribute_line, attribute_column,
                            "attribute '" + attribute.name + "' is not quoted");
            }
            const char quote = text_[pos_];
            advance(1U);
            const std::size_t value_begin = pos_;
            while (pos_ < text_.size() && text_[pos_] != quote)
            {
                advance(1U);
            }
            if (pos_ >= text_.size())
            {
                return failNext(attribute_line, attribute_column,
                            "attribute '" + attribute.name + "' is not closed");
            }
            const std::string_view raw = text_.substr(value_begin, pos_ - value_begin);
            const std::uint32_t value_line = line_;
            const std::uint32_t value_column = column_;
            advance(1U); /* closing quote */
            if (!decode(raw, attribute.value, value_line, value_column))
            {
                return Next::Error;
            }
            out.attributes.push_back(std::move(attribute));
        }
    }

    std::string_view text_;
    std::vector<Diagnostic>* diagnostics_ = nullptr;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t column_ = 1;
};

const Attribute* findAttribute(const Tag& tag, const char* name)
{
    for (const Attribute& attribute : tag.attributes)
    {
        if (attribute.name == name)
        {
            return &attribute;
        }
    }
    return nullptr;
}

bool fail(std::vector<Diagnostic>& diagnostics, std::uint32_t line, std::uint32_t column,
          std::string message)
{
    diagnostics.push_back(
        Diagnostic{Diagnostic::Severity::Error, line, column, std::move(message)});
    return false;
}

bool parseUInt(std::string_view text, std::uint32_t& out)
{
    if (text.empty())
    {
        return false;
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::string_view> splitTokens(std::string_view text)
{
    std::vector<std::string_view> tokens;
    std::size_t index = 0;
    while (index < text.size())
    {
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index])) != 0)
        {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index])) == 0)
        {
            ++index;
        }
        if (index > begin)
        {
            tokens.push_back(text.substr(begin, index - begin));
        }
    }
    return tokens;
}

bool parseDocumentAttributes(Document& document, const Tag& tag,
                             std::vector<Diagnostic>& diagnostics)
{
    for (const Attribute& attribute : tag.attributes)
    {
        if (attribute.name == "version")
        {
            std::uint32_t version = 0;
            if (!parseUInt(attribute.value, version) || version == 0U)
            {
                return fail(diagnostics, tag.line, tag.column, "version must be a positive integer");
            }
            document.setFormatVersion(version);
        }
        else if (attribute.name == "schemas")
        {
            std::string_view remaining = attribute.value;
            while (!remaining.empty())
            {
                const std::size_t comma = remaining.find(',');
                std::string_view token =
                    comma == std::string_view::npos ? remaining : remaining.substr(0U, comma);
                while (!token.empty() &&
                       std::isspace(static_cast<unsigned char>(token.front())) != 0)
                {
                    token.remove_prefix(1U);
                }
                while (!token.empty() &&
                       std::isspace(static_cast<unsigned char>(token.back())) != 0)
                {
                    token.remove_suffix(1U);
                }
                if (!token.empty())
                {
                    const StringId schema = document.intern(token);
                    if (!document.addSchema(schema))
                    {
                        return fail(diagnostics, tag.line, tag.column,
                                    "schema '" + std::string(token) + "' is repeated");
                    }
                }
                if (comma == std::string_view::npos)
                {
                    break;
                }
                remaining.remove_prefix(comma + 1U);
            }
        }
        else
        {
            return fail(diagnostics, tag.line, tag.column,
                        "unknown attribute '" + attribute.name + "' on <document>");
        }
    }
    return true;
}

bool parseResources(Reader& reader, Document& document, const Tag& tag,
                    std::vector<Diagnostic>& diagnostics)
{
    if (!tag.attributes.empty())
    {
        return fail(diagnostics, tag.line, tag.column, "<resources> takes no attributes");
    }
    if (tag.self_closing)
    {
        return true;
    }
    for (;;)
    {
        Tag child;
        const Reader::Next next = reader.next(child);
        if (next == Reader::Next::Error)
        {
            return false;
        }
        if (next == Reader::Next::End)
        {
            return fail(diagnostics, tag.line, tag.column, "<resources> is not closed");
        }
        if (child.closing)
        {
            if (child.name != "resources")
            {
                return fail(diagnostics, child.line, child.column,
                            "unexpected </" + child.name + ">");
            }
            return true;
        }
        if (child.name != "asset")
        {
            return fail(diagnostics, child.line, child.column,
                        "unknown element <" + child.name + "> inside <resources>");
        }
        for (const Attribute& attribute : child.attributes)
        {
            if (attribute.name != "id" && attribute.name != "guid")
            {
                return fail(diagnostics, child.line, child.column,
                            "unknown attribute '" + attribute.name + "' on <asset>");
            }
        }
        const Attribute* alias = findAttribute(child, "id");
        const Attribute* guid = findAttribute(child, "guid");
        if (alias == nullptr || alias->value.empty() || guid == nullptr || guid->value.empty())
        {
            return fail(diagnostics, child.line, child.column,
                        "<asset> needs a non-empty id and guid");
        }
        Resource resource;
        resource.alias = document.intern(alias->value);
        resource.guid = guid->value;
        if (!document.addResource(std::move(resource)))
        {
            return fail(diagnostics, child.line, child.column,
                        "resource '" + alias->value + "' is repeated");
        }
        if (!child.self_closing)
        {
            Tag closing;
            const Reader::Next closing_next = reader.next(closing);
            if (closing_next == Reader::Next::Error)
            {
                return false;
            }
            if (closing_next != Reader::Next::Tag || !closing.closing ||
                closing.name != "asset")
            {
                return fail(diagnostics, child.line, child.column, "<asset> has no content");
            }
        }
    }
}

bool parseComponent(Reader& reader, Document& document, NodeHandle node, const Tag& tag,
                    bool explicit_form, std::vector<Diagnostic>& diagnostics)
{
    Component component;
    if (explicit_form)
    {
        const Attribute* type = findAttribute(tag, "type");
        if (type == nullptr || type->value.empty())
        {
            return fail(diagnostics, tag.line, tag.column, "<component> needs a type");
        }
        component.type = document.intern(type->value);
    }
    else
    {
        component.type = document.intern(tag.name);
    }
    if (component.type == kInvalidString)
    {
        return fail(diagnostics, tag.line, tag.column, "a component needs a type");
    }

    for (const Attribute& attribute : tag.attributes)
    {
        if (explicit_form && attribute.name == "type")
        {
            continue;
        }
        if (!explicit_form && attribute.name == "type")
        {
            return fail(diagnostics, tag.line, tag.column,
                        "attribute 'type' is reserved; use <component type=...> for a field named type");
        }
        component.fields.push_back(
            Field{document.intern(attribute.name), attribute.value});
    }
    if (document.hasComponent(node, component.type))
    {
        return fail(diagnostics, tag.line, tag.column,
                    "component '" + std::string(document.text(component.type)) + "' is repeated");
    }
    if (!document.setComponent(node, std::move(component)))
    {
        return fail(diagnostics, tag.line, tag.column, "component could not be recorded");
    }

    if (tag.self_closing)
    {
        return true;
    }
    for (;;)
    {
        Tag child;
        const Reader::Next next = reader.next(child);
        if (next == Reader::Next::Error)
        {
            return false;
        }
        if (next == Reader::Next::End)
        {
            return fail(diagnostics, tag.line, tag.column,
                        "<" + tag.name + "> is not closed");
        }
        if (child.closing)
        {
            if (child.name != tag.name)
            {
                return fail(diagnostics, child.line, child.column,
                            "unexpected </" + child.name + ">");
            }
            return true;
        }
        return fail(diagnostics, child.line, child.column,
                    "nested component values are not implemented; use a schema-bound field");
    }
}

NodeHandle parseNode(Reader& reader, Document& document, const Tag& tag, std::uint32_t depth,
                     std::vector<Diagnostic>& diagnostics, bool& ok)
{
    ok = false;
    if (depth > 1024U)
    {
        fail(diagnostics, tag.line, tag.column, "the node tree is too deep");
        return NodeHandle{};
    }

    const NodeHandle node = document.create();
    if (!node.valid())
    {
        fail(diagnostics, tag.line, tag.column, "a node could not be created");
        return NodeHandle{};
    }

    for (const Attribute& attribute : tag.attributes)
    {
        if (attribute.name == "key")
        {
            const StringId key = document.intern(attribute.value);
            if (key == kInvalidString || !document.setKey(node, key))
            {
                fail(diagnostics, tag.line, tag.column,
                     "key '" + attribute.value + "' is empty or repeated");
                return NodeHandle{};
            }
        }
        else if (attribute.name == "id")
        {
            const StringId id = document.intern(attribute.value);
            if (id == kInvalidString || !document.setId(node, id))
            {
                fail(diagnostics, tag.line, tag.column,
                     "id '" + attribute.value + "' is empty or repeated");
                return NodeHandle{};
            }
        }
        else if (attribute.name == "name")
        {
            document.setName(node, document.intern(attribute.value));
        }
        else if (attribute.name == "type")
        {
            const StringId type = document.intern(attribute.value);
            if (type == kInvalidString)
            {
                fail(diagnostics, tag.line, tag.column, "type must not be empty");
                return NodeHandle{};
            }
            document.setType(node, type);
        }
        else if (attribute.name == "class")
        {
            std::vector<StringId> classes;
            for (const std::string_view token : splitTokens(attribute.value))
            {
                const StringId value = document.intern(token);
                if (value == kInvalidString)
                {
                    fail(diagnostics, tag.line, tag.column, "class name must not be empty");
                    return NodeHandle{};
                }
                classes.push_back(value);
            }
            if (!document.setClasses(node, Span<const StringId>(classes.data(), classes.size())))
            {
                fail(diagnostics, tag.line, tag.column, "class list is invalid");
                return NodeHandle{};
            }
        }
        else
        {
            fail(diagnostics, tag.line, tag.column,
                 "unknown attribute '" + attribute.name + "' on <node>");
            return NodeHandle{};
        }
    }

    if (tag.self_closing)
    {
        ok = true;
        return node;
    }

    for (;;)
    {
        Tag child;
        const Reader::Next next = reader.next(child);
        if (next == Reader::Next::Error)
        {
            return NodeHandle{};
        }
        if (next == Reader::Next::End)
        {
            fail(diagnostics, tag.line, tag.column, "<node> is not closed");
            return NodeHandle{};
        }
        if (child.closing)
        {
            if (child.name != "node")
            {
                fail(diagnostics, child.line, child.column, "unexpected </" + child.name + ">");
                return NodeHandle{};
            }
            ok = true;
            return node;
        }
        if (child.name == "node")
        {
            bool child_ok = false;
            const NodeHandle child_node =
                parseNode(reader, document, child, depth + 1U, diagnostics, child_ok);
            if (!child_ok || !document.append(node, child_node))
            {
                return NodeHandle{};
            }
            continue;
        }
        if (child.name == "component")
        {
            if (!parseComponent(reader, document, node, child, true, diagnostics))
            {
                return NodeHandle{};
            }
            continue;
        }
        if (!parseComponent(reader, document, node, child, false, diagnostics))
        {
            return NodeHandle{};
        }
    }
}

} // namespace

std::unique_ptr<Document> Document::parse(std::string_view xml,
                                          std::vector<Diagnostic>& diagnostics)
{
    diagnostics.clear();
    auto document = std::make_unique<Document>();
    Reader reader(xml, diagnostics);
    if (!reader.skipDeclaration())
    {
        return nullptr;
    }

    Tag tag;
    Reader::Next next = reader.next(tag);
    if (next == Reader::Next::Error)
    {
        return nullptr;
    }
    if (next != Reader::Next::Tag || tag.closing || tag.name != "document")
    {
        fail(diagnostics, tag.line, tag.column, "expected <document>");
        return nullptr;
    }
    if (!parseDocumentAttributes(*document, tag, diagnostics))
    {
        return nullptr;
    }
    if (tag.self_closing)
    {
        fail(diagnostics, tag.line, tag.column, "<document> needs a root node");
        return nullptr;
    }

    bool have_root = false;
    bool closed = false;
    while (!closed)
    {
        next = reader.next(tag);
        if (next == Reader::Next::Error)
        {
            return nullptr;
        }
        if (next == Reader::Next::End)
        {
            fail(diagnostics, 0U, 0U, "<document> is not closed");
            return nullptr;
        }
        if (tag.closing)
        {
            if (tag.name != "document")
            {
                fail(diagnostics, tag.line, tag.column, "unexpected </" + tag.name + ">");
                return nullptr;
            }
            closed = true;
            continue;
        }
        if (tag.name == "resources")
        {
            if (!parseResources(reader, *document, tag, diagnostics))
            {
                return nullptr;
            }
            continue;
        }
        if (tag.name != "node")
        {
            fail(diagnostics, tag.line, tag.column,
                 "unknown element <" + tag.name + "> in <document>");
            return nullptr;
        }
        if (have_root)
        {
            fail(diagnostics, tag.line, tag.column, "a document has one root node");
            return nullptr;
        }
        bool root_ok = false;
        const NodeHandle root = parseNode(reader, *document, tag, 0U, diagnostics, root_ok);
        if (!root_ok || !document->setRoot(root))
        {
            return nullptr;
        }
        have_root = true;
    }

    next = reader.next(tag);
    if (next == Reader::Next::Error)
    {
        return nullptr;
    }
    if (next != Reader::Next::End)
    {
        fail(diagnostics, tag.line, tag.column, "content after </document>");
        return nullptr;
    }
    if (!have_root)
    {
        fail(diagnostics, 0U, 0U, "<document> needs a root node");
        return nullptr;
    }
    return document;
}

} // namespace resonate::dom
