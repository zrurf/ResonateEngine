#include <resonate/dom/document.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <utility>

namespace resonate::dom
{

struct Document::Impl
{
    struct Slot
    {
        NodeHandle self;
        StringId key = kInvalidString;
        StringId id = kInvalidString;
        StringId name = kInvalidString;
        StringId type = kInvalidString;
        std::vector<StringId> classes;
        NodeHandle parent;
        NodeHandle first;
        NodeHandle last;
        NodeHandle prev;
        NodeHandle next;
        std::vector<Component> components;
        std::vector<Field> properties;
        bool alive = false;
    };

    std::uint32_t format_version = 1;
    std::vector<StringId> schemas;
    std::vector<Resource> resources;

    /* deque keeps a string's storage stable while the table grows, so a
       string_view returned by text() lives as long as the document. */
    std::deque<std::string> strings;
    std::unordered_map<std::string, StringId> interned;

    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_slots;
    NodeHandle root;
    std::unordered_map<StringId, NodeHandle> keys;
    std::unordered_map<StringId, NodeHandle> ids;
    std::uint64_t revision = 0;

    [[nodiscard]] Slot* slot(NodeHandle node) noexcept
    {
        if (node.index >= slots.size())
        {
            return nullptr;
        }
        Slot& found = slots[node.index];
        return found.alive && found.self.generation == node.generation ? &found : nullptr;
    }

    [[nodiscard]] const Slot* slot(NodeHandle node) const noexcept
    {
        if (node.index >= slots.size())
        {
            return nullptr;
        }
        const Slot& found = slots[node.index];
        return found.alive && found.self.generation == node.generation ? &found : nullptr;
    }

    [[nodiscard]] bool alive(NodeHandle node) const noexcept
    {
        return slot(node) != nullptr;
    }

    void touch() noexcept
    {
        ++revision;
    }

    NodeHandle allocate(StringId type)
    {
        std::uint32_t index = 0;
        if (!free_slots.empty())
        {
            index = free_slots.back();
            free_slots.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(slots.size());
            slots.emplace_back();
        }

        const std::uint32_t generation =
            slots[index].self.generation == 0 ? 1U : slots[index].self.generation;
        Slot fresh;
        fresh.self = NodeHandle{index, generation};
        fresh.type = type;
        fresh.alive = true;
        slots[index] = std::move(fresh);
        touch();
        return slots[index].self;
    }

    void release(Slot& slot)
    {
        if (slot.key != kInvalidString)
        {
            keys.erase(slot.key);
        }
        if (slot.id != kInvalidString)
        {
            ids.erase(slot.id);
        }
        slot.parent = NodeHandle{};
        slot.first = NodeHandle{};
        slot.last = NodeHandle{};
        slot.prev = NodeHandle{};
        slot.next = NodeHandle{};
        slot.key = kInvalidString;
        slot.id = kInvalidString;
        slot.name = kInvalidString;
        slot.type = kInvalidString;
        slot.classes.clear();
        slot.components.clear();
        slot.properties.clear();
        slot.alive = false;
        slot.self.generation += 1U;
        free_slots.push_back(slot.self.index);
        touch();
    }

    void unlink(Slot& slot)
    {
        Slot* const parent_slot = this->slot(slot.parent);
        if (parent_slot != nullptr)
        {
            if (parent_slot->first == slot.self)
            {
                parent_slot->first = slot.next;
            }
            if (parent_slot->last == slot.self)
            {
                parent_slot->last = slot.prev;
            }
        }
        if (Slot* const previous = this->slot(slot.prev))
        {
            previous->next = slot.next;
        }
        if (Slot* const next = this->slot(slot.next))
        {
            next->prev = slot.prev;
        }
        slot.parent = NodeHandle{};
        slot.prev = NodeHandle{};
        slot.next = NodeHandle{};
    }

    void linkBefore(Slot& parent_slot, Slot& child, Slot& reference)
    {
        child.parent = parent_slot.self;
        child.prev = reference.prev;
        child.next = reference.self;
        if (Slot* const previous = this->slot(reference.prev))
        {
            previous->next = child.self;
        }
        else
        {
            parent_slot.first = child.self;
        }
        reference.prev = child.self;
    }

    void linkAfter(Slot& parent_slot, Slot& child, Slot& reference)
    {
        child.parent = parent_slot.self;
        child.prev = reference.self;
        child.next = reference.next;
        if (Slot* const next = this->slot(reference.next))
        {
            next->prev = child.self;
        }
        else
        {
            parent_slot.last = child.self;
        }
        reference.next = child.self;
    }

    void linkLast(Slot& parent_slot, Slot& child)
    {
        child.parent = parent_slot.self;
        child.prev = parent_slot.last;
        child.next = NodeHandle{};
        if (Slot* const previous = this->slot(parent_slot.last))
        {
            previous->next = child.self;
        }
        else
        {
            parent_slot.first = child.self;
        }
        parent_slot.last = child.self;
    }

    [[nodiscard]] bool isAncestor(NodeHandle ancestor, NodeHandle node) const noexcept
    {
        NodeHandle current = node;
        for (std::uint32_t guard = 0; guard < (1U << 20U); ++guard)
        {
            const Slot* found = slot(current);
            if (found == nullptr || !found->parent.valid())
            {
                return false;
            }
            if (found->parent == ancestor)
            {
                return true;
            }
            current = found->parent;
        }
        return false;
    }

    void destroySubtree(NodeHandle node)
    {
        Slot* found = slot(node);
        if (found == nullptr)
        {
            return;
        }
        while (found->first.valid())
        {
            destroySubtree(found->first);
            found = slot(node);
            if (found == nullptr)
            {
                return;
            }
        }
        if (root == node)
        {
            root = NodeHandle{};
        }
        unlink(*found);
        release(*found);
    }

    NodeHandle cloneNode(Document& self, NodeHandle source, const CloneOptions& options)
    {
        const Slot* found = slot(source);
        if (found == nullptr)
        {
            return NodeHandle{};
        }
        /* allocate() may grow the slot vector, so the source is copied before
           any new slot exists. */
        const Slot original = *found;

        const NodeHandle copy = allocate(original.type);
        Slot* target = slot(copy);
        if (target == nullptr)
        {
            return NodeHandle{};
        }
        target->name = options.copy_name ? original.name : kInvalidString;
        if (options.copy_classes)
        {
            target->classes = original.classes;
        }
        target->components = original.components;
        target->properties = original.properties;
        if (options.copy_key)
        {
            if (!self.setKey(copy, original.key))
            {
                destroySubtree(copy);
                return NodeHandle{};
            }
        }
        if (options.copy_id)
        {
            if (!self.setId(copy, original.id))
            {
                destroySubtree(copy);
                return NodeHandle{};
            }
        }

        for (NodeHandle child = original.first; child.valid(); child = slot(child)->next)
        {
            const NodeHandle child_copy = cloneNode(self, child, options);
            if (!child_copy.valid() || !self.append(copy, child_copy))
            {
                destroySubtree(copy);
                return NodeHandle{};
            }
        }
        return copy;
    }

    template <typename Fn> void visitPreOrder(NodeHandle node, Fn&& fn) const
    {
        if (!slot(node))
        {
            return;
        }
        std::vector<NodeHandle> stack;
        stack.push_back(node);
        while (!stack.empty())
        {
            const NodeHandle current = stack.back();
            stack.pop_back();
            const Slot* found = slot(current);
            if (found == nullptr)
            {
                continue;
            }
            fn(current, *found);
            for (NodeHandle child = found->last; child.valid(); child = slot(child)->prev)
            {
                stack.push_back(child);
            }
        }
    }
};

Document::Document() : impl_(std::make_unique<Impl>())
{
}

Document::~Document() = default;

Document::Document(Document&&) noexcept = default;

Document& Document::operator=(Document&&) noexcept = default;

StringId Document::intern(std::string_view text)
{
    if (text.empty())
    {
        return kInvalidString;
    }
    const auto found = impl_->interned.find(std::string(text));
    if (found != impl_->interned.end())
    {
        return found->second;
    }
    const auto id = static_cast<StringId>(impl_->strings.size());
    impl_->strings.emplace_back(text);
    impl_->interned.emplace(impl_->strings.back(), id);
    return id;
}

std::string_view Document::text(StringId id) const noexcept
{
    if (id == kInvalidString || id >= impl_->strings.size())
    {
        return {};
    }
    return impl_->strings[id];
}

std::uint32_t Document::formatVersion() const noexcept
{
    return impl_->format_version;
}

void Document::setFormatVersion(std::uint32_t version)
{
    if (impl_->format_version == version)
    {
        return;
    }
    impl_->format_version = version;
    impl_->touch();
}

Span<const StringId> Document::schemas() const noexcept
{
    return Span<const StringId>(impl_->schemas.data(), impl_->schemas.size());
}

bool Document::addSchema(StringId schema)
{
    if (schema == kInvalidString ||
        std::find(impl_->schemas.begin(), impl_->schemas.end(), schema) != impl_->schemas.end())
    {
        return false;
    }
    impl_->schemas.push_back(schema);
    impl_->touch();
    return true;
}

Span<const Resource> Document::resources() const noexcept
{
    return Span<const Resource>(impl_->resources.data(), impl_->resources.size());
}

bool Document::addResource(Resource resource)
{
    if (resource.alias == kInvalidString || resource.guid.empty())
    {
        return false;
    }
    for (const Resource& existing : impl_->resources)
    {
        if (existing.alias == resource.alias)
        {
            return false;
        }
    }
    impl_->resources.push_back(std::move(resource));
    impl_->touch();
    return true;
}

NodeHandle Document::root() const noexcept
{
    return impl_->root;
}

bool Document::setRoot(NodeHandle node)
{
    if (!impl_->alive(node) || impl_->root.valid())
    {
        return false;
    }
    if (impl_->slot(node)->parent.valid())
    {
        return false;
    }
    impl_->root = node;
    impl_->touch();
    return true;
}

bool Document::alive(NodeHandle node) const noexcept
{
    return impl_->slot(node) != nullptr;
}

NodeHandle Document::create(StringId type)
{
    return impl_->allocate(type);
}

bool Document::destroy(NodeHandle node)
{
    if (!impl_->alive(node))
    {
        return false;
    }
    impl_->destroySubtree(node);
    return true;
}

bool Document::detach(NodeHandle node)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr || node == impl_->root || !slot->parent.valid())
    {
        return false;
    }
    impl_->unlink(*slot);
    impl_->touch();
    return true;
}

bool Document::append(NodeHandle parent, NodeHandle child)
{
    Impl::Slot* parent_slot = impl_->slot(parent);
    Impl::Slot* child_slot = impl_->slot(child);
    if (parent_slot == nullptr || child_slot == nullptr || child == impl_->root ||
        child == parent || impl_->isAncestor(child, parent))
    {
        return false;
    }
    impl_->unlink(*child_slot);
    impl_->linkLast(*parent_slot, *child_slot);
    impl_->touch();
    return true;
}

bool Document::insertBefore(NodeHandle parent, NodeHandle child, NodeHandle reference)
{
    Impl::Slot* parent_slot = impl_->slot(parent);
    Impl::Slot* child_slot = impl_->slot(child);
    Impl::Slot* reference_slot = impl_->slot(reference);
    if (parent_slot == nullptr || child_slot == nullptr || reference_slot == nullptr ||
        reference_slot->parent != parent || child == impl_->root || child == reference ||
        child == parent || impl_->isAncestor(child, parent))
    {
        return false;
    }
    impl_->unlink(*child_slot);
    impl_->linkBefore(*parent_slot, *child_slot, *reference_slot);
    impl_->touch();
    return true;
}

bool Document::insertAfter(NodeHandle parent, NodeHandle child, NodeHandle reference)
{
    Impl::Slot* parent_slot = impl_->slot(parent);
    Impl::Slot* child_slot = impl_->slot(child);
    Impl::Slot* reference_slot = impl_->slot(reference);
    if (parent_slot == nullptr || child_slot == nullptr || reference_slot == nullptr ||
        reference_slot->parent != parent || child == impl_->root || child == reference ||
        child == parent || impl_->isAncestor(child, parent))
    {
        return false;
    }
    impl_->unlink(*child_slot);
    impl_->linkAfter(*parent_slot, *child_slot, *reference_slot);
    impl_->touch();
    return true;
}

bool Document::replace(NodeHandle parent, NodeHandle old_child, NodeHandle new_child)
{
    Impl::Slot* parent_slot = impl_->slot(parent);
    Impl::Slot* old_slot = impl_->slot(old_child);
    Impl::Slot* new_slot = impl_->slot(new_child);
    if (parent_slot == nullptr || old_slot == nullptr || new_slot == nullptr ||
        old_slot->parent != parent || new_child == impl_->root || new_child == parent ||
        impl_->isAncestor(new_child, parent))
    {
        return false;
    }
    if (old_child == new_child)
    {
        return true;
    }

    impl_->unlink(*new_slot);
    new_slot->parent = parent;
    new_slot->prev = old_slot->prev;
    new_slot->next = old_slot->next;
    if (Impl::Slot* const previous = impl_->slot(old_slot->prev))
    {
        previous->next = new_child;
    }
    else
    {
        parent_slot->first = new_child;
    }
    if (Impl::Slot* const next = impl_->slot(old_slot->next))
    {
        next->prev = new_child;
    }
    else
    {
        parent_slot->last = new_child;
    }
    old_slot->parent = NodeHandle{};
    old_slot->prev = NodeHandle{};
    old_slot->next = NodeHandle{};
    impl_->touch();
    return true;
}

NodeHandle Document::parent(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->parent : NodeHandle{};
}

NodeHandle Document::firstChild(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->first : NodeHandle{};
}

NodeHandle Document::lastChild(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->last : NodeHandle{};
}

NodeHandle Document::nextSibling(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->next : NodeHandle{};
}

NodeHandle Document::previousSibling(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->prev : NodeHandle{};
}

std::uint32_t Document::childCount(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return 0U;
    }
    std::uint32_t count = 0U;
    for (NodeHandle child = slot->first; child.valid(); child = impl_->slot(child)->next)
    {
        ++count;
    }
    return count;
}

StringId Document::key(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->key : kInvalidString;
}

bool Document::setKey(NodeHandle node, StringId value)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    if (value == slot->key)
    {
        return true;
    }
    if (value != kInvalidString)
    {
        const auto found = impl_->keys.find(value);
        if (found != impl_->keys.end() && found->second != node)
        {
            return false;
        }
    }
    if (slot->key != kInvalidString)
    {
        impl_->keys.erase(slot->key);
    }
    slot->key = value;
    if (value != kInvalidString)
    {
        impl_->keys[value] = node;
    }
    impl_->touch();
    return true;
}

StringId Document::id(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->id : kInvalidString;
}

bool Document::setId(NodeHandle node, StringId value)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    if (value == slot->id)
    {
        return true;
    }
    if (value != kInvalidString)
    {
        const auto found = impl_->ids.find(value);
        if (found != impl_->ids.end() && found->second != node)
        {
            return false;
        }
    }
    if (slot->id != kInvalidString)
    {
        impl_->ids.erase(slot->id);
    }
    slot->id = value;
    if (value != kInvalidString)
    {
        impl_->ids[value] = node;
    }
    impl_->touch();
    return true;
}

StringId Document::name(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->name : kInvalidString;
}

bool Document::setName(NodeHandle node, StringId value)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    slot->name = value;
    impl_->touch();
    return true;
}

StringId Document::type(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    return slot != nullptr ? slot->type : kInvalidString;
}

bool Document::setType(NodeHandle node, StringId value)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    slot->type = value;
    impl_->touch();
    return true;
}

Span<const StringId> Document::classes(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return {};
    }
    return Span<const StringId>(slot->classes.data(), slot->classes.size());
}

bool Document::setClasses(NodeHandle node, Span<const StringId> classes)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    std::vector<StringId> next;
    next.reserve(classes.size());
    for (const StringId entry : classes)
    {
        if (entry == kInvalidString || entry >= impl_->strings.size())
        {
            return false;
        }
        next.push_back(entry);
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    slot->classes = std::move(next);
    impl_->touch();
    return true;
}

bool Document::hasComponent(NodeHandle node, StringId type) const noexcept
{
    return component(node, type) != nullptr;
}

const Component* Document::component(NodeHandle node, StringId type) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return nullptr;
    }
    for (const Component& entry : slot->components)
    {
        if (entry.type == type)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool Document::setComponent(NodeHandle node, Component component)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr || component.type == kInvalidString)
    {
        return false;
    }
    for (std::size_t index = 0; index < component.fields.size(); ++index)
    {
        if (component.fields[index].name == kInvalidString)
        {
            return false;
        }
        for (std::size_t other = index + 1U; other < component.fields.size(); ++other)
        {
            if (component.fields[index].name == component.fields[other].name)
            {
                return false;
            }
        }
    }
    for (Component& entry : slot->components)
    {
        if (entry.type == component.type)
        {
            entry = std::move(component);
            impl_->touch();
            return true;
        }
    }
    slot->components.push_back(std::move(component));
    impl_->touch();
    return true;
}

bool Document::removeComponent(NodeHandle node, StringId type)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    const auto found = std::find_if(slot->components.begin(), slot->components.end(),
                                    [type](const Component& entry) { return entry.type == type; });
    if (found == slot->components.end())
    {
        return false;
    }
    slot->components.erase(found);
    impl_->touch();
    return true;
}

Span<const Component> Document::components(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return {};
    }
    return Span<const Component>(slot->components.data(), slot->components.size());
}

const Field* Document::property(NodeHandle node, StringId name) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr || name == kInvalidString)
    {
        return nullptr;
    }
    for (const Field& field : slot->properties)
    {
        if (field.name == name)
        {
            return &field;
        }
    }
    return nullptr;
}

bool Document::setProperty(NodeHandle node, Field property)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr || property.name == kInvalidString)
    {
        return false;
    }
    for (Field& field : slot->properties)
    {
        if (field.name == property.name)
        {
            field.value = std::move(property.value);
            impl_->touch();
            return true;
        }
    }
    slot->properties.push_back(std::move(property));
    impl_->touch();
    return true;
}

bool Document::removeProperty(NodeHandle node, StringId name)
{
    Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return false;
    }
    const auto found = std::find_if(slot->properties.begin(), slot->properties.end(),
                                    [name](const Field& field) { return field.name == name; });
    if (found == slot->properties.end())
    {
        return false;
    }
    slot->properties.erase(found);
    impl_->touch();
    return true;
}

Span<const Field> Document::properties(NodeHandle node) const noexcept
{
    const Impl::Slot* slot = impl_->slot(node);
    if (slot == nullptr)
    {
        return {};
    }
    return Span<const Field>(slot->properties.data(), slot->properties.size());
}

NodeHandle Document::findKey(StringId key) const noexcept
{
    const auto found = impl_->keys.find(key);
    return found != impl_->keys.end() ? found->second : NodeHandle{};
}

NodeHandle Document::findId(StringId id) const noexcept
{
    const auto found = impl_->ids.find(id);
    return found != impl_->ids.end() ? found->second : NodeHandle{};
}

std::vector<NodeHandle> Document::findType(StringId type_id) const
{
    std::vector<NodeHandle> result;
    impl_->visitPreOrder(impl_->root,
                         [type_id, &result](NodeHandle node, const Impl::Slot& slot)
                         {
                             if (slot.type == type_id)
                             {
                                 result.push_back(node);
                             }
                         });
    return result;
}

std::vector<NodeHandle> Document::findClass(StringId class_id) const
{
    std::vector<NodeHandle> result;
    impl_->visitPreOrder(impl_->root,
                         [class_id, &result](NodeHandle node, const Impl::Slot& slot)
                         {
                             if (std::find(slot.classes.begin(), slot.classes.end(), class_id) !=
                                 slot.classes.end())
                             {
                                 result.push_back(node);
                             }
                         });
    return result;
}

NodeHandle Document::clone(NodeHandle source, const CloneOptions& options)
{
    return impl_->cloneNode(*this, source, options);
}

std::uint64_t Document::revision() const noexcept
{
    return impl_->revision;
}

} // namespace resonate::dom
