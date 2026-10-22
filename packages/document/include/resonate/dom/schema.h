#ifndef RESONATE_DOM_SCHEMA_H
#define RESONATE_DOM_SCHEMA_H

#include <cstddef>
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

} // namespace resonate::dom

#endif /* RESONATE_DOM_SCHEMA_H */
