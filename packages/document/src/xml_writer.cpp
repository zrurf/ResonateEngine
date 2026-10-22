#include <resonate/dom/document.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace resonate::dom
{
namespace
{

void appendIndent(std::string& out, std::uint32_t level)
{
    out.append(static_cast<std::size_t>(level) * 2U, ' ');
}

void appendEscaped(std::string& out, std::string_view value)
{
    for (const char character : value)
    {
        switch (character)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        case '\n':
            out += "&#10;";
            break;
        case '\r':
            out += "&#13;";
            break;
        case '\t':
            out += "&#9;";
            break;
        default:
            out += character;
            break;
        }
    }
}

void appendAttribute(std::string& out, const char* name, std::string_view value)
{
    out += ' ';
    out += name;
    out += "=\"";
    appendEscaped(out, value);
    out += '"';
}

bool lessText(const Document& document, StringId left, StringId right)
{
    return document.text(left) < document.text(right);
}

std::string classesText(const Document& document, Span<const StringId> classes)
{
    std::vector<StringId> sorted(classes.begin(), classes.end());
    std::sort(sorted.begin(), sorted.end(),
              [&document](StringId left, StringId right)
              { return lessText(document, left, right); });
    std::string text;
    for (const StringId entry : sorted)
    {
        if (!text.empty())
        {
            text += ' ';
        }
        text += document.text(entry);
    }
    return text;
}

void writeNode(const Document& document, NodeHandle node, std::uint32_t level, std::string& out)
{
    if (!document.alive(node))
    {
        return;
    }

    appendIndent(out, level);
    out += "<node";
    const StringId key = document.key(node);
    const StringId id = document.id(node);
    const StringId name = document.name(node);
    const StringId type = document.type(node);
    if (key != kInvalidString)
    {
        appendAttribute(out, "key", document.text(key));
    }
    if (id != kInvalidString)
    {
        appendAttribute(out, "id", document.text(id));
    }
    if (name != kInvalidString)
    {
        appendAttribute(out, "name", document.text(name));
    }
    if (type != kInvalidString)
    {
        appendAttribute(out, "type", document.text(type));
    }
    const std::string classes = classesText(document, document.classes(node));
    if (!classes.empty())
    {
        appendAttribute(out, "class", classes);
    }
    const Span<const Field> properties = document.properties(node);
    std::vector<const Field*> ordered_properties;
    ordered_properties.reserve(properties.size());
    for (const Field& property : properties)
    {
        ordered_properties.push_back(&property);
    }
    std::sort(ordered_properties.begin(), ordered_properties.end(),
              [&document](const Field* left, const Field* right)
              { return lessText(document, left->name, right->name); });
    for (const Field* property : ordered_properties)
    {
        appendAttribute(out, std::string(document.text(property->name)).c_str(), property->value);
    }

    const Span<const Component> components = document.components(node);
    const NodeHandle first_child = document.firstChild(node);
    if (components.empty() && properties.empty() && !first_child.valid())
    {
        out += "/>\n";
        return;
    }

    out += ">\n";
    std::vector<const Component*> ordered;
    ordered.reserve(components.size());
    for (const Component& component : components)
    {
        ordered.push_back(&component);
    }
    std::sort(ordered.begin(), ordered.end(),
              [&document](const Component* left, const Component* right)
              { return lessText(document, left->type, right->type); });

    for (const Component* component : ordered)
    {
        appendIndent(out, level + 1U);
        out += "<component";
        appendAttribute(out, "type", document.text(component->type));
        std::vector<const Field*> fields;
        fields.reserve(component->fields.size());
        for (const Field& field : component->fields)
        {
            fields.push_back(&field);
        }
        std::sort(fields.begin(), fields.end(),
                  [&document](const Field* left, const Field* right)
                  { return lessText(document, left->name, right->name); });
        for (const Field* field : fields)
        {
            appendAttribute(out, std::string(document.text(field->name)).c_str(), field->value);
        }
        out += "/>\n";
    }

    for (NodeHandle child = first_child; child.valid(); child = document.nextSibling(child))
    {
        writeNode(document, child, level + 1U, out);
    }
    appendIndent(out, level);
    out += "</node>\n";
}

} // namespace

std::string Document::toXml() const
{
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out += "<document";
    appendAttribute(out, "version", std::to_string(formatVersion()));
    std::vector<StringId> schema_list(schemas().begin(), schemas().end());
    if (!schema_list.empty())
    {
        std::sort(schema_list.begin(), schema_list.end(),
                  [this](StringId left, StringId right) { return lessText(*this, left, right); });
        std::string schemas_text;
        for (const StringId schema : schema_list)
        {
            if (!schemas_text.empty())
            {
                schemas_text += ',';
            }
            schemas_text += text(schema);
        }
        appendAttribute(out, "schemas", schemas_text);
    }

    const Span<const Resource> resource_list = resources();
    const NodeHandle root_node = root();
    if (resource_list.empty() && !root_node.valid())
    {
        out += "/>\n";
        return out;
    }
    out += ">\n";

    if (!resource_list.empty())
    {
        appendIndent(out, 1U);
        out += "<resources>\n";
        std::vector<const Resource*> ordered;
        ordered.reserve(resource_list.size());
        for (const Resource& resource : resource_list)
        {
            ordered.push_back(&resource);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [this](const Resource* left, const Resource* right)
                  { return lessText(*this, left->alias, right->alias); });
        for (const Resource* resource : ordered)
        {
            appendIndent(out, 2U);
            out += "<asset";
            appendAttribute(out, "id", text(resource->alias));
            appendAttribute(out, "guid", resource->guid);
            out += "/>\n";
        }
        appendIndent(out, 1U);
        out += "</resources>\n";
    }

    writeNode(*this, root_node, 1U, out);
    out += "</document>\n";
    return out;
}

} // namespace resonate::dom
