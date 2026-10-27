#ifndef RESONATE_UI_WIDGET_SHOWN_H
#define RESONATE_UI_WIDGET_SHOWN_H

#include <algorithm>

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui::detail
{

/* The scrollbar thumb in target pixels: where the display list paints it and
   what the input system drags. A view whose content fits has none. */
struct ScrollThumb
{
    bool visible = false;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    /* Scroll units a pixel of thumb travel covers. */
    float scroll_per_pixel = 0.0F;
};

inline ScrollThumb scrollThumbOf(const ComputedRect& rect, const ScrollView& view)
{
    ScrollThumb thumb;
    const float range = view.content_height - rect.height;
    if (range <= 0.0F || view.color.w <= 0.0F || !(rect.height > 0.0F))
    {
        return thumb;
    }
    const float width = 6.0F;
    const float minimum = std::fmin(24.0F, rect.height);
    const float page = std::clamp(rect.height / view.content_height, 0.0F, 1.0F);
    const float height = std::fmax(minimum, rect.height * page);
    const float travel = rect.height - height;
    const float fraction = std::clamp(view.scroll / range, 0.0F, 1.0F);
    thumb.visible = true;
    thumb.width = width;
    thumb.height = height;
    thumb.x = rect.x + rect.width - width - 2.0F;
    thumb.y = rect.y + travel * fraction;
    thumb.scroll_per_pixel = travel > 0.0F ? range / travel : 0.0F;
    return thumb;
}

/* Whether a child takes part in painting and hit testing, given the widget
   state its parent carries: a Dropdown's popup only while open, a Tabs
   container only the page whose index is active. Layout still places hidden
   children; only their paint and their hits are dropped. */
inline bool childShown(ecs::World& world, ecs::Entity parent, ecs::Entity child)
{
    if (world.get<DropdownList>(child) != nullptr)
    {
        const Dropdown* dropdown = world.get<Dropdown>(parent);
        return dropdown != nullptr && dropdown->open;
    }
    if (const TabPage* page = world.get<TabPage>(child))
    {
        const Tabs* tabs = world.get<Tabs>(parent);
        return tabs != nullptr && tabs->active == page->index;
    }
    return true;
}

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_WIDGET_SHOWN_H */
