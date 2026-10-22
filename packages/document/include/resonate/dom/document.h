#ifndef RESONATE_DOM_DOCUMENT_H
#define RESONATE_DOM_DOCUMENT_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/core/span.h>

/*
 * The generic document tree: nodes, components, identity and text codec.
 *
 * This is authoring/cooked data, not the runtime scene. A domain adapter
 * instantiates it into whatever its runtime needs (ECS entities for scene/UI);
 * the DOM itself knows no domain vocabulary. A node owns a slot in the
 * document, so handles survive moves and only die with the node.
 *
 * Identity is layered: NodeHandle is the runtime handle every operation uses;
 * `key` is the optional stable primary key patches and bindings anchor to;
 * `id` is the optional HTML/XAML-style query handle; `name` is a display
 * label. The design is in dev-note/design/25-dom.md.
 */
namespace resonate::dom
{

using StringId = std::uint32_t;
inline constexpr StringId kInvalidString = 0xFFFFFFFFU;

struct NodeHandle
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        return generation != 0;
    }

    friend constexpr bool operator==(NodeHandle left, NodeHandle right) noexcept
    {
        return left.index == right.index && left.generation == right.generation;
    }

    friend constexpr bool operator!=(NodeHandle left, NodeHandle right) noexcept
    {
        return !(left == right);
    }
};

struct Field
{
    StringId name = kInvalidString;
    std::string value;
};

struct Component
{
    StringId type = kInvalidString;
    std::vector<Field> fields;
};

struct Resource
{
    StringId alias = kInvalidString;
    std::string guid;
};

struct CloneOptions
{
    /* A clone is a new node by default: no key/id, so the copy cannot collide
       with or silently retarget patches that name the original. Prefab
       instancing, which wants the source keys preserved inside the instance,
       passes true. */
    bool copy_key = false;
    bool copy_id = false;
    bool copy_name = true;
    bool copy_classes = true;
};

struct Diagnostic
{
    enum class Severity : std::uint8_t
    {
        Warning = 0,
        Error = 1,
    };

    Severity severity = Severity::Error;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string message;
};

class Document
{
  public:
    Document();
    ~Document();

    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    /* --- strings --- */

    /* Empty text interned as the null string. */
    StringId intern(std::string_view text);
    [[nodiscard]] std::string_view text(StringId id) const noexcept;

    /* --- document header --- */

    [[nodiscard]] std::uint32_t formatVersion() const noexcept;
    void setFormatVersion(std::uint32_t version);

    [[nodiscard]] Span<const StringId> schemas() const noexcept;
    bool addSchema(StringId schema);

    [[nodiscard]] Span<const Resource> resources() const noexcept;
    bool addResource(Resource resource);

    /* --- nodes --- */

    [[nodiscard]] NodeHandle root() const noexcept;
    /* Sets the one root. False when a root is already set, or the node has a
       parent. */
    bool setRoot(NodeHandle node);

    NodeHandle create(StringId type = kInvalidString);
    /* Destroys the node and its subtree; handles to them become invalid. */
    bool destroy(NodeHandle node);
    [[nodiscard]] bool alive(NodeHandle node) const noexcept;

    /* Detaches from the parent, keeping the node and its subtree alive.
       The document root cannot be detached. */
    bool detach(NodeHandle node);

    bool append(NodeHandle parent, NodeHandle child);
    bool insertBefore(NodeHandle parent, NodeHandle child, NodeHandle reference);
    bool insertAfter(NodeHandle parent, NodeHandle child, NodeHandle reference);
    bool replace(NodeHandle parent, NodeHandle old_child, NodeHandle new_child);

    [[nodiscard]] NodeHandle parent(NodeHandle node) const noexcept;
    [[nodiscard]] NodeHandle firstChild(NodeHandle node) const noexcept;
    [[nodiscard]] NodeHandle lastChild(NodeHandle node) const noexcept;
    [[nodiscard]] NodeHandle nextSibling(NodeHandle node) const noexcept;
    [[nodiscard]] NodeHandle previousSibling(NodeHandle node) const noexcept;
    [[nodiscard]] std::uint32_t childCount(NodeHandle node) const noexcept;

    /* --- identity --- */

    [[nodiscard]] StringId key(NodeHandle node) const noexcept;
    bool setKey(NodeHandle node, StringId key);
    [[nodiscard]] StringId id(NodeHandle node) const noexcept;
    bool setId(NodeHandle node, StringId id);
    [[nodiscard]] StringId name(NodeHandle node) const noexcept;
    bool setName(NodeHandle node, StringId name);
    [[nodiscard]] StringId type(NodeHandle node) const noexcept;
    bool setType(NodeHandle node, StringId type);

    [[nodiscard]] Span<const StringId> classes(NodeHandle node) const noexcept;
    bool setClasses(NodeHandle node, Span<const StringId> classes);

    /* --- components --- */

    [[nodiscard]] bool hasComponent(NodeHandle node, StringId type) const noexcept;
    [[nodiscard]] const Component* component(NodeHandle node, StringId type) const noexcept;
    bool setComponent(NodeHandle node, Component component);
    bool removeComponent(NodeHandle node, StringId type);
    [[nodiscard]] Span<const Component> components(NodeHandle node) const noexcept;

    /* --- queries --- */

    /* Detached nodes remain findable by key/id. */
    [[nodiscard]] NodeHandle findKey(StringId key) const noexcept;
    [[nodiscard]] NodeHandle findId(StringId id) const noexcept;
    /* In document order (pre-order); detached nodes are not visited. */
    [[nodiscard]] std::vector<NodeHandle> findType(StringId type) const;
    [[nodiscard]] std::vector<NodeHandle> findClass(StringId class_id) const;

    /* --- clone --- */

    NodeHandle clone(NodeHandle source, const CloneOptions& options = {});

    /* --- revisions --- */

    [[nodiscard]] std::uint64_t revision() const noexcept;

    /* --- text codec --- */

    /* Parses the document grammar; returns null and fills diagnostics when
       the text is not a valid document. */
    static std::unique_ptr<Document> parse(std::string_view xml,
                                           std::vector<Diagnostic>& diagnostics);

    /* Canonical explicit form: stable ordering and formatting. */
    [[nodiscard]] std::string toXml() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::dom

#endif /* RESONATE_DOM_DOCUMENT_H */
