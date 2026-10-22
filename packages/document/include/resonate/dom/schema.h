#ifndef RESONATE_DOM_SCHEMA_H
#define RESONATE_DOM_SCHEMA_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <resonate/core/span.h>

/*
 * The node-type vocabulary a document parser resolves against.
 *
 * A node type is a named composition: a list of components it implies plus
 * the component fields exposed as node attributes (the implicit syntax). The
 * registry is deliberately read-only and virtual only at this boundary; the
 * generated UI table and the test tables implement it without any widget class
 * hierarchy.
 */
namespace resonate::dom
{

struct ExposedField
{
    const char* attribute = nullptr;
    const char* component = nullptr;
    const char* field = nullptr;
};

struct UsedComponent
{
    const char* component = nullptr;
    Span<const ExposedField> exposed;
};

/* A node-type field the domain compiler consumes as text (a selector, a binding
   expression). It is not chunk data and has no component mapping. */
struct AuthoringField
{
    const char* name = nullptr;
    const char* type = nullptr;
};

struct NodeTypeDesc
{
    /* The element name used by implicit syntax and by `type=`. */
    const char* name = nullptr;
    /* The module-qualified name; null when the short name is the only one. */
    const char* qualified_name = nullptr;
    Span<const UsedComponent> components;
    Span<const AuthoringField> authoring;
};

/* One field of a storage component: enough for a generic authoring binder to
   parse the text and place the bytes. */
struct ComponentFieldDesc
{
    const char* name = nullptr;
    const char* type = nullptr;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct ComponentDesc
{
    const char* name = nullptr;
    const char* qualified_name = nullptr;
    std::uint32_t size = 0;
    std::uint32_t align = 0;
    std::uint32_t derived = 0;
    Span<const ComponentFieldDesc> fields;
};

class SchemaRegistry
{
  public:
    SchemaRegistry() = default;
    virtual ~SchemaRegistry() = default;

    SchemaRegistry(const SchemaRegistry&) = delete;
    SchemaRegistry& operator=(const SchemaRegistry&) = delete;

    /* Resolves a short or qualified node type name. */
    [[nodiscard]] virtual const NodeTypeDesc* findNodeType(std::string_view name) const = 0;
    /* Resolves a short or qualified component type name. */
    [[nodiscard]] virtual bool isComponent(std::string_view name) const = 0;
};

/* The table a generated module header exposes; the registry only reads it. */
class TableSchemaRegistry final : public SchemaRegistry
{
  public:
    TableSchemaRegistry(Span<const NodeTypeDesc> node_types,
                        Span<const char* const> components);

    [[nodiscard]] const NodeTypeDesc* findNodeType(std::string_view name) const override;
    [[nodiscard]] bool isComponent(std::string_view name) const override;

  private:
    Span<const NodeTypeDesc> node_types_;
    Span<const char* const> components_;
};

/* Several modules' tables behind one registry; first match wins. */
class CompositeSchemaRegistry final : public SchemaRegistry
{
  public:
    explicit CompositeSchemaRegistry(Span<const SchemaRegistry* const> registries);

    [[nodiscard]] const NodeTypeDesc* findNodeType(std::string_view name) const override;
    [[nodiscard]] bool isComponent(std::string_view name) const override;

  private:
    Span<const SchemaRegistry* const> registries_;
};

/* The component-side table: field layouts for the generic authoring binder. */
class ComponentRegistry
{
  public:
    ComponentRegistry() = default;
    virtual ~ComponentRegistry() = default;

    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;

    [[nodiscard]] virtual const ComponentDesc* find(std::string_view name) const = 0;
};

class TableComponentRegistry final : public ComponentRegistry
{
  public:
    explicit TableComponentRegistry(Span<const ComponentDesc> components);

    [[nodiscard]] const ComponentDesc* find(std::string_view name) const override;

  private:
    Span<const ComponentDesc> components_;
};

class CompositeComponentRegistry final : public ComponentRegistry
{
  public:
    explicit CompositeComponentRegistry(Span<const ComponentRegistry* const> registries);

    [[nodiscard]] const ComponentDesc* find(std::string_view name) const override;

  private:
    Span<const ComponentRegistry* const> registries_;
};

} // namespace resonate::dom

#endif /* RESONATE_DOM_SCHEMA_H */
