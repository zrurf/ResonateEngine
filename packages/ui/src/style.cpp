#include <resonate/ui/style.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <resonate/hierarchy/tree.h>

namespace resonate::ui
{
namespace
{

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
                                                dom::NodeHandle node) const
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
    for (const dom::NodeHandle sheet : sheets)
    {
        collectStyleRules(document, sheet, rule_nodes);
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
                           (rule.selector.type.empty() ? 0 : 1);
        rule.order = order++;
        for (const dom::Component& component : document.components(node))
        {
            if (baseName(document.text(component.type)) == "ComputedRect")
            {
                diagnostics.push_back(dom::Diagnostic{
                    dom::Diagnostic::Severity::Error, 0U, 0U,
                    "style rule declares a derived component (ComputedRect)"});
                continue;
            }
            rule.declarations.push_back(component);
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

bool isStyleNode(const dom::Document& document, dom::NodeHandle node)
{
    return isNodeType(document, node, "StyleSheet") || isNodeType(document, node, "StyleRule");
}

} // namespace resonate::ui
