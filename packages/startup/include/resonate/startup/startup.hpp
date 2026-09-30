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

#include <string>
#include <vector>

namespace resonate::startup
{

/* The display options a command line asks for. */
struct DisplayOptions
{
    /* --headless: no window, no input, and an offscreen canvas instead. */
    bool headless = false;

    /* Window title, or empty when the command line named none and the binary's
       own name for it applies. */
    std::string title;

    /* Frame size in pixels: the window's back buffer, or the offscreen target
       when there is no window. */
    int width = 1600;
    int height = 900;
};

/* Recognised: --headless, --title, --width, --height. Anything else is left for
   whoever else parses this command line. */
DisplayOptions displayOptionsFromCommandLine(const std::vector<std::string>& arguments);

/* Parses the command line, composes a windowed or an offscreen run, and returns
   the process exit code. `default_title` names the window when the command line
   did not. */
int run(const std::vector<std::string>& arguments, const char* default_title);

/* From the platform's own main(). */
std::vector<std::string> commandLine(int argc, char** argv);

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
