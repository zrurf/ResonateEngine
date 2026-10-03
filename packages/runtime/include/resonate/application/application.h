#ifndef RESONATE_APPLICATION_APPLICATION_H
#define RESONATE_APPLICATION_APPLICATION_H

#include <functional>
#include <string>
#include <vector>

#include <resonate/module/capability.h>

namespace resonate
{

class ModuleHost;

namespace ecs
{
class World;
}

/* Startup sequence shared by the launcher and the editor.
 *
 * The runtime knows nothing about displays. It owns the module host, the world
 * the run's systems record into, and the frame loop; whatever the host itself
 * provides and whatever it does per frame arrive as hooks the caller supplies,
 * so a windowed run, an offscreen run and a server are the same loop with
 * different wiring. */
class Application
{
  public:
    struct Config
    {
        /* Directory scanned for module libraries. Defaults to "plugins" beside
           the executable; RESONATE_PLUGIN_PATH overrides it. */
        std::string plugin_directory;

        /* Path exposed to modules through the host API's read_config. */
        std::string config_file;

        /* Called after the host exists and before modules resolve, so a module
           that requires one of the host's capabilities finds it in the registry.
           Empty means the host provides nothing of its own. */
        std::function<ResonateStatus(ResonateCapabilityRegistry*)> publish;

        /* Called once, after the host exists and the run's world is set, before
           the modules attach: the run's own C++ systems register here, through
           addEngineSystem. Empty means the schedule carries plugin systems
           only. */
        std::function<ResonateStatus(ModuleHost&, ecs::World&)> systems;

        /* Called at the top of every frame, before the stages run. Returning
           false ends the run, which is how a window reports a quit request.
           Empty means the host has no per-frame work. */
        std::function<bool()> tick;
    };

    /* Discovers, resolves and attaches modules, runs the frame loop, then
       detaches in reverse order. Returns a process exit code.

       Runs until requestStop() is called — the SIGINT handler installed for the
       run's duration does that, so Ctrl+C stops the process in any mode — or until
       the tick above returns false. */
    static int run(const Config& config);

    /* Ends the run in progress: the frame finishes, then the loop stops. */
    static void requestStop();

    /* Recognised: --plugin-dir, --config. Display options belong to whatever
       supplies the display; resonate/startup/startup.hpp parses those and wires
       this up. */
    static Config configFromCommandLine(const std::vector<std::string>& arguments);
    static Config configFromCommandLine(int argc, char** argv);
};

} // namespace resonate

#endif /* RESONATE_APPLICATION_APPLICATION_H */
