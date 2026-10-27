#ifndef RESONATE_UI_WIDGET_SHOWN_H
#define RESONATE_UI_WIDGET_SHOWN_H

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui::detail
{

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
