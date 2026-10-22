#include <resonate/dom/abi.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <resonate/dom/document.h>

struct ResonateDomDocument
{
    resonate::dom::Document document;
};

namespace
{

using resonate::dom::kInvalidString;
using resonate::dom::NodeHandle;
using resonate::dom::StringId;

ResonateDomString makeString(StringId id)
{
    return ResonateDomString{id};
}

StringId takeString(ResonateDomString string)
{
    return string.id;
}

ResonateDomNode makeNode(NodeHandle handle)
{
    if (!handle.valid())
    {
        return ResonateDomNode{0};
    }
    return ResonateDomNode{static_cast<std::uint64_t>(handle.index) |
                           (static_cast<std::uint64_t>(handle.generation) << 32U)};
}

NodeHandle takeNode(ResonateDomNode node)
{
    if (node.value == 0U)
    {
        return NodeHandle{};
    }
    return NodeHandle{static_cast<std::uint32_t>(node.value),
                      static_cast<std::uint32_t>(node.value >> 32U)};
}

bool valid(const ResonateDomDocument* document)
{
    return document != nullptr;
}

bool valid(const ResonateDomDocument* document, NodeHandle node)
{
    return document != nullptr && document->document.alive(node);
}

StringId intern(ResonateDomDocument* document, const char* text, std::uint32_t size)
{
    if (document == nullptr || text == nullptr || size == 0U)
    {
        return kInvalidString;
    }
    return document->document.intern(std::string_view(text, size));
}

const resonate::dom::Component* findComponent(const ResonateDomDocument* document,
                                              NodeHandle node, StringId type)
{
    return document->document.component(node, type);
}

} // namespace

extern "C"
{

ResonateDomDocument* resonate_dom_create(void)
{
    return new (std::nothrow) ResonateDomDocument();
}

void resonate_dom_destroy(ResonateDomDocument* document)
{
    delete document;
}

ResonateStatus resonate_dom_parse(ResonateDomDocument* document, const char* utf8,
                                  uint32_t size_bytes, ResonateDomDiagnosticFn diagnostics,
                                  void* user)
{
    if (document == nullptr || utf8 == nullptr || size_bytes == 0U)
    {
        return RESONATE_E_INVALID;
    }

    std::vector<resonate::dom::Diagnostic> messages;
    auto parsed = resonate::dom::Document::parse(std::string_view(utf8, size_bytes), messages);
    if (parsed == nullptr)
    {
        if (diagnostics != nullptr)
        {
            for (const resonate::dom::Diagnostic& message : messages)
            {
                ResonateDomDiagnostic out{};
                out.struct_size = sizeof(out);
                out.severity =
                    message.severity == resonate::dom::Diagnostic::Severity::Error ? 1U : 0U;
                out.line = message.line;
                out.column = message.column;
                out.message = message.message.c_str();
                diagnostics(user, &out);
            }
        }
        return RESONATE_E_INVALID;
    }
    document->document = std::move(*parsed);
    return RESONATE_OK;
}

uint32_t resonate_dom_xml_size(const ResonateDomDocument* document)
{
    if (document == nullptr)
    {
        return 0U;
    }
    return static_cast<std::uint32_t>(document->document.toXml().size());
}

uint32_t resonate_dom_xml(const ResonateDomDocument* document, char* out_utf8, uint32_t capacity)
{
    if (document == nullptr)
    {
        return 0U;
    }
    const std::string xml = document->document.toXml();
    if (out_utf8 != nullptr && capacity != 0U)
    {
        std::memcpy(out_utf8, xml.data(), std::min<std::size_t>(capacity, xml.size()));
    }
    return static_cast<std::uint32_t>(xml.size());
}

uint64_t resonate_dom_revision(const ResonateDomDocument* document)
{
    return document != nullptr ? document->document.revision() : 0U;
}

ResonateDomString resonate_dom_intern(ResonateDomDocument* document, const char* utf8,
                                      uint32_t size_bytes)
{
    return makeString(intern(document, utf8, size_bytes));
}

const char* resonate_dom_text(const ResonateDomDocument* document, ResonateDomString string,
                              uint32_t* out_size)
{
    if (out_size != nullptr)
    {
        *out_size = 0U;
    }
    if (document == nullptr || string.id == kInvalidString)
    {
        return nullptr;
    }
    const std::string_view text = document->document.text(string.id);
    if (text.empty())
    {
        return nullptr;
    }
    if (out_size != nullptr)
    {
        *out_size = static_cast<std::uint32_t>(text.size());
    }
    return text.data();
}

ResonateDomNode resonate_dom_root(const ResonateDomDocument* document)
{
    return document != nullptr ? makeNode(document->document.root()) : ResonateDomNode{0};
}

ResonateStatus resonate_dom_set_root(ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr && document->document.setRoot(takeNode(node)) ? RESONATE_OK
                                                                             : RESONATE_E_INVALID;
}

int32_t resonate_dom_alive(const ResonateDomDocument* document, ResonateDomNode node)
{
    return valid(document, takeNode(node)) ? 1 : 0;
}

ResonateDomNode resonate_dom_parent(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? makeNode(document->document.parent(takeNode(node)))
                               : ResonateDomNode{0};
}

ResonateDomNode resonate_dom_first_child(const ResonateDomDocument* document,
                                         ResonateDomNode node)
{
    return document != nullptr ? makeNode(document->document.firstChild(takeNode(node)))
                               : ResonateDomNode{0};
}

ResonateDomNode resonate_dom_last_child(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? makeNode(document->document.lastChild(takeNode(node)))
                               : ResonateDomNode{0};
}

ResonateDomNode resonate_dom_next_sibling(const ResonateDomDocument* document,
                                          ResonateDomNode node)
{
    return document != nullptr ? makeNode(document->document.nextSibling(takeNode(node)))
                               : ResonateDomNode{0};
}

ResonateDomNode resonate_dom_previous_sibling(const ResonateDomDocument* document,
                                              ResonateDomNode node)
{
    return document != nullptr ? makeNode(document->document.previousSibling(takeNode(node)))
                               : ResonateDomNode{0};
}

uint32_t resonate_dom_child_count(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? document->document.childCount(takeNode(node)) : 0U;
}

ResonateDomNode resonate_dom_create_node(ResonateDomDocument* document, ResonateDomString type)
{
    return document != nullptr ? makeNode(document->document.create(takeString(type)))
                               : ResonateDomNode{0};
}

ResonateStatus resonate_dom_destroy_node(ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr && document->document.destroy(takeNode(node)) ? RESONATE_OK
                                                                             : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_detach(ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr && document->document.detach(takeNode(node)) ? RESONATE_OK
                                                                            : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_append(ResonateDomDocument* document, ResonateDomNode parent,
                                   ResonateDomNode child)
{
    return document != nullptr &&
                   document->document.append(takeNode(parent), takeNode(child))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_insert_before(ResonateDomDocument* document, ResonateDomNode parent,
                                          ResonateDomNode child, ResonateDomNode reference)
{
    return document != nullptr && document->document.insertBefore(
                                      takeNode(parent), takeNode(child), takeNode(reference))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_insert_after(ResonateDomDocument* document, ResonateDomNode parent,
                                         ResonateDomNode child, ResonateDomNode reference)
{
    return document != nullptr && document->document.insertAfter(
                                      takeNode(parent), takeNode(child), takeNode(reference))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_replace(ResonateDomDocument* document, ResonateDomNode parent,
                                    ResonateDomNode old_child, ResonateDomNode new_child)
{
    return document != nullptr && document->document.replace(
                                      takeNode(parent), takeNode(old_child), takeNode(new_child))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateDomNode resonate_dom_clone(ResonateDomDocument* document, ResonateDomNode node,
                                   int32_t copy_key, int32_t copy_id)
{
    if (document == nullptr)
    {
        return ResonateDomNode{0};
    }
    resonate::dom::CloneOptions options;
    options.copy_key = copy_key != 0;
    options.copy_id = copy_id != 0;
    return makeNode(document->document.clone(takeNode(node), options));
}

ResonateDomString resonate_dom_key(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? makeString(document->document.key(takeNode(node)))
                               : makeString(kInvalidString);
}

ResonateStatus resonate_dom_set_key(ResonateDomDocument* document, ResonateDomNode node,
                                    ResonateDomString key)
{
    return document != nullptr && document->document.setKey(takeNode(node), takeString(key))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateDomString resonate_dom_id(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? makeString(document->document.id(takeNode(node)))
                               : makeString(kInvalidString);
}

ResonateStatus resonate_dom_set_id(ResonateDomDocument* document, ResonateDomNode node,
                                   ResonateDomString id)
{
    return document != nullptr && document->document.setId(takeNode(node), takeString(id))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateDomString resonate_dom_name(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? makeString(document->document.name(takeNode(node)))
                               : makeString(kInvalidString);
}

ResonateStatus resonate_dom_set_name(ResonateDomDocument* document, ResonateDomNode node,
                                     ResonateDomString name)
{
    return document != nullptr && document->document.setName(takeNode(node), takeString(name))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateDomString resonate_dom_type(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? makeString(document->document.type(takeNode(node)))
                               : makeString(kInvalidString);
}

ResonateStatus resonate_dom_set_type(ResonateDomDocument* document, ResonateDomNode node,
                                     ResonateDomString type)
{
    return document != nullptr && document->document.setType(takeNode(node), takeString(type))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

uint32_t resonate_dom_class_count(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? static_cast<std::uint32_t>(
                                     document->document.classes(takeNode(node)).size())
                               : 0U;
}

ResonateDomString resonate_dom_class_at(const ResonateDomDocument* document, ResonateDomNode node,
                                        uint32_t index)
{
    if (document == nullptr)
    {
        return makeString(kInvalidString);
    }
    const auto classes = document->document.classes(takeNode(node));
    return index < classes.size() ? makeString(classes[index]) : makeString(kInvalidString);
}

ResonateStatus resonate_dom_set_classes(ResonateDomDocument* document, ResonateDomNode node,
                                        const ResonateDomString* classes, uint32_t count)
{
    if (document == nullptr || (classes == nullptr && count != 0U))
    {
        return RESONATE_E_INVALID;
    }
    std::vector<StringId> ids;
    ids.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        ids.push_back(takeString(classes[index]));
    }
    return document->document.setClasses(
               takeNode(node), resonate::Span<const StringId>(ids.data(), ids.size()))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

uint32_t resonate_dom_component_count(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? static_cast<std::uint32_t>(
                                     document->document.components(takeNode(node)).size())
                               : 0U;
}

ResonateDomString resonate_dom_component_type_at(const ResonateDomDocument* document,
                                                 ResonateDomNode node, uint32_t index)
{
    if (document == nullptr)
    {
        return makeString(kInvalidString);
    }
    const auto components = document->document.components(takeNode(node));
    return index < components.size() ? makeString(components[index].type)
                                     : makeString(kInvalidString);
}

uint32_t resonate_dom_field_count(const ResonateDomDocument* document, ResonateDomNode node,
                                  ResonateDomString component)
{
    if (document == nullptr)
    {
        return 0U;
    }
    const auto* found = findComponent(document, takeNode(node), takeString(component));
    return found != nullptr ? static_cast<std::uint32_t>(found->fields.size()) : 0U;
}

ResonateDomString resonate_dom_field_name_at(const ResonateDomDocument* document,
                                             ResonateDomNode node, ResonateDomString component,
                                             uint32_t index)
{
    if (document == nullptr)
    {
        return makeString(kInvalidString);
    }
    const auto* found = findComponent(document, takeNode(node), takeString(component));
    return found != nullptr && index < found->fields.size() ? makeString(found->fields[index].name)
                                                           : makeString(kInvalidString);
}

const char* resonate_dom_field_value_at(const ResonateDomDocument* document, ResonateDomNode node,
                                        ResonateDomString component, uint32_t index,
                                        uint32_t* out_size)
{
    if (out_size != nullptr)
    {
        *out_size = 0U;
    }
    if (document == nullptr)
    {
        return nullptr;
    }
    const auto* found = findComponent(document, takeNode(node), takeString(component));
    if (found == nullptr || index >= found->fields.size())
    {
        return nullptr;
    }
    if (out_size != nullptr)
    {
        *out_size = static_cast<std::uint32_t>(found->fields[index].value.size());
    }
    return found->fields[index].value.c_str();
}

ResonateStatus resonate_dom_set_field(ResonateDomDocument* document, ResonateDomNode node,
                                      ResonateDomString component, ResonateDomString field,
                                      const char* value, uint32_t size_bytes)
{
    if (document == nullptr || value == nullptr || takeString(component) == kInvalidString ||
        takeString(field) == kInvalidString)
    {
        return RESONATE_E_INVALID;
    }
    const NodeHandle handle = takeNode(node);
    if (!document->document.alive(handle))
    {
        return RESONATE_E_INVALID;
    }
    resonate::dom::Component next;
    if (const auto* existing =
            document->document.component(handle, takeString(component));
        existing != nullptr)
    {
        next = *existing;
    }
    else
    {
        next.type = takeString(component);
    }
    for (resonate::dom::Field& entry : next.fields)
    {
        if (entry.name == takeString(field))
        {
            entry.value.assign(value, size_bytes);
            return document->document.setComponent(handle, std::move(next)) ? RESONATE_OK
                                                                             : RESONATE_E_INVALID;
        }
    }
    next.fields.push_back(resonate::dom::Field{takeString(field),
                                               std::string(value, size_bytes)});
    return document->document.setComponent(handle, std::move(next)) ? RESONATE_OK
                                                                     : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_remove_component(ResonateDomDocument* document, ResonateDomNode node,
                                             ResonateDomString component)
{
    return document != nullptr &&
                   document->document.removeComponent(takeNode(node), takeString(component))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

uint32_t resonate_dom_property_count(const ResonateDomDocument* document, ResonateDomNode node)
{
    return document != nullptr ? static_cast<std::uint32_t>(
                                     document->document.properties(takeNode(node)).size())
                               : 0U;
}

ResonateDomString resonate_dom_property_name_at(const ResonateDomDocument* document,
                                                ResonateDomNode node, uint32_t index)
{
    if (document == nullptr)
    {
        return makeString(kInvalidString);
    }
    const auto properties = document->document.properties(takeNode(node));
    return index < properties.size() ? makeString(properties[index].name)
                                     : makeString(kInvalidString);
}

const char* resonate_dom_property_value_at(const ResonateDomDocument* document,
                                           ResonateDomNode node, uint32_t index,
                                           uint32_t* out_size)
{
    if (out_size != nullptr)
    {
        *out_size = 0U;
    }
    if (document == nullptr)
    {
        return nullptr;
    }
    const auto properties = document->document.properties(takeNode(node));
    if (index >= properties.size())
    {
        return nullptr;
    }
    if (out_size != nullptr)
    {
        *out_size = static_cast<std::uint32_t>(properties[index].value.size());
    }
    return properties[index].value.c_str();
}

ResonateStatus resonate_dom_set_property(ResonateDomDocument* document, ResonateDomNode node,
                                         ResonateDomString name, const char* value,
                                         uint32_t size_bytes)
{
    if (document == nullptr || value == nullptr || takeString(name) == kInvalidString)
    {
        return RESONATE_E_INVALID;
    }
    return document->document.setProperty(
               takeNode(node),
               resonate::dom::Field{takeString(name), std::string(value, size_bytes)})
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateStatus resonate_dom_remove_property(ResonateDomDocument* document, ResonateDomNode node,
                                            ResonateDomString name)
{
    return document != nullptr &&
                   document->document.removeProperty(takeNode(node), takeString(name))
               ? RESONATE_OK
               : RESONATE_E_INVALID;
}

ResonateDomNode resonate_dom_find_key(const ResonateDomDocument* document, ResonateDomString key)
{
    return document != nullptr ? makeNode(document->document.findKey(takeString(key)))
                               : ResonateDomNode{0};
}

ResonateDomNode resonate_dom_find_id(const ResonateDomDocument* document, ResonateDomString id)
{
    return document != nullptr ? makeNode(document->document.findId(takeString(id)))
                               : ResonateDomNode{0};
}

uint32_t resonate_dom_find_type_count(const ResonateDomDocument* document, ResonateDomString type)
{
    return document != nullptr
               ? static_cast<std::uint32_t>(
                     document->document.findType(takeString(type)).size())
               : 0U;
}

uint32_t resonate_dom_find_type_fill(const ResonateDomDocument* document, ResonateDomString type,
                                     ResonateDomNode* out_nodes, uint32_t capacity)
{
    if (document == nullptr)
    {
        return 0U;
    }
    const std::vector<NodeHandle> found = document->document.findType(takeString(type));
    const std::uint32_t count = static_cast<std::uint32_t>(found.size());
    if (out_nodes != nullptr)
    {
        const std::uint32_t copied = capacity < count ? capacity : count;
        for (std::uint32_t index = 0; index < copied; ++index)
        {
            out_nodes[index] = makeNode(found[index]);
        }
    }
    return count;
}

uint32_t resonate_dom_find_class_count(const ResonateDomDocument* document,
                                       ResonateDomString class_id)
{
    return document != nullptr
               ? static_cast<std::uint32_t>(
                     document->document.findClass(takeString(class_id)).size())
               : 0U;
}

uint32_t resonate_dom_find_class_fill(const ResonateDomDocument* document,
                                      ResonateDomString class_id, ResonateDomNode* out_nodes,
                                      uint32_t capacity)
{
    if (document == nullptr)
    {
        return 0U;
    }
    const std::vector<NodeHandle> found = document->document.findClass(takeString(class_id));
    const std::uint32_t count = static_cast<std::uint32_t>(found.size());
    if (out_nodes != nullptr)
    {
        const std::uint32_t copied = capacity < count ? capacity : count;
        for (std::uint32_t index = 0; index < copied; ++index)
        {
            out_nodes[index] = makeNode(found[index]);
        }
    }
    return count;
}

} // extern "C"
