#include <resonate/ui/hit_test.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <resonate/hierarchy/tree.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

struct OrderEntry
{
    ecs::Entity entity;
    std::int32_t z = 0;
};

/* One scratch per thread: hit testing runs before anything else in a frame and
   the tree only grows, so the buffer is allocated once and reused. */
std::vector<OrderEntry>& scratch()
{
    static thread_local std::vector<OrderEntry> entries;
    return entries;
}

bool contains(const ComputedRect& rect, float x, float y)
{
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

ecs::Entity hit(ecs::World& world, ecs::Entity entity, float x, float y, std::uint32_t depth,
                std::vector<OrderEntry>& order)
{
    if (depth > hierarchy::kMaxDepth)
    {
        return ecs::Entity{};
    }

    const auto children = hierarchy::children(world, entity);
    const std::size_t start = order.size();
    for (const ecs::Entity child : children)
    {
        const RectTransform* transform = world.get<RectTransform>(child);
        order.push_back(OrderEntry{child, transform != nullptr ? transform->z : 0});
    }
    const std::size_t end = order.size();
    std::stable_sort(order.begin() + static_cast<std::ptrdiff_t>(start),
                     order.begin() + static_cast<std::ptrdiff_t>(end),
                     [](const OrderEntry& left, const OrderEntry& right)
                     { return left.z < right.z; });

    /* Reverse paint order: the last child drawn is the first considered. */
    for (std::size_t index = end; index > start; --index)
    {
        const ecs::Entity child = order[index - 1U].entity;
        const ecs::Entity found = hit(world, child, x, y, depth + 1U, order);
        if (found.valid())
        {
            return found;
        }
    }

    const ComputedRect* rect = world.get<ComputedRect>(entity);
    if (rect != nullptr && contains(*rect, x, y))
    {
        return entity;
    }
    return ecs::Entity{};
}

} // namespace

ecs::Entity hitTest(ecs::World& world, ecs::Entity root, float x, float y)
{
    std::vector<OrderEntry>& order = scratch();
    order.clear();
    return hit(world, root, x, y, 0U, order);
}

} // namespace resonate::ui
