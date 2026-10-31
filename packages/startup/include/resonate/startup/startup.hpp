#ifndef RESONATE_STARTUP_STARTUP_H
#define RESONATE_STARTUP_STARTUP_H

/*
 * The startup the engine's own binaries share: one command line, a display if one
 * was asked for, and the runtime's frame loop around it.
 *
 * It lives above both the runtime and the window package because it is the only
 * place that knows about both — which is what keeps the runtime free of any
 * knowledge of displays, and the window package free of any knowledge of the
 * frame loop.
 */

#include <optional>
#include <string>
#include <vector>

#include <resonate/application/systems.h>
#include <resonate/dom/schema.h>
#include <resonate/window/input.h>

namespace resonate::ui
{
class StringTable;
}

namespace resonate::startup
{

class CVarHost;

/* The display options a command line asks for. */
struct DisplayOptions
{
    /* --headless: no window, no input, and an offscreen canvas instead. */
    bool headless = false;

    /* Window title, or empty when the command line named none and the binary's
       own name for it applies. */
    std::string title;

    /* Frame size in pixels when the command line named one, or nullopt — the
       run's configuration then decides (a window's back buffer, or the
       offscreen target), falling back to 1600x900. */
    std::optional<int> width;
    std::optional<int> height;
};

/* Recognised: --headless, --title, --width, --height. Anything else is left for
   whoever else parses this command line. */
DisplayOptions displayOptionsFromCommandLine(const std::vector<std::string>& arguments);

/* What a host adds to the run beyond the engine's own systems: extra schedule
   entries, and a hook that runs once the UI document is instantiated, so the
   host can address widgets by key and wire its own behaviour. */
struct HostExtension
{
    std::vector<EngineSystemDesc> systems;
    void (*ready)(void* context, ecs::World& world, ecs::Entity canvas) = nullptr;
    void* ready_context = nullptr;
    /* When set, the run copies its CVar host into it after binding the
       engine's own keys: the application reads and applies them and binds
       more. */
    CVarHost* cvars = nullptr;
    /* The host's shortcut pass: called once per frame after the UI has taken
       the keys it owns. The hook polls the keys it handles from `input`, so a
       chord a focused text field consumed is not replayed here. */
    bool (*keys)(void* context, ecs::World& world, const ResonateInput* input) = nullptr;
    void* keys_context = nullptr;
    /* The scene roots the run's document produced, handed over once the scene
       is composed: a host that tears the scene down or writes it back needs
       every one of them. Empty when the run loaded no scene document. */
    void (*scene)(void* context, ecs::World& world,
                  const std::vector<ecs::Entity>& roots) = nullptr;
    void* scene_context = nullptr;
};

/* The content vocabulary a run composes: every module's generated schema and
   component tables behind one registry each. A host that reads a document at
   runtime (an editor restoring a captured object) parses with the same tables
   the demo was loaded with. */
[[nodiscard]] const dom::SchemaRegistry& contentSchema();
[[nodiscard]] const dom::ComponentRegistry& contentComponents();

/* The run's UI string table (content/ui/strings.rstrings), or null when the
   run loaded none. Markup resolves "#key" against it while the document is
   instantiated; a host looks up the keys it composes at runtime. Valid from
   composition until run() returns. */
[[nodiscard]] const ui::StringTable* stringTable();

/* Parses the command line, composes a windowed or an offscreen run, and returns
   the process exit code. `default_title` names the window when the command line
   did not. */
int run(const std::vector<std::string>& arguments, const char* default_title,
        const HostExtension& extension = {});

/* From the platform's own main(). */
std::vector<std::string> commandLine(int argc, char** argv);

#if defined(RESONATE_PLATFORM_WINDOWS)
/* Installs the unhandled-exception filter: a fault writes a minidump beside
   the executable (resonate-crash-<timestamp>.dmp) and the process ends with
   it. RESONATE_CRASH_TEST=1 raises right after the install, which is how the
   dump path is verified without a real fault. */
void installCrashHandler();
#endif

#ifdef RESONATE_PLATFORM_WINDOWS
/* The process's arguments as UTF-8, program name first, read from the OS.
 *
 * A Windows windowed binary has no main(argc, argv), and the CRT hands it the
 * command line as UTF-16 while leaving the narrow array null, so the OS is the
 * only dependable source. */
std::vector<std::string> commandLine();
#endif

} // namespace resonate::startup

#endif /* RESONATE_STARTUP_STARTUP_H */
