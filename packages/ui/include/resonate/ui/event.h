#ifndef RESONATE_UI_EVENT_H
#define RESONATE_UI_EVENT_H

#include <cstdint>

#include <resonate/ecs/component.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

/*
 * Input events routed along the widget tree, DOM-style.
 *
 * The target is the widget under the pointer (or its nearest focusable ancestor
 * when that widget is not interactive). Capture runs from the canvas down to the
 * target, then the target itself, then the bubble phase back up to the canvas.
 * A handler answers true to consume the event, which stops the routing; the
 * built-in default actions (wheel scrolling a ScrollView) run after the bubble
 * phase only if nothing consumed the event.
 */
namespace resonate::ui
{

enum class EventKind : std::uint8_t
{
    PointerDown,
    PointerUp,
    PointerMove,
    Click,
    Wheel,
};

enum class EventPhase : std::uint8_t
{
    Capture,
    Target,
    Bubble,
};

/* The bit a listener uses to subscribe to one kind. */
inline constexpr std::uint16_t eventBit(EventKind kind)
{
    return static_cast<std::uint16_t>(1U << static_cast<unsigned>(kind));
}

struct Event
{
    EventKind kind = EventKind::PointerMove;
    /* Layout space, the same space the pointer arrives in. */
    float x = 0.0F;
    float y = 0.0F;
    /* Wheel detents; positive scrolls down. */
    float wheel = 0.0F;
    bool consumed = false;
};

/* Answers an event for one widget. Returning true consumes it: neither the rest
   of the phase nor the next phase runs. */
using EventHandler = bool (*)(void* context, ecs::World& world, ecs::Entity widget,
                              EventPhase phase, Event& event);

/* A widget's event subscription; plain data, so it is an ordinary ECS
   component that code attaches at runtime (documents never author one). */
struct Listener
{
    EventHandler handler = nullptr;
    void* context = nullptr;
    std::uint16_t kinds = 0U;
};

/* Routes one event. Returns true when a handler consumed it, including the
   built-in wheel scroll. `forced_target`, when valid, replaces the widget under
   the pointer — how a captured gesture keeps receiving its events. */
[[nodiscard]] bool dispatchEvent(ecs::World& world, ecs::Entity canvas, Event& event,
                                 ecs::Entity forced_target = {});

/* Takes the pointer for `entity` until the release: moves and the release are
   routed to it wherever the pointer goes, and the built-in gestures (scroll,
   slider, splitter, scrub) stand aside. An application handler calls this from
   a PointerDown. */
[[nodiscard]] bool capturePointer(ecs::World& world, ecs::Entity canvas, ecs::Entity entity);

/* Drops the capture early; the release does it too. */
void releasePointer(ecs::World& world, ecs::Entity canvas);

/* A key activation: dispatches a Click to the focused widget as if its centre
   were pressed and released — Enter on a focused button runs its default
   actions and listeners. False when nothing focusable has focus. */
[[nodiscard]] bool activateFocused(ecs::World& world, ecs::Entity canvas);

/* Touch-style scrolling: the first viewport on the chain from the widget under
   the point upward that can still move follows the finger. `pixels` is the
   finger's downward travel, so content tracks it one to one; a viewport clamped
   at its edge defers to its ancestor. Handlers are not involved — updateInput
   only gets here once the motion passed the drag slop. */
[[nodiscard]] bool dragScroll(ecs::World& world, ecs::Entity canvas, float x, float y,
                              float pixels);

} // namespace resonate::ui

namespace resonate::ecs
{

/* A listener is runtime data (a function pointer and a mask), never authored,
   so it has no schema declaration and names itself here. */
RESONATE_COMPONENT(resonate::ui::Listener, "resonate.ui.Listener");

} // namespace resonate::ecs

#endif /* RESONATE_UI_EVENT_H */
