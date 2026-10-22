#include <resonate/dom/schema.h>

#include <string_view>

namespace resonate::dom
{
namespace
{

std::string_view baseName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1U);
}

} // namespace

TableSchemaRegistry::TableSchemaRegistry(Span<const NodeTypeDesc> node_types,
                                         Span<const char* const> components)
    : node_types_(node_types), components_(components)
{
}

const NodeTypeDesc* TableSchemaRegistry::findNodeType(std::string_view name) const
{
    const std::string_view base = baseName(name);
    for (const NodeTypeDesc& type : node_types_)
    {
        if (type.name != nullptr && base == type.name)
        {
            return &type;
        }
    }
    return nullptr;
}

bool TableSchemaRegistry::isComponent(std::string_view name) const
{
    const std::string_view base = baseName(name);
    for (const char* const component : components_)
    {
        if (component != nullptr && base == component)
        {
            return true;
        }
    }
    return false;
}

CompositeSchemaRegistry::CompositeSchemaRegistry(Span<const SchemaRegistry* const> registries)
    : registries_(registries)
{
}

const NodeTypeDesc* CompositeSchemaRegistry::findNodeType(std::string_view name) const
{
    for (const SchemaRegistry* registry : registries_)
    {
        if (registry == nullptr)
        {
            continue;
        }
        if (const NodeTypeDesc* found = registry->findNodeType(name))
        {
            return found;
        }
    }
    return nullptr;
}

bool CompositeSchemaRegistry::isComponent(std::string_view name) const
{
    for (const SchemaRegistry* registry : registries_)
    {
        if (registry != nullptr && registry->isComponent(name))
        {
            return true;
        }
    }
    return false;
}

TableComponentRegistry::TableComponentRegistry(Span<const ComponentDesc> components)
    : components_(components)
{
}

const ComponentDesc* TableComponentRegistry::find(std::string_view name) const
{
    const std::string_view base = baseName(name);
    for (const ComponentDesc& component : components_)
    {
        if (component.name != nullptr && base == component.name)
        {
            return &component;
        }
    }
    return nullptr;
}

CompositeComponentRegistry::CompositeComponentRegistry(
    Span<const ComponentRegistry* const> registries)
    : registries_(registries)
{
}

const ComponentDesc* CompositeComponentRegistry::find(std::string_view name) const
{
    for (const ComponentRegistry* registry : registries_)
    {
        if (registry == nullptr)
        {
            continue;
        }
        if (const ComponentDesc* found = registry->find(name))
        {
            return found;
        }
    }
    return nullptr;
}

} // namespace resonate::dom
