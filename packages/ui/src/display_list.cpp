#include <resonate/ui/display_list.h>

#include <algorithm>
#include <cstdint>
#include <utility>
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

bool report(ecs::World& world, ecs::Entity entity, const char* what)
{
    world.report(ecs::World::Report::Error, "ui display list: entity %u:%u %s", entity.index,
                 entity.generation, what);
    return false;
}

} // namespace

struct DisplayList::Impl
{
    std::vector<Quad> quads;
    std::vector<OrderEntry> order;
};

bool DisplayList::paint(ecs::World& world, ecs::Entity entity, std::uint32_t depth)
{
    if (depth > hierarchy::kMaxDepth)
    {
        return report(world, entity, "sits below the UI tree's depth limit");
    }

    if (const Background* background = world.get<Background>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Background but no ComputedRect");
        }
        impl_->quads.push_back(
            Quad{rect->x, rect->y, rect->width, rect->height, background->color});
    }

    const auto children = hierarchy::children(world, entity);
    const std::size_t start = impl_->order.size();
    for (const ecs::Entity child : children)
    {
        const RectTransform* transform = world.get<RectTransform>(child);
        impl_->order.push_back(OrderEntry{child, transform != nullptr ? transform->z : 0});
    }
    const std::size_t end = impl_->order.size();
    std::stable_sort(impl_->order.begin() + static_cast<std::ptrdiff_t>(start),
                     impl_->order.begin() + static_cast<std::ptrdiff_t>(end),
                     [](const OrderEntry& left, const OrderEntry& right)
                     { return left.z < right.z; });

    /* The range stays valid across recursion: a child appends its own entries
       after `end`, never inside this one. */
    for (std::size_t index = start; index < end; ++index)
    {
        const ecs::Entity child = impl_->order[index].entity;
        if (!paint(world, child, depth + 1U))
        {
            return false;
        }
    }
    return true;
}

DisplayList::DisplayList() : impl_(std::make_unique<Impl>())
{
}

DisplayList::~DisplayList() = default;

DisplayList::DisplayList(DisplayList&&) noexcept = default;

DisplayList& DisplayList::operator=(DisplayList&&) noexcept = default;

void DisplayList::clear()
{
    impl_->quads.clear();
    impl_->order.clear();
}

void DisplayList::push(const Quad& quad)
{
    impl_->quads.push_back(quad);
}

bool DisplayList::empty() const noexcept
{
    return impl_->quads.empty();
}

std::size_t DisplayList::size() const noexcept
{
    return impl_->quads.size();
}

Span<const Quad> DisplayList::quads() const noexcept
{
    return Span<const Quad>(impl_->quads.data(), impl_->quads.size());
}

bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out)
{
    out.clear();
    return out.paint(world, root, 0U);
}

} // namespace resonate::ui
