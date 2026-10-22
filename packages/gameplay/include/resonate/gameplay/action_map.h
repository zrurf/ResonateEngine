#ifndef RESONATE_GAMEPLAY_ACTION_MAP_H
#define RESONATE_GAMEPLAY_ACTION_MAP_H

/* Input actions as data: a .ractionmap document declares actions and the
   device controls bound to them, and the map samples the frame's raw input
   into pressed/held/released states. Gameplay reads actions, not keys, so a
   rebind is an asset change.

   Bindings exist per device (keyboard, mouse, gamepad); the gamepad source is
   not wired yet, so gamepad bindings validate and are kept but do not drive
   state. Phases in the document are accepted and currently implied by the
   digital state (pressed/held/released); analog values are a later slice. */

#include <cstddef>
#include <memory>
#include <string>

struct ResonateInput;

namespace resonate::gameplay
{

/* What one action looks like in the frame that just ended. */
struct ActionState
{
    bool pressed = false;  /* down now, up in the previous update */
    bool held = false;     /* down now through any binding */
    bool released = false; /* up now, down in the previous update */
};

class ActionMap final
{
  public:
    ActionMap();
    ~ActionMap();

    ActionMap(ActionMap&&) noexcept;
    ActionMap& operator=(ActionMap&&) noexcept;

    ActionMap(const ActionMap&) = delete;
    ActionMap& operator=(const ActionMap&) = delete;

    /* Reads and validates the whole document before replacing anything: an
       unknown element, attribute or control, a duplicate action, or a binding
       that names an undeclared action fails the load and leaves the map
       unchanged. */
    bool load(const std::string& path, std::string& error);

    /* Samples the raw input once per update. A null input (headless, no window)
       releases every action, so a frame without a device cannot leave a key
       stuck down. */
    void update(const ResonateInput* input);

    [[nodiscard]] std::size_t actionCount() const noexcept;

    /* The action's state, or null when the map has no such action. */
    [[nodiscard]] const ActionState* state(const char* action) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::gameplay

#endif /* RESONATE_GAMEPLAY_ACTION_MAP_H */
