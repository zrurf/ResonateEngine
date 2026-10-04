#include <resonate/application/application.h>

#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/world.h>
#include <resonate/module/host.hpp>
#include <resonate/pal/chrono.h>
#include <resonate/pal/io.h>

namespace resonate
{
namespace
{

/*
 * Set by the SIGINT handler and by requestStop().
 *
 * A plain flag rather than an atomic, because sig_atomic_t is what a handler may
 * write and the shared state is one one-way bit: the worst a race between the two
 * writers can do is have the loop notice a frame later.
 */
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void onInterrupt(int)
{
    g_stop_requested = 1;
}

/* A shipped game keeps its plugins beside the executable. A build tree does not:
   the binaries land under build/<platform>/<arch>/<mode> while the plugin
   directory is build/plugins, so the ancestors are searched as well. */
std::string defaultPluginDirectory()
{
    const std::string executable_directory = resonate_pal_io_executable_directory();
    std::string directory = executable_directory;

    for (int level = 0; level < 4; ++level)
    {
        const std::string candidate = directory + "/plugins";
        if (resonate_pal_io_is_directory(candidate.c_str()) != 0U)
        {
            return candidate;
        }

        const std::size_t separator = directory.find_last_of("/\\");
        if (separator == std::string::npos)
        {
            break;
        }
        directory.erase(separator);
    }

    /* Nothing found, so name the documented location: the host log then points
       somewhere a plugin could be put. */
    return executable_directory + "/plugins";
}

} // namespace

void Application::requestStop()
{
    g_stop_requested = 1;
}

Application::Config Application::configFromCommandLine(int argc, char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0U);
    for (int index = 0; index < argc; ++index)
    {
        arguments.emplace_back(argv[index] != nullptr ? argv[index] : "");
    }
    return configFromCommandLine(arguments);
}

Application::Config Application::configFromCommandLine(const std::vector<std::string>& arguments)
{
    Config config;

    /* The environment is the fallback, not the override, so a launcher can point
       a build at its own plugin directory without the variable winning over an
       explicit flag. */
    const char* const from_environment = std::getenv("RESONATE_PLUGIN_PATH");
    config.plugin_directory = from_environment != nullptr && *from_environment != '\0'
                                  ? from_environment
                                  : defaultPluginDirectory();

    for (std::size_t index = 1; index + 1 < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        if (argument == "--plugin-dir")
        {
            config.plugin_directory = arguments[++index];
        }
        else if (argument == "--config")
        {
            config.config_file = arguments[++index];
        }
    }

    return config;
}

int Application::run(const Config& config)
{
    /* The world is the run's state, declared before the host because the host
       borrows it: its systems' command buffers release reservations through it,
       so the world has to be the one that outlives them. */
    ecs::World world(systemAllocator());

    std::unique_ptr<ModuleHost> host = ModuleHost::create(systemAllocator());
    if (host == nullptr)
    {
        std::fprintf(stderr, "resonate: the module host could not be created\n");
        return EXIT_FAILURE;
    }
    host->setWorld(&world);

    if (config.publish)
    {
        const ResonateStatus published = config.publish(host->capabilities());
        if (published != RESONATE_OK)
        {
            std::fprintf(stderr, "resonate: the host's own capabilities were not published\n");
            return EXIT_FAILURE;
        }
    }

    /* The run's own C++ systems, registered before the modules attach: they are
       known first, so a module that collides with one gets the report. */
    if (config.systems)
    {
        const ResonateStatus registered = config.systems(*host, world);
        if (registered != RESONATE_OK)
        {
            std::fprintf(stderr, "resonate: the run's systems were not registered\n");
            return EXIT_FAILURE;
        }
    }

    if (host->discover(config.plugin_directory) != RESONATE_OK)
    {
        return EXIT_FAILURE;
    }

    /* What the host provides decides what resolves: a module requiring something
       it does not publish is refused here rather than attached and handed
       something that is not there. */
    if (host->resolve() != RESONATE_OK || host->attachAll() != RESONATE_OK)
    {
        return EXIT_FAILURE;
    }

    /* Cleared so two runs in one process are independent, and restored afterwards
       so a caller that had its own handler gets it back. */
    g_stop_requested = 0;
    void (*const previous)(int) = std::signal(SIGINT, &onInterrupt);

    if (JobSystem* const jobs = host->jobs(); jobs != nullptr && config.worker_count != 0U)
    {
        jobs->requestWorkerCount(config.worker_count);
    }

    FrameClock clock(config.frame_clock);

    uint64_t previous_ticks = resonate_pal_chrono_ticks();
    while (g_stop_requested == 0)
    {
        /* Before the stages, which is what a window capability asks for: input is
           refreshed for the frame, and a quit request ends the run here rather
           than partway through the stages. */
        if (config.tick && !config.tick())
        {
            break;
        }

        float elapsed_seconds = 0.0F;
        if (config.frame_elapsed)
        {
            elapsed_seconds = config.frame_elapsed();
        }
        else
        {
            const uint64_t now_ticks = resonate_pal_chrono_ticks();
            const uint64_t elapsed_ns =
                resonate_pal_chrono_ticks_to_nanoseconds(now_ticks - previous_ticks);
            previous_ticks = now_ticks;
            elapsed_seconds = static_cast<float>(elapsed_ns) * 1e-9F;
        }

        /* One displayed frame: the simulation steps its wall time covers — the
           clock's budget bounds how many — then one render frame on the wall
           time itself. */
        const std::uint32_t steps = clock.advance(elapsed_seconds);
        if (clock.droppedSteps() != 0U)
        {
            std::fprintf(stderr,
                         "resonate: frame time over the step budget; %llu step(s) dropped\n",
                         static_cast<unsigned long long>(clock.droppedSteps()));
        }
        for (std::uint32_t step = 0; step < steps; ++step)
        {
            host->runSimulationStep(clock.stepSeconds());
        }
        host->runRenderFrame(elapsed_seconds);
    }

    if (previous != SIG_ERR)
    {
        std::signal(SIGINT, previous);
    }

    host->detachAll();
    return EXIT_SUCCESS;
}

} // namespace resonate
