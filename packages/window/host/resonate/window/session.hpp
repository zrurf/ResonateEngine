#ifndef RESONATE_WINDOW_SESSION_HPP
#define RESONATE_WINDOW_SESSION_HPP

/*
 * A display the host composes when it wants one.
 *
 * The runtime does not need a window: a run without a session is headless, which
 * is what a dedicated server or an offscreen renderer is. What it does need is
 * for the window and input capabilities to be published *before* modules
 * resolve, which is why that lives here rather than in a caller.
 *
 * Host-side: a plugin resolves the capabilities, never this.
 */

#include <string>

#include <resonate/module/capability.h>

namespace resonate::window
{

class Session
{
  public:
    struct Config
    {
        std::string title = "Resonate";

        /* Back-buffer size in pixels. */
        int width = 1600;
        int height = 900;
    };

    Session() = default;
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /* Publishes the window, input and window-backed canvas capabilities, then
       creates the window. RESONATE_E_UNSUPPORTED when the platform has no display
       to open one on, RESONATE_E_INVALID for a size that cannot be passed on,
       RESONATE_E_STATE if this session was already started.

       Once per session: the capabilities stay published for as long as the
       registry lives, because the ABI promises a resolved capability is valid
       until the consumer detaches — a module holding one must not find it
       withdrawn mid-run. */
    ResonateStatus start(ResonateCapabilityRegistry* registry, const Config& config);

    /* Destroys the window. The capabilities stay published and report a
       zero-sized, gone window, which is the state their headers document. */
    void stop();

    /* Pumps the OS event queue and refreshes input. False once the window is
       gone, which is how a quit request reads. */
    [[nodiscard]] bool pump();

  private:
    bool started_ = false;
};

} // namespace resonate::window

#endif /* RESONATE_WINDOW_SESSION_HPP */
