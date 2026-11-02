/* The cooked document codec.
 *
 * A document loads much more often than it is authored, so the shipped form is
 * a table walk instead of an XML parse: strings once, nodes in slot order, each
 * node's identity, components, properties and children by index. The header is
 * self-describing (codec version, document format version, schemas, resources),
 * and the format is versioned because the XML form and this one are one model
 * with two codecs.
 *
 * Layout (little-endian):
 *
 *   "RDOM" | u32 codec | u32 document format version
 *   | u32 string count, each: u32 length, bytes
 *   | u32 schema count, each u32 string id
 *   | u32 resource count, each: u32 alias id, u32 guid length, bytes
 *   | u32 node count
 *   | each node: u32 type, key, id, name | u32 class count, ids
 *       | u32 component count, each: u32 type, u32 field count,
 *           each: u32 name, u32 length, bytes
 *       | u32 property count, each: u32 name, u32 length, bytes
 *       | u32 child count, payload indices
 *   | u32 root payload index, or 0xFFFFFFFF for none
 */

#include <resonate/dom/document.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace resonate::dom
{
namespace
{

constexpr char kMagic[4] = {'R', 'D', 'O', 'M'};
constexpr std::uint32_t kCodecVersion = 1U;
constexpr std::uint32_t kNoIndex = 0xFFFFFFFFU;

struct Writer
{
    std::vector<std::byte> bytes;

    void u32(std::uint32_t value)
    {
        for (int byte = 0; byte < 4; ++byte)
        {
            bytes.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void text(std::string_view value)
    {
        u32(static_cast<std::uint32_t>(value.size()));
        const auto* first = reinterpret_cast<const std::byte*>(value.data());
        bytes.insert(bytes.end(), first, first + value.size());
    }

    void id(StringId value)
    {
        u32(value);
    }
};

struct Reader
{
    Span<const std::byte> data;
    std::size_t at = 0;

    [[nodiscard]] bool take(std::size_t size, Span<const std::byte>& out)
    {
        if (size > data.size() - at)
        {
            return false;
        }
        out = Span<const std::byte>(data.data() + at, size);
        at += size;
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& out)
    {
        Span<const std::byte> raw;
        if (!take(4U, raw))
        {
            return false;
        }
        out = 0U;
        for (std::size_t byte = 0U; byte < 4U; ++byte)
        {
            out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raw[byte]))
                   << (8U * static_cast<unsigned>(byte));
        }
        return true;
    }

    [[nodiscard]] bool text(std::string& out)
    {
        std::uint32_t size = 0U;
        Span<const std::byte> raw;
        if (!u32(size) || !take(size, raw))
        {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
        return true;
    }
};

void addDiagnostic(std::vector<Diagnostic>& diagnostics, const std::string& message)
{
    Diagnostic diagnostic;
    diagnostic.severity = Diagnostic::Severity::Error;
    diagnostic.message = message;
    diagnostics.push_back(std::move(diagnostic));
}

} // namespace

bool Document::isBinary(Span<const std::byte> bytes) noexcept
{
    if (bytes.size() < sizeof(kMagic))
    {
        return false;
    }
    return std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

std::vector<std::byte> Document::toBinary() const
{
    const std::vector<NodeHandle> live = nodes();
    std::unordered_map<std::uint32_t, std::uint32_t> payload_of;
    payload_of.reserve(live.size());
    for (std::uint32_t index = 0U; index < live.size(); ++index)
    {
        payload_of.emplace(live[index].index, index);
    }

    Writer writer;
    writer.bytes.insert(writer.bytes.end(),
                        reinterpret_cast<const std::byte*>(kMagic),
                        reinterpret_cast<const std::byte*>(kMagic) + sizeof(kMagic));
    writer.u32(kCodecVersion);
    writer.u32(formatVersion());

    writer.u32(stringCount());
    for (StringId id = 0U; id < stringCount(); ++id)
    {
        writer.text(text(id));
    }

    writer.u32(static_cast<std::uint32_t>(schemas().size()));
    for (const StringId schema : schemas())
    {
        writer.id(schema);
    }

    writer.u32(static_cast<std::uint32_t>(resources().size()));
    for (const Resource& resource : resources())
    {
        writer.id(resource.alias);
        writer.text(resource.guid);
    }

    writer.u32(static_cast<std::uint32_t>(live.size()));
    for (const NodeHandle node : live)
    {
        writer.id(type(node));
        writer.id(key(node));
        writer.id(id(node));
        writer.id(name(node));

        const Span<const StringId> classes = this->classes(node);
        writer.u32(static_cast<std::uint32_t>(classes.size()));
        for (const StringId class_id : classes)
        {
            writer.id(class_id);
        }

        const Span<const Component> node_components = components(node);
        writer.u32(static_cast<std::uint32_t>(node_components.size()));
        for (const Component& component : node_components)
        {
            writer.id(component.type);
            writer.u32(static_cast<std::uint32_t>(component.fields.size()));
            for (const Field& field : component.fields)
            {
                writer.id(field.name);
                writer.text(field.value);
            }
        }

        const Span<const Field> node_properties = properties(node);
        writer.u32(static_cast<std::uint32_t>(node_properties.size()));
        for (const Field& property : node_properties)
        {
            writer.id(property.name);
            writer.text(property.value);
        }

        writer.u32(childCount(node));
        for (NodeHandle child = firstChild(node); child.valid(); child = nextSibling(child))
        {
            writer.u32(payload_of[child.index]);
        }
    }

    const NodeHandle document_root = root();
    writer.u32(document_root.valid() ? payload_of[document_root.index] : kNoIndex);
    return std::move(writer.bytes);
}

std::unique_ptr<Document> Document::parseBinary(Span<const std::byte> bytes,
                                                std::vector<Diagnostic>& diagnostics)
{
    if (bytes.size() < sizeof(kMagic) + 8U)
    {
        addDiagnostic(diagnostics, "the cooked document is truncated");
        return nullptr;
    }
    if (!isBinary(bytes))
    {
        addDiagnostic(diagnostics, "the payload is not a cooked document");
        return nullptr;
    }

    Reader reader{bytes, sizeof(kMagic)};
    std::uint32_t codec = 0U;
    std::uint32_t format = 0U;
    std::uint32_t string_count = 0U;
    if (!reader.u32(codec) || !reader.u32(format) || !reader.u32(string_count))
    {
        addDiagnostic(diagnostics, "the cooked document header is truncated");
        return nullptr;
    }
    if (codec != kCodecVersion)
    {
        addDiagnostic(diagnostics, "the cooked document codec " + std::to_string(codec) +
                                       " is not the one this build reads");
        return nullptr;
    }
    if (string_count > reader.data.size() - reader.at)
    {
        addDiagnostic(diagnostics, "the cooked document's string table is truncated");
        return nullptr;
    }

    auto document = std::make_unique<Document>();
    document->setFormatVersion(format);
    for (std::uint32_t index = 0U; index < string_count; ++index)
    {
        std::string value;
        if (!reader.text(value))
        {
            addDiagnostic(diagnostics, "the cooked document's string table is truncated");
            return nullptr;
        }
        const StringId interned = document->intern(value);
        if (interned != index)
        {
            addDiagnostic(diagnostics,
                          "the cooked document's string table has a duplicate or empty entry");
            return nullptr;
        }
    }

    std::uint32_t schema_count = 0U;
    if (!reader.u32(schema_count) || schema_count > (reader.data.size() - reader.at) / 4U)
    {
        addDiagnostic(diagnostics, "the cooked document's schema list is truncated");
        return nullptr;
    }
    for (std::uint32_t index = 0U; index < schema_count; ++index)
    {
        std::uint32_t schema = 0U;
        if (!reader.u32(schema) || schema >= string_count)
        {
            addDiagnostic(diagnostics, "the cooked document names an unknown schema string");
            return nullptr;
        }
        if (!document->addSchema(schema))
        {
            addDiagnostic(diagnostics, "the cooked document repeats a schema");
            return nullptr;
        }
    }

    std::uint32_t resource_count = 0U;
    if (!reader.u32(resource_count) || resource_count > (reader.data.size() - reader.at) / 8U)
    {
        addDiagnostic(diagnostics, "the cooked document's resource table is truncated");
        return nullptr;
    }
    for (std::uint32_t index = 0U; index < resource_count; ++index)
    {
        std::uint32_t alias = 0U;
        std::string guid;
        if (!reader.u32(alias) || !reader.text(guid) || alias >= string_count)
        {
            addDiagnostic(diagnostics, "the cooked document's resource table is malformed");
            return nullptr;
        }
        Resource resource;
        resource.alias = alias;
        resource.guid = std::move(guid);
        if (!document->addResource(resource))
        {
            addDiagnostic(diagnostics, "the cooked document repeats a resource alias");
            return nullptr;
        }
    }

    std::uint32_t node_count = 0U;
    if (!reader.u32(node_count) || node_count > (reader.data.size() - reader.at) / 28U)
    {
        addDiagnostic(diagnostics, "the cooked document's node table is truncated");
        return nullptr;
    }
    std::vector<NodeHandle> handles;
    handles.reserve(node_count);
    std::vector<std::vector<std::uint32_t>> child_lists;
    child_lists.reserve(node_count);
    for (std::uint32_t index = 0U; index < node_count; ++index)
    {
        std::uint32_t type = kInvalidString;
        std::uint32_t key = kInvalidString;
        std::uint32_t id = kInvalidString;
        std::uint32_t name = kInvalidString;
        if (!reader.u32(type) || !reader.u32(key) || !reader.u32(id) || !reader.u32(name))
        {
            addDiagnostic(diagnostics, "the cooked document's node table is truncated");
            return nullptr;
        }
        const auto known = [string_count](std::uint32_t value)
        { return value == kInvalidString || value < string_count; };
        if (!known(type) || !known(key) || !known(id) || !known(name))
        {
            addDiagnostic(diagnostics, "the cooked document names an unknown node string");
            return nullptr;
        }

        const NodeHandle node = document->create(type);
        if (!node.valid() || !document->setKey(node, key) || !document->setId(node, id) ||
            !document->setName(node, name))
        {
            addDiagnostic(diagnostics, "the cooked document repeats a node key or id");
            return nullptr;
        }
        handles.push_back(node);

        std::uint32_t class_count = 0U;
        if (!reader.u32(class_count) || class_count > (reader.data.size() - reader.at) / 4U)
        {
            addDiagnostic(diagnostics, "the cooked document's class list is truncated");
            return nullptr;
        }
        std::vector<StringId> classes;
        classes.reserve(class_count);
        for (std::uint32_t slot = 0U; slot < class_count; ++slot)
        {
            std::uint32_t class_id = kInvalidString;
            if (!reader.u32(class_id) || !known(class_id))
            {
                addDiagnostic(diagnostics, "the cooked document names an unknown class string");
                return nullptr;
            }
            classes.push_back(class_id);
        }
        if (!document->setClasses(node, Span<const StringId>(classes.data(), classes.size())))
        {
            addDiagnostic(diagnostics, "the cooked document's class list is malformed");
            return nullptr;
        }

        std::uint32_t component_count = 0U;
        if (!reader.u32(component_count) || component_count > (reader.data.size() - reader.at) / 8U)
        {
            addDiagnostic(diagnostics, "the cooked document's component list is truncated");
            return nullptr;
        }
        for (std::uint32_t slot = 0U; slot < component_count; ++slot)
        {
            Component component;
            std::uint32_t field_count = 0U;
            if (!reader.u32(component.type) || !reader.u32(field_count) ||
                component.type >= string_count ||
                field_count > (reader.data.size() - reader.at) / 8U)
            {
                addDiagnostic(diagnostics, "the cooked document's component list is truncated");
                return nullptr;
            }
            component.fields.reserve(field_count);
            for (std::uint32_t entry = 0U; entry < field_count; ++entry)
            {
                Field field;
                if (!reader.u32(field.name) || !reader.text(field.value) ||
                    field.name >= string_count)
                {
                    addDiagnostic(diagnostics, "the cooked document names an unknown field");
                    return nullptr;
                }
                component.fields.push_back(std::move(field));
            }
            if (!document->setComponent(node, std::move(component)))
            {
                addDiagnostic(diagnostics, "the cooked document repeats a component type");
                return nullptr;
            }
        }

        std::uint32_t property_count = 0U;
        if (!reader.u32(property_count) || property_count > (reader.data.size() - reader.at) / 8U)
        {
            addDiagnostic(diagnostics, "the cooked document's property list is truncated");
            return nullptr;
        }
        for (std::uint32_t slot = 0U; slot < property_count; ++slot)
        {
            Field property;
            if (!reader.u32(property.name) || !reader.text(property.value) ||
                property.name >= string_count)
            {
                addDiagnostic(diagnostics, "the cooked document names an unknown property");
                return nullptr;
            }
            if (!document->setProperty(node, std::move(property)))
            {
                addDiagnostic(diagnostics, "the cooked document repeats or misnames a property");
                return nullptr;
            }
        }

        std::uint32_t child_count = 0U;
        if (!reader.u32(child_count) || child_count > (reader.data.size() - reader.at) / 4U)
        {
            addDiagnostic(diagnostics, "the cooked document's child lists are truncated");
            return nullptr;
        }
        std::vector<std::uint32_t> children;
        children.reserve(child_count);
        for (std::uint32_t slot = 0U; slot < child_count; ++slot)
        {
            std::uint32_t child = 0U;
            if (!reader.u32(child) || child >= node_count)
            {
                addDiagnostic(diagnostics, "the cooked document names an unknown child node");
                return nullptr;
            }
            children.push_back(child);
        }
        child_lists.push_back(std::move(children));
    }

    /* Children are attached after every node exists: a cooked file does not
       promise that a parent's slot precedes its children's. */
    for (std::uint32_t index = 0U; index < node_count; ++index)
    {
        for (const std::uint32_t child : child_lists[index])
        {
            if (!document->append(handles[index], handles[child]))
            {
                addDiagnostic(diagnostics,
                              "the cooked document's child lists do not form a tree");
                return nullptr;
            }
        }
    }

    std::uint32_t root_index = kNoIndex;
    if (!reader.u32(root_index))
    {
        addDiagnostic(diagnostics, "the cooked document's root is truncated");
        return nullptr;
    }
    if (root_index != kNoIndex)
    {
        if (root_index >= node_count || !document->setRoot(handles[root_index]))
        {
            addDiagnostic(diagnostics, "the cooked document's root is not a free node");
            return nullptr;
        }
    }
    if (reader.at != bytes.size())
    {
        addDiagnostic(diagnostics, "the cooked document has trailing bytes");
        return nullptr;
    }
    return document;
}

} // namespace resonate::dom
