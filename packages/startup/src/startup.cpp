#include <resonate/startup/startup.hpp>

#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef RESONATE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

#include <resonate/application/application.h>
#include <resonate/application/systems.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/system.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/render/offscreen.hpp>
#include <resonate/window/session.hpp>

namespace resonate::startup
{
namespace
{

/* A value that is not a positive integer leaves the target alone, so a bad
   argument falls back to the default rather than to zero or a wrapped negative.
   atoi would be undefined on overflow, which a hand-typed argument reaches. */
void readPositive(const std::string& text, int& out_value)
{
    const char* const begin = text.c_str();
    char* end = nullptr;
    const long parsed = std::strtol(begin, &end, 10);
    if (end == begin || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
    {
        return;
    }
    out_value = static_cast<int>(parsed);
}

std::string titleFor(const DisplayOptions& display, const char* default_title)
{
    if (!display.title.empty())
    {
        return display.title;
    }
    return default_title != nullptr ? default_title : "Resonate";
}

} // namespace

DisplayOptions displayOptionsFromCommandLine(const std::vector<std::string>& arguments)
{
    DisplayOptions options;

    /* Every option takes a value except --headless, so the loop walks one
       argument at a time and each of the others checks for its own. */
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        const bool has_value = index + 1 < arguments.size();

        if (argument == "--headless")
        {
            options.headless = true;
        }
        /* An option that only means something for a window brings one back, so a
           run configured `--headless --title X` is a titled window rather than a
           silent no-op: whichever flag comes last decides. */
        else if (argument == "--title" && has_value)
        {
            options.headless = false;
            options.title = arguments[++index];
        }
        else if (argument == "--width" && has_value)
        {
            options.headless = false;
            readPositive(arguments[++index], options.width);
        }
        else if (argument == "--height" && has_value)
        {
            options.headless = false;
            readPositive(arguments[++index], options.height);
        }
    }

    return options;
}

int run(const std::vector<std::string>& arguments, const char* default_title)
{
    Application::Config config = Application::configFromCommandLine(arguments);

    const DisplayOptions display = displayOptionsFromCommandLine(arguments);
    const auto width = static_cast<uint32_t>(display.width);
    const auto height = static_cast<uint32_t>(display.height);

    /* The session outlives the run, and the capabilities it publishes outlive the
       modules that resolved them. */
    window::Session session;

    /* The engine's own systems, in every composition: the hierarchy's transform
       propagation resolves WorldTransform in LATE. The descriptor and the system
       outlive the run, which is what the borrowed-context rule needs. */
    hierarchy::PropagationSystem propagation;
    const EngineSystemDesc propagation_desc{"resonate.hierarchy.propagation",
                                            RESONATE_STAGE_LATE_UPDATE,
                                            &hierarchy::PropagationSystem::invoke, &propagation};
    config.systems = [&propagation, &propagation_desc](ModuleHost& host, ecs::World& world)
    {
        if (!hierarchy::registerComponents(world))
        {
            return RESONATE_E_INVALID;
        }
        propagation.jobs = host.jobs();
        return addEngineSystem(host, propagation_desc);
    };

    if (display.headless)
    {
        config.publish = [width, height](ResonateCapabilityRegistry* registry)
        { return render::publishOffscreen(registry, width, height); };

        /* Nothing to pump, so the run ends when it is asked to. */
        config.tick = [] { return true; };
    }
    else
    {
        window::Session::Config window_config;
        window_config.title = titleFor(display, default_title);
        window_config.width = display.width;
        window_config.height = display.height;

        /* The two config structs are deliberately separate types, converted here
           at the one place that knows about both. */
        config.publish = [&session, window_config](ResonateCapabilityRegistry* registry)
        { return session.start(registry, window_config); };

        /* False once the window is gone, which is how a quit request reads. */
        config.tick = [&session] { return session.pump(); };
    }

    return Application::run(config);
}

std::vector<std::string> commandLine(int argc, char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0U);
    for (int index = 0; index < argc; ++index)
    {
        arguments.emplace_back(argv[index] != nullptr ? argv[index] : "");
    }
    return arguments;
}

} // namespace resonate::startup

#ifdef RESONATE_PLATFORM_WINDOWS

namespace
{

/* -1 includes the terminator, which is then left out of the result. */
std::string toUtf8(const wchar_t* text)
{
    if (text == nullptr)
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }

    std::string converted(static_cast<std::size_t>(size) - 1U, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, converted.data(), size, nullptr, nullptr);
    return converted;
}

} // namespace

std::vector<std::string> resonate::startup::commandLine()
{
    std::vector<std::string> arguments;
    if (__wargv == nullptr || __argc <= 0)
    {
        return arguments;
    }

    arguments.reserve(static_cast<std::size_t>(__argc));
    for (int index = 0; index < __argc; ++index)
    {
        arguments.push_back(toUtf8(__wargv[index]));
    }
    return arguments;
}

#endif /* RESONATE_PLATFORM_WINDOWS */
