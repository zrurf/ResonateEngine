#include <resonate/startup/startup.hpp>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef RESONATE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

#include <resonate/application/application.h>
#if defined(RESONATE_WITH_CSHARP)
#    include <resonate/csharp/managed_systems.hpp>
#endif
#include <resonate/pal/io.h>
#include <resonate/application/systems.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/system.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/render/offscreen.hpp>
#include <resonate/renderer/renderer.h>
#include <resonate/window/session.hpp>

namespace resonate::startup
{
namespace
{

/* A value that is not a positive integer leaves the fallback alone, so a bad
   argument falls back to the default rather than to zero or a wrapped negative.
   atoi would be undefined on overflow, which a hand-typed argument reaches. */
std::optional<int> positiveFrom(const std::string& text)
{
    const char* const begin = text.c_str();
    char* end = nullptr;
    const long parsed = std::strtol(begin, &end, 10);
    if (end == begin || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
    {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::string titleFor(const DisplayOptions& display, const char* default_title)
{
    if (!display.title.empty())
    {
        return display.title;
    }
    return default_title != nullptr ? default_title : "Resonate";
}

#if defined(RESONATE_WITH_CSHARP)
/* A relative path is taken beside the executable first, which is where a
   packaged content tree lives; otherwise it is left as given. */
std::string resolveManagedPath(const std::string& path)
{
    const std::filesystem::path candidate(path);
    if (candidate.is_absolute())
    {
        return path;
    }
    const std::string directory = resonate_pal_io_executable_directory();
    const std::filesystem::path beside = std::filesystem::path(directory) / candidate;
    std::error_code error;
    if (std::filesystem::is_regular_file(beside, error))
    {
        return beside.string();
    }
    return path;
}
#endif

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
            options.width = positiveFrom(arguments[++index]);
        }
        else if (argument == "--height" && has_value)
        {
            options.headless = false;
            options.height = positiveFrom(arguments[++index]);
        }
    }

    return options;
}

int run(const std::vector<std::string>& arguments, const char* default_title)
{
    Application::Config config = Application::configFromCommandLine(arguments);
    const DisplayOptions display = displayOptionsFromCommandLine(arguments);

    /* The configuration loads here, before anything consumes it: the window's
       size is a setting too. No file (or an unparsable one) means defaults
       everywhere. */
    if (config.config_file.empty())
    {
        config.config_file = "resonate.toml";
    }
    config.settings.load(config.config_file);

    /* CLI beats configuration beats the default. */
    const uint32_t width = static_cast<uint32_t>(
        display.width.has_value()
            ? *display.width
            : static_cast<int>(config.settings.readUInt("resonate.window", "width", 1600U)));
    const uint32_t height = static_cast<uint32_t>(
        display.height.has_value()
            ? *display.height
            : static_cast<int>(config.settings.readUInt("resonate.window", "height", 900U)));
    config.max_fps = config.settings.readUInt("resonate.loop", "max-fps", 0U);

    /* The session outlives the run, and the capabilities it publishes outlive the
       modules that resolved them. */
    window::Session session;

    /* The engine's own systems, in every composition: the hierarchy's transform
       propagation resolves WorldTransform in LATE, the renderer extracts and
       draws in RENDER. The descriptors and the systems outlive the run, which
       is what the borrowed-context rule needs. */
    hierarchy::PropagationSystem propagation;
    const EngineSystemDesc propagation_desc{"resonate.hierarchy.propagation",
                                            RESONATE_STAGE_LATE_UPDATE,
                                            &hierarchy::PropagationSystem::invoke, &propagation};
    renderer::RenderSystem render_system;

    /* The view's post chain is data: the configured feature names are validated
       at assembly, and a bad stack ends the run with the reason rather than
       falling back to something the project did not ask for. */
    renderer::FeatureStack features = renderer::FeatureStack::defaults();
    {
        const std::string chain =
            config.settings.value("resonate.render", "features").value_or("bloom, tonemap");
        const std::string antialiasing =
            config.settings.value("resonate.render", "antialiasing").value_or("fxaa");
        std::string error;
        if (!renderer::parseFeatureStack(chain, antialiasing, features, error))
        {
            std::fprintf(stderr, "resonate: feature stack: %s\n", error.c_str());
            return EXIT_FAILURE;
        }
        if (const std::optional<std::string> threshold =
                config.settings.value("resonate.render", "bloom-threshold");
            threshold.has_value())
        {
            char* end = nullptr;
            const float parsed = std::strtof(threshold->c_str(), &end);
            if (end != threshold->c_str() && *end == '\0' && parsed > 0.0F)
            {
                features.bloom_threshold = parsed;
            }
        }
    }
    render_system.setFeatures(features);

#if defined(RESONATE_WITH_CSHARP)
    /* An optional managed assembly: its systems register alongside the C++
       ones and run through the dispatch table, so the frame never touches a
       delegate. A configured but broken assembly ends the run with the
       reason. */
    csharp::ManagedSystems managed;
    bool managed_configured = false;
    {
        const std::string configured =
            config.settings.value("resonate.csharp", "assembly").value_or("");
        if (!configured.empty())
        {
            managed_configured = true;
            const std::string assembly = resolveManagedPath(configured);
            std::string runtime_config =
                config.settings.value("resonate.csharp", "runtime-config").value_or("");
            if (runtime_config.empty())
            {
                runtime_config = std::filesystem::path(assembly)
                                     .replace_extension(".runtimeconfig.json")
                                     .string();
            }
            const std::string entry_type =
                config.settings.value("resonate.csharp", "entry-type").value_or("");
            if (entry_type.empty())
            {
                std::fprintf(stderr, "resonate: [resonate.csharp] entry-type is required\n");
                return EXIT_FAILURE;
            }
            std::string error;
            if (!managed.load(resolveManagedPath(runtime_config), assembly, entry_type, error))
            {
                std::fprintf(stderr, "resonate: managed assembly: %s\n", error.c_str());
                return EXIT_FAILURE;
            }
            const std::string component =
                config.settings.value("resonate.csharp", "component").value_or("");
            if (!component.empty())
            {
                managed.set_component(component.c_str());
            }
            const std::string patch =
                config.settings.value("resonate.csharp", "patch").value_or("");
            if (!patch.empty())
            {
                const std::string patch_host =
                    config.settings.value("resonate.csharp", "patch-host").value_or("");
                const std::string patch_host_type =
                    config.settings.value("resonate.csharp", "patch-host-type").value_or("");
                const std::string patch_entry =
                    config.settings.value("resonate.csharp", "patch-entry-type").value_or("");
                if (patch_host.empty() || patch_host_type.empty() || patch_entry.empty())
                {
                    std::fprintf(stderr, "resonate: [resonate.csharp] patch needs patch-host, "
                                         "patch-host-type and patch-entry-type\n");
                    return EXIT_FAILURE;
                }
                std::string patch_error;
                if (!managed.load_patch(resolveManagedPath(patch_host), patch_host_type,
                                        resolveManagedPath(patch), patch_entry, patch_error))
                {
                    std::fprintf(stderr, "resonate: managed patch: %s\n", patch_error.c_str());
                    return EXIT_FAILURE;
                }
            }
        }
    }
#endif

    const EngineSystemDesc render_desc{"renderer.frame", RESONATE_STAGE_RENDER,
                                       &renderer::RenderSystem::invoke, &render_system};
#if defined(RESONATE_WITH_CSHARP)
    /* Every stage the managed assembly may declare. The schedule reads these
       every frame and copies the names, so they live as long as the run; only
       the stages the assembly filled are registered. */
    std::array<EngineSystemDesc, csharp::ManagedSystems::kStageEntryCount> managed_descs{};
    {
        const csharp::ManagedSystems::StageEntry* entries =
            csharp::ManagedSystems::stageEntries();
        for (std::size_t index = 0; index < managed_descs.size(); ++index)
        {
            managed_descs[index] = EngineSystemDesc{entries[index].name, entries[index].stage,
                                                    entries[index].run, &managed};
        }
    }
#endif
    config.systems = [&](ModuleHost& host, ecs::World& world)
    {
        if (!hierarchy::registerComponents(world) ||
            !renderer::RenderSystem::registerComponents(world))
        {
            return RESONATE_E_INVALID;
        }
        /* The level slices live on this system's stack. The parallel path has
           been seen running a slice after its wait returned on Android, so the
           levels are computed inline until the job system's parallel-context
           lifetime is settled. */
        propagation.jobs = nullptr;
        render_system.setRegistry(host.capabilities());
        if (addEngineSystem(host, propagation_desc) != RESONATE_OK ||
            addEngineSystem(host, render_desc) != RESONATE_OK)
        {
            return RESONATE_E_INVALID;
        }
#if defined(RESONATE_WITH_CSHARP)
        if (managed_configured)
        {
            const csharp::ManagedSystems::StageEntry* entries =
                csharp::ManagedSystems::stageEntries();
            for (std::size_t index = 0; index < managed_descs.size(); ++index)
            {
                if (managed.usesStage(entries[index].stage) &&
                    addEngineSystem(host, managed_descs[index]) != RESONATE_OK)
                {
                    return RESONATE_E_INVALID;
                }
            }
        }
#endif
        /* The demo scene is what the window shows until scenes load from disk;
           it is plain ECS entities, created before the frame loop starts. */
        renderer::composeDemoScene(world);
        return RESONATE_OK;
    };

    /* The renderer's device, graph and pipelines come up before the frame
       loop, so the first displayed frame draws instead of compiling. */
    config.startup = [&render_system] { render_system.warmup(); };

    /* The renderer's device resources go while the device module is alive;
       after the run the host detaches and the capability dies with it. The
       managed context closes after the frame loop has stopped. */
    config.shutdown = [&]
    {
        render_system.release();
#if defined(RESONATE_WITH_CSHARP)
        managed.unload();
#endif
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
        window_config.width = static_cast<int>(width);
        window_config.height = static_cast<int>(height);

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
