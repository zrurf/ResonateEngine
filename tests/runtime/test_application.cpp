#include <catch2/catch_all.hpp>

#include <csignal>
#include <string>

#include <SDL3/SDL.h>

#include <resonate/application/application.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/window/session.hpp>

#include "paths.h"

namespace
{

/* SDL's dummy video driver: a window that exists with no display behind it, which
   is not the same thing as a headless run. */
struct DummyVideo
{
    DummyVideo()
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1);
    }
};

/* A driver name no platform has. A run that opened video at all would fail here,
   so this is what makes "headless" a checked property rather than an absent
   window. The windowed cases put the driver back themselves. */
void useAnImpossibleVideoDriver()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "resonate-must-not-open-video");
    SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "resonate-must-not-open-video", 1);
}

/* Runs the loop for `limit` frames and then asks it to stop.
 *
 * The engine has no frame bound of its own — a game runs until it is told to stop
 * — so how long a run lasts is the caller's business, and this is the hook it
 * expresses that in. */
struct StopAfter
{
    resonate::window::Session* session = nullptr;
    int frames = 0;
    int limit = 1;

    bool tick()
    {
        if (session != nullptr && !session->pump())
        {
            return false;
        }
        if (frames >= limit)
        {
            return false;
        }
        ++frames;
        return true;
    }
};

resonate::Application::Config configFor(const std::string& plugin_directory)
{
    resonate::Application::Config config;
    config.plugin_directory = plugin_directory;
    return config;
}

} // namespace

TEST_CASE("the application runs frames and shuts down", "[runtime]")
{
    /* This target depends on the plugins being built, so a directory that is not
       there is a build problem rather than a reason to pass without checking. */
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    const DummyVideo dummy;

    constexpr int FRAMES = 3;
    resonate::window::Session session;
    StopAfter stop{&session, 0, FRAMES};

    resonate::window::Session::Config window_config;
    window_config.title = "Resonate test";
    window_config.width = 320;
    window_config.height = 200;

    resonate::Application::Config config = configFor(plugin_directory);
    config.publish = [&session, window_config](ResonateCapabilityRegistry* registry)
    { return session.start(registry, window_config); };
    config.tick = [&stop] { return stop.tick(); };

    /* Host, display capabilities, discovery, resolve, attach, the pumped frame
       loop and its stages, then detach and teardown. */
    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);
    REQUIRE(stop.frames == FRAMES);
}

TEST_CASE("SIGINT ends a run", "[runtime]")
{
    const DummyVideo dummy;

    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    resonate::window::Session session;
    resonate::window::Session::Config window_config;
    window_config.title = "Resonate test";
    window_config.width = 320;
    window_config.height = 200;

    /* The tick never asks for a stop; it raises the interrupt a console would and
       then keeps returning true. The bound is a safety net that also makes the
       test discriminating: if the handler were not installed or the flag not
       honoured, the run would take all of them. */
    constexpr int SAFETY_LIMIT = 1000;
    int frames = 0;
    bool raised = false;

    resonate::Application::Config config = configFor(plugin_directory);
    config.publish = [&session, window_config](ResonateCapabilityRegistry* registry)
    { return session.start(registry, window_config); };
    config.tick = [&session, &frames, &raised]
    {
        if (!session.pump())
        {
            return false;
        }
        if (!raised)
        {
            raised = true;
            std::raise(SIGINT);
        }
        ++frames;
        return frames < SAFETY_LIMIT;
    };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);
    REQUIRE(raised);
    REQUIRE(frames < 4);
}

TEST_CASE("a headless run never opens SDL video", "[runtime]")
{
    useAnImpossibleVideoDriver();

    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    constexpr int FRAMES = 2;
    StopAfter stop{nullptr, 0, FRAMES};

    resonate::Application::Config config = configFor(plugin_directory);

    /* The offscreen canvas is what the modules that draw require, so they attach
       here without a window existing — which is the point of the abstraction. */
    config.publish = [](ResonateCapabilityRegistry* registry)
    { return resonate::render::publishOffscreen(registry, 320U, 200U); };
    config.tick = [&stop] { return stop.tick(); };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);
    REQUIRE(stop.frames == FRAMES);
}

TEST_CASE("the application reports a plugin directory it cannot read", "[runtime]")
{
    const DummyVideo dummy;

    REQUIRE(resonate::Application::run(configFor("no_such_plugin_directory")) == EXIT_FAILURE);
}
