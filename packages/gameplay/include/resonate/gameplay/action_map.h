#ifndef RESONATE_GAMEPLAY_ACTION_MAP_H
#define RESONATE_GAMEPLAY_ACTION_MAP_H

/* Input actions as data: a .ractionmap document declares actions and the
   device controls bound to them, and the map samples the frame's raw input
   into pressed/held/released states. Gameplay reads actions, not keys, so a
   rebind is an asset change.

   Keyboard, mouse and gamepad bindings all drive the digital states; a gamepad
   trigger counts as down past its halfway point. Analog values and rebind
   persistence are later slices. */

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

    /* Runtime rebinding. The change replaces the action's bindings on that
       device (other devices keep theirs) and is remembered as a user override;
       the base document is not edited. `unbind` removes that device's bindings
       for the action. Both fail on an unknown action, device or control and
       leave the map unchanged. */
    bool bind(const char* action, const char* device, const char* control, std::string& error);
    bool unbind(const char* action, const char* device, std::string& error);

    /* The user layer: a small <action-overrides> document with <binding> and
       <unbind> elements, applied on top of the base map. loadOverrides applies
       the whole file or nothing; saveOverrides writes the current overrides so
       a rebind survives a restart. */
    bool loadOverrides(const std::string& path, std::string& error);
    bool saveOverrides(const std::string& path, std::string& error) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::gameplay

#endif /* RESONATE_GAMEPLAY_ACTION_MAP_H */
