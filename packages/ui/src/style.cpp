#include <resonate/ui/style.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <map>

#include <resonate/hierarchy/tree.h>
#include <resonate/ui/document_schema.h>

namespace resonate::ui
{
namespace
{

void trimInPlace(std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        text.clear();
        return;
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1U);
}

/* Substitutes var(--token[, fallback]) references; an unknown token without a
   fallback is a compile error, like any other style mistake. */
bool substituteVariables(const std::map<std::string, std::string>& variables,
                         const std::string& value, std::string& out, std::string& error)
{
    out.clear();
    std::size_t index = 0;
    while (index < value.size())
    {
        const std::size_t start = value.find("var(", index);
        if (start == std::string::npos)
        {
            out.append(value, index, std::string::npos);
            return true;
        }
        out.append(value, index, start - index);
        const std::size_t end = value.find(')', start);
        if (end == std::string::npos)
        {
            error = "unterminated var( in \"" + value + "\"";
            return false;
        }
        std::string call = value.substr(start + 4U, end - start - 4U);
        std::string fallback;
        bool has_fallback = false;
        if (const std::size_t comma = call.find(','); comma != std::string::npos)
        {
            fallback = call.substr(comma + 1U);
            call = call.substr(0U, comma);
            has_fallback = true;
            trimInPlace(fallback);
        }
        trimInPlace(call);
        const auto found = variables.find(call);
        if (found != variables.end())
        {
            out.append(found->second);
        }
        else if (has_fallback)
        {
            out.append(fallback);
        }
        else
        {
            error = "unknown theme variable \"" + call + "\"";
            return false;
        }
        index = end + 1U;
    }
    return true;
}

std::string_view baseName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1U);
}

bool isNodeType(const dom::Document& document, dom::NodeHandle node, std::string_view name)
{
    const dom::StringId type = document.type(node);
    return type != dom::kInvalidString && baseName(document.text(type)) == name;
}

bool isIdentifierCharacter(char character)
{
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_' || character == '-';
}

struct Selector
{
    std::string type;
    std::vector<dom::StringId> classes;
    dom::StringId id = dom::kInvalidString;
    std::uint32_t state = 0U;
};

bool parseSelector(dom::Document& document, std::string_view text, Selector& out,
                   std::string& error)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0)
    {
        --end;
    }
    text = text.substr(begin, end - begin);
    if (text.empty())
    {
        error = "the selector is empty";
        return false;
    }

    std::size_t index = 0;
    if (isIdentifierCharacter(text[index]))
    {
        const std::size_t type_begin = index;
        while (index < text.size() && isIdentifierCharacter(text[index]))
        {
            ++index;
        }
        out.type = std::string(text.substr(type_begin, index - type_begin));
    }

    while (index < text.size())
    {
        const char marker = text[index];
        if (marker == ':')
        {
            ++index;
            const std::size_t state_begin = index;
            while (index < text.size() && isIdentifierCharacter(text[index]))
            {
                ++index;
            }
            const std::string_view state = text.substr(state_begin, index - state_begin);
            if (state == "hover")
            {
                out.state = 1U;
            }
            else if (state == "pressed")
            {
                out.state = 2U;
            }
            else if (state == "disabled")
            {
                out.state = 3U;
            }
            else if (state == "focus")
            {
                out.state = 4U;
            }
            else
            {
                error = "unknown state pseudo-class ':" + std::string(state) + "'";
                return false;
            }
            if (index != text.size())
            {
                error = "a state pseudo-class ends the selector";
                return false;
            }
            break;
        }
        if (marker != '.' && marker != '#')
        {
            error = "unexpected character in selector; only Type.class#id is supported";
            return false;
        }
        ++index;
        const std::size_t name_begin = index;
        while (index < text.size() && isIdentifierCharacter(text[index]))
        {
            ++index;
        }
        if (index == name_begin)
        {
            error = marker == '.' ? "a class name is empty" : "an id is empty";
            return false;
        }
        const std::string_view name = text.substr(name_begin, index - name_begin);
        const dom::StringId id = document.intern(name);
        if (marker == '.')
        {
            out.classes.push_back(id);
        }
        else
        {
            if (out.id != dom::kInvalidString)
            {
                error = "a selector names one id";
                return false;
            }
            out.id = id;
        }
    }
    return true;
}

void collectStyleRules(dom::Document& document, dom::NodeHandle node,
                       std::vector<dom::NodeHandle>& out)
{
    if (isNodeType(document, node, "StyleRule"))
    {
        out.push_back(node);
    }
    for (dom::NodeHandle child = document.firstChild(node); child.valid();
         child = document.nextSibling(child))
    {
        collectStyleRules(document, child, out);
    }
}

/* Theme variables are data: every <Variable> under a sheet, the last
   definition in document order winning. */
void collectVariables(dom::Document& document, dom::NodeHandle node,
                      dom::StringId token_name, dom::StringId value_name,
                      std::map<std::string, std::string>& out)
{
    if (isNodeType(document, node, "Variable"))
    {
        const dom::Field* token = document.property(node, token_name);
        const dom::Field* value = document.property(node, value_name);
        if (token != nullptr && !token->value.empty() && value != nullptr)
        {
            out[token->value] = value->value;
        }
    }
    for (dom::NodeHandle child = document.firstChild(node); child.valid();
         child = document.nextSibling(child))
    {
        collectVariables(document, child, token_name, value_name, out);
    }
}

/* Recreates one node of a stylesheet under the target document: strings are
   interned there, so the imported rules compare against the document's own
   classes and ids. */
dom::NodeHandle importNode(dom::Document& target, dom::Document& source,
                           dom::NodeHandle source_node)
{
    const dom::StringId type = source.type(source_node);
    if (type == dom::kInvalidString)
    {
        return dom::NodeHandle{};
    }
    const dom::NodeHandle copy = target.create(target.intern(source.text(type)));
    if (!copy.valid())
    {
        return copy;
    }
    if (const dom::StringId key = source.key(source_node); key != dom::kInvalidString)
    {
        target.setKey(copy, target.intern(source.text(key)));
    }
    if (const dom::StringId id = source.id(source_node); id != dom::kInvalidString)
    {
        target.setId(copy, target.intern(source.text(id)));
    }
    if (const dom::StringId name = source.name(source_node); name != dom::kInvalidString)
    {
        target.setName(copy, target.intern(source.text(name)));
    }
    std::vector<dom::StringId> classes;
    for (const dom::StringId class_id : source.classes(source_node))
    {
        classes.push_back(target.intern(source.text(class_id)));
    }
    if (!classes.empty())
    {
        target.setClasses(copy, resonate::Span<const dom::StringId>(classes.data(), classes.size()));
    }
    for (const dom::Component& component : source.components(source_node))
    {
        dom::Component imported;
        imported.type = target.intern(source.text(component.type));
        imported.fields.reserve(component.fields.size());
        for (const dom::Field& field : component.fields)
        {
            imported.fields.push_back(
                dom::Field{target.intern(source.text(field.name)), field.value});
        }
        target.setComponent(copy, std::move(imported));
    }
    for (const dom::Field& property : source.properties(source_node))
    {
        target.setProperty(copy,
                           dom::Field{target.intern(source.text(property.name)), property.value});
    }
    for (dom::NodeHandle child = source.firstChild(source_node); child.valid();
         child = source.nextSibling(child))
    {
        const dom::NodeHandle imported_child = importNode(target, source, child);
        if (imported_child.valid())
        {
            target.append(copy, imported_child);
        }
    }
    return copy;
}

} // namespace

struct StyleTable::Impl
{
    struct Rule
    {
        Selector selector;
        std::int32_t specificity = 0;
        std::uint32_t order = 0U;
        std::vector<dom::Component> declarations;
    };

    std::vector<Rule> rules;

    [[nodiscard]] bool matches(const dom::Document& document, const Rule& rule,
                               dom::NodeHandle node) const
    {
        if (!rule.selector.type.empty())
        {
            const dom::StringId type = document.type(node);
            if (type == dom::kInvalidString ||
                baseName(document.text(type)) != rule.selector.type)
            {
                return false;
            }
        }
        if (rule.selector.id != dom::kInvalidString && document.id(node) != rule.selector.id)
        {
            return false;
        }
        const Span<const dom::StringId> classes = document.classes(node);
        for (const dom::StringId required : rule.selector.classes)
        {
            if (std::find(classes.begin(), classes.end(), required) == classes.end())
            {
                return false;
            }
        }
        return true;
    }
};

StyleTable::StyleTable() : impl_(std::make_unique<Impl>())
{
}

StyleTable::~StyleTable() = default;

StyleTable::StyleTable(StyleTable&&) noexcept = default;

StyleTable& StyleTable::operator=(StyleTable&&) noexcept = default;

bool StyleTable::empty() const noexcept
{
    return impl_->rules.empty();
}

std::vector<dom::Component> StyleTable::resolve(const dom::Document& document,
                                                dom::NodeHandle node,
                                                std::uint32_t state) const
{
    std::vector<dom::Component> result(document.components(node).begin(),
                                       document.components(node).end());
    if (impl_->rules.empty() || isStyleNode(document, node))
    {
        return result;
    }

    /* Fields present in the document are explicit and win over every rule. */
    std::vector<std::vector<dom::StringId>> explicit_fields(result.size());
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        explicit_fields[index].reserve(result[index].fields.size());
        for (const dom::Field& field : result[index].fields)
        {
            explicit_fields[index].push_back(field.name);
        }
    }

    for (const Impl::Rule& rule : impl_->rules)
    {
        if (rule.selector.state != 0U && rule.selector.state != state)
        {
            continue;
        }
        if (!impl_->matches(document, rule, node))
        {
            continue;
        }
        for (const dom::Component& declaration : rule.declarations)
        {
            std::size_t target = result.size();
            for (std::size_t index = 0; index < result.size(); ++index)
            {
                if (result[index].type == declaration.type)
                {
                    target = index;
                    break;
                }
            }
            if (target == result.size())
            {
                result.push_back(declaration);
                explicit_fields.emplace_back();
                continue;
            }

            for (const dom::Field& field : declaration.fields)
            {
                if (std::find(explicit_fields[target].begin(), explicit_fields[target].end(),
                              field.name) != explicit_fields[target].end())
                {
                    continue;
                }
                bool replaced = false;
                for (dom::Field& existing : result[target].fields)
                {
                    if (existing.name == field.name)
                    {
                        existing.value = field.value;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced)
                {
                    result[target].fields.push_back(field);
                }
            }
        }
    }
    return result;
}

bool buildStyles(dom::Document& document, StyleTable& out,
                 std::vector<dom::Diagnostic>& diagnostics)
{
    out = StyleTable{};
    const dom::StringId sheet_type = document.intern("StyleSheet");
    const dom::StringId selector_name = document.intern("selector");
    const std::vector<dom::NodeHandle> sheets = document.findType(sheet_type);

    std::vector<dom::NodeHandle> rule_nodes;
    std::map<std::string, std::string> variables;
    {
        const dom::StringId token_name = document.intern("token");
        const dom::StringId value_name = document.intern("value");
        for (const dom::NodeHandle sheet : sheets)
        {
            collectVariables(document, sheet, token_name, value_name, variables);
            collectStyleRules(document, sheet, rule_nodes);
        }
    }

    std::uint32_t order = 0U;
    for (const dom::NodeHandle node : rule_nodes)
    {
        const dom::Field* selector_field = document.property(node, selector_name);
        if (selector_field == nullptr || selector_field->value.empty())
        {
            diagnostics.push_back(dom::Diagnostic{dom::Diagnostic::Severity::Error, 0U, 0U,
                                                  "style rule has no selector"});
            continue;
        }
        StyleTable::Impl::Rule rule;
        std::string error;
        if (!parseSelector(document, selector_field->value, rule.selector, error))
        {
            diagnostics.push_back(dom::Diagnostic{dom::Diagnostic::Severity::Error, 0U, 0U,
                                                  "style rule: " + error});
            continue;
        }
        rule.specificity = (rule.selector.id != dom::kInvalidString ? 100 : 0) +
                           static_cast<std::int32_t>(rule.selector.classes.size()) * 10 +
                           (rule.selector.type.empty() ? 0 : 1) +
                           (rule.selector.state == 1U
                                ? 10
                                : (rule.selector.state == 0U ? 0 : 20));
        rule.order = order++;
        bool declarations_ok = true;
        for (const dom::Component& component : document.components(node))
        {
            if (baseName(document.text(component.type)) == "ComputedRect")
            {
                diagnostics.push_back(dom::Diagnostic{
                    dom::Diagnostic::Severity::Error, 0U, 0U,
                    "style rule declares a derived component (ComputedRect)"});
                continue;
            }
            dom::Component declaration = component;
            for (dom::Field& field : declaration.fields)
            {
                if (field.value.find("var(") == std::string::npos)
                {
                    continue;
                }
                std::string substituted;
                if (!substituteVariables(variables, field.value, substituted, error))
                {
                    diagnostics.push_back(dom::Diagnostic{dom::Diagnostic::Severity::Error, 0U,
                                                          0U, "style rule: " + error});
                    declarations_ok = false;
                    break;
                }
                field.value = std::move(substituted);
            }
            if (!declarations_ok)
            {
                break;
            }
            rule.declarations.push_back(std::move(declaration));
        }
        if (!declarations_ok)
        {
            continue;
        }
        out.impl_->rules.push_back(std::move(rule));
    }

    std::stable_sort(out.impl_->rules.begin(), out.impl_->rules.end(),
                     [](const StyleTable::Impl::Rule& left, const StyleTable::Impl::Rule& right)
                     {
                         if (left.specificity != right.specificity)
                         {
                             return left.specificity < right.specificity;
                         }
                         return left.order < right.order;
                     });
    return diagnostics.empty();
}

bool loadStylesheets(dom::Document& document, void* context, StylesheetReader reader,
                     std::vector<dom::Diagnostic>& diagnostics)
{
    if (reader == nullptr)
    {
        return true;
    }
    const dom::StringId sheet_type = document.intern("StyleSheet");
    const dom::StringId src_name = document.intern("src");
    const std::vector<dom::NodeHandle> sheets = document.findType(sheet_type);
    bool ok = true;
    for (const dom::NodeHandle sheet : sheets)
    {
        const dom::Field* src = document.property(sheet, src_name);
        if (src == nullptr || src->value.empty())
        {
            continue;
        }
        const std::string path = src->value;
        std::string text;
        if (!reader(context, path, text))
        {
            diagnostics.push_back(dom::Diagnostic{
                dom::Diagnostic::Severity::Error, 0U, 0U,
                "stylesheet '" + path + "' could not be read"});
            ok = false;
            continue;
        }
        std::vector<dom::Diagnostic> imported_diagnostics;
        auto imported = dom::Document::parse(text, &documentSchema(), imported_diagnostics);
        for (dom::Diagnostic& diagnostic : imported_diagnostics)
        {
            diagnostic.message = path + ": " + diagnostic.message;
            diagnostics.push_back(std::move(diagnostic));
        }
        if (imported == nullptr)
        {
            ok = false;
            continue;
        }
        const std::vector<dom::NodeHandle> imported_sheets =
            imported->findType(imported->intern("StyleSheet"));
        if (imported_sheets.empty())
        {
            diagnostics.push_back(dom::Diagnostic{dom::Diagnostic::Severity::Error, 0U, 0U,
                                                  "stylesheet '" + path +
                                                      "' carries no StyleSheet"});
            ok = false;
            continue;
        }
        const dom::NodeHandle parent = document.parent(sheet);
        dom::NodeHandle reference = sheet;
        for (const dom::NodeHandle imported_sheet : imported_sheets)
        {
            const dom::NodeHandle copy = importNode(document, *imported, imported_sheet);
            if (!copy.valid() || !parent.valid() ||
                !document.insertAfter(parent, copy, reference))
            {
                diagnostics.push_back(dom::Diagnostic{dom::Diagnostic::Severity::Error, 0U, 0U,
                                                      "stylesheet '" + path +
                                                          "' could not be imported"});
                ok = false;
                break;
            }
            reference = copy;
        }
        document.removeProperty(sheet, src_name);
    }
    return ok;
}

bool isStyleNode(const dom::Document& document, dom::NodeHandle node)
{
    return isNodeType(document, node, "StyleSheet") || isNodeType(document, node, "StyleRule");
}

} // namespace resonate::ui
