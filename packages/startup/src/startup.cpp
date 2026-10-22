#include <resonate/startup/startup.hpp>

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#ifdef RESONATE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

#include <resonate/application/application.h>
#include <resonate/application/settings.h>
#include <resonate/gameplay/action_map.h>
#if defined(RESONATE_WITH_CSHARP)
#    include <resonate/csharp/managed_systems.hpp>
#endif
#include <resonate/pal/io.h>
#include <resonate/application/systems.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/system.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/dom/document.h>
#include <resonate/render/canvas.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/renderer/renderer.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/layout.h>
#include <resonate/window/input.h>
#include <resonate/window/session.hpp>

#include "resonate.ui/components.gen.h"

namespace resonate::startup
{
namespace
{

/* The input system: one sample per frame, before anything gameplay reads the
   actions. A run without the input capability (headless) releases everything. */
struct ActionTick
{
    gameplay::ActionMap* map = nullptr;
    const ResonateInput* input = nullptr;

    static void invoke(void* context, ecs::World&, ecs::CommandBuffer&, float)
    {
        auto* tick = static_cast<ActionTick*>(context);
        tick->map->update(tick->input);
    }
};

/* The layout system: the retained tree is resolved against the target the
   renderer will draw into, once per frame before RENDER extracts it. */
struct UiFrame
{
    ecs::Entity canvas{};
    const ResonateRenderCanvas* target = nullptr;

    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer&, float)
    {
        auto* frame = static_cast<UiFrame*>(context);
        if (frame == nullptr || frame->target == nullptr || !frame->canvas.valid())
        {
            return;
        }
        const std::uint32_t width = frame->target->width(frame->target->self);
        const std::uint32_t height = frame->target->height(frame->target->self);
        if (width == 0U || height == 0U)
        {
            return;
        }
        ui::layout(world, frame->canvas, static_cast<float>(width), static_cast<float>(height));
    }
};

ecs::Entity composeBuiltinHud(ecs::World& world);

/* Content lives beside the executable in a shipped layout and in the project
   tree during development; the walk covers both. */
std::string findContentFile(const std::string& relative)
{
    std::filesystem::path directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 6; ++level)
    {
        const std::filesystem::path candidate = directory / relative;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate.string();
        }
        if (!directory.has_parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    return {};
}

/* The configured UI document, instantiated into the world. A missing default
   document falls back to the built-in HUD; a configured one or a broken one
   ends the run with the reason. */
ecs::Entity loadDemoHud(ecs::World& world, const std::string& configured, bool required)
{
    /* A host that stages its assets elsewhere (Android) names the document
       directly; the setting and the content walk cover desktop runs. */
    const char* const from_environment = std::getenv("RESONATE_UI_DOCUMENT");
    const bool overridden = from_environment != nullptr && *from_environment != '\0';
    if (configured.empty() && !overridden)
    {
        return composeBuiltinHud(world);
    }
    const std::string path =
        overridden ? std::string(from_environment) : findContentFile(configured);
    if (path.empty())
    {
        if (required || overridden)
        {
            std::fprintf(stderr, "resonate: ui document: cannot find %s\n", configured.c_str());
            return ecs::Entity{};
        }
        std::fprintf(stderr, "resonate: ui document: %s is missing; using the built-in HUD\n",
                     configured.c_str());
        return composeBuiltinHud(world);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        std::fprintf(stderr, "resonate: ui document: cannot read %s\n", path.c_str());
        return ecs::Entity{};
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::vector<dom::Diagnostic> diagnostics;
    auto document = dom::Document::parse(text, &ui::documentSchema(), diagnostics);
    if (document == nullptr)
    {
        for (const dom::Diagnostic& diagnostic : diagnostics)
        {
            std::fprintf(stderr, "resonate: ui document: %s:%u:%u: %s\n", path.c_str(),
                         diagnostic.line, diagnostic.column, diagnostic.message.c_str());
        }
        return ecs::Entity{};
    }
    const ecs::Entity canvas = ui::instantiate(world, *document);
    if (!canvas.valid())
    {
        return ecs::Entity{};
    }
    std::fprintf(stderr, "resonate: ui: %s\n", path.c_str());
    return canvas;
}

/* The start-up's demo HUD: a panel with three bars, laid out and painted every
   frame. It is sample content, not a library default. */
ecs::Entity composeBuiltinHud(ecs::World& world)
{
    const ecs::Entity canvas = world.create();
    world.add(canvas, ui::Canvas{});
    world.add(canvas, ui::ComputedRect{});

    ecs::CommandBuffer commands(world);

    ui::LayoutGroup column;
    column.direction = ui::FlexDirection::Column;
    column.align = ui::Align::Stretch;
    column.gap = 8.0F;
    column.padding_left = 12.0F;
    column.padding_top = 12.0F;
    column.padding_right = 12.0F;
    column.padding_bottom = 12.0F;

    ui::RectTransform panel_rect;
    panel_rect.x = 40.0F;
    panel_rect.y = 40.0F;
    panel_rect.width = 360.0F;
    panel_rect.height = 180.0F;

    const ecs::Entity panel = world.create();
    world.add(panel, panel_rect);
    world.add(panel, ui::ComputedRect{});
    world.add(panel, ui::Background{Vec4{0.06F, 0.07F, 0.10F, 0.92F}});
    world.add(panel, column);
    hierarchy::setParent(commands, panel, canvas);

    const auto addBar = [&](float height, Vec4 color)
    {
        ui::RectTransform rect;
        rect.height = height;
        const ecs::Entity bar = world.create();
        world.add(bar, rect);
        world.add(bar, ui::ComputedRect{});
        world.add(bar, ui::Background{color});
        hierarchy::setParent(commands, bar, panel);
    };
    addBar(28.0F, Vec4{0.20F, 0.42F, 0.90F, 1.0F});
    addBar(16.0F, Vec4{0.35F, 0.84F, 0.45F, 1.0F});
    addBar(16.0F, Vec4{0.92F, 0.52F, 0.30F, 1.0F});

    world.play(commands);
    return canvas;
}

/* A value that is not a positive integer leaves the fallback alone, so a bad
   argument falls back to the default rather than to zero or a wrapped negative.
   atoi would be undefined on overflow, which a hand-typed argument reaches. */
std::optional<int> positiveFrom(const std::string& text)
{
    const char* const begin = text.c_str();
    char* end = nullptr;
    /* 64-bit, so a hand-typed value beyond int does not saturate at LONG_MAX
       on Windows and slip through as valid. */
    const long long parsed = std::strtoll(begin, &end, 10);
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

    /* The project's settings contract sits beside the run's config. When it is
       present, every key is checked before anything consumes it: an unknown key
       is a warning, a wrong type or an out-of-range value ends the run. A
       missing contract leaves the keys unchecked, so a game without one still
       runs. */
    SettingsSchema schema;
    {
        std::filesystem::path schema_path =
            std::filesystem::path(config.config_file).parent_path() / "resonate.settings.toml";
        if (!std::filesystem::is_regular_file(schema_path))
        {
            schema_path = "resonate.settings.toml";
        }
        if (std::filesystem::is_regular_file(schema_path))
        {
            std::string error;
            if (!schema.load(schema_path.string(), error))
            {
                std::fprintf(stderr, "resonate: settings schema: %s\n", error.c_str());
                return EXIT_FAILURE;
            }
            std::vector<std::string> warnings;
            if (!schema.validate(config.settings, warnings, error))
            {
                std::fprintf(stderr, "resonate: config: %s\n", error.c_str());
                return EXIT_FAILURE;
            }
            for (const std::string& warning : warnings)
            {
                std::fprintf(stderr, "resonate: config: %s\n", warning.c_str());
            }
        }
    }

    /* Input actions are optional: a run without a map still has raw input.
       The map loads before anything consumes it, and a broken asset ends the
       run with the reason. */
    gameplay::ActionMap action_map;
    const std::string action_map_path =
        config.settings.value("resonate.input", "action-map").value_or("");
    const std::string bindings_path =
        config.settings.value("resonate.input", "bindings").value_or("");
    if (!action_map_path.empty())
    {
        std::string error;
        if (!action_map.load(action_map_path, error))
        {
            std::fprintf(stderr, "resonate: action map: %s\n", error.c_str());
            return EXIT_FAILURE;
        }
        if (!bindings_path.empty() && !action_map.loadOverrides(bindings_path, error))
        {
            std::fprintf(stderr, "resonate: input bindings: %s\n", error.c_str());
            return EXIT_FAILURE;
        }
        std::fprintf(stderr, "resonate: input: %zu action(s)\n", action_map.actionCount());
    }
    else if (!bindings_path.empty())
    {
        std::fprintf(stderr, "resonate: [resonate.input] bindings needs an action-map\n");
        return EXIT_FAILURE;
    }

    /* CLI beats configuration beats the schema default. */
    const uint32_t width = display.width.has_value()
                               ? *display.width
                               : schema.readUInt(config.settings, "resonate.window.width", 1600U);
    const uint32_t height =
        display.height.has_value()
            ? *display.height
            : schema.readUInt(config.settings, "resonate.window.height", 900U);
    config.max_fps = schema.readUInt(config.settings, "resonate.loop.max-fps", 0U);
    const std::optional<std::string> ui_document_setting =
        config.settings.value("resonate.ui", "document");
    const std::string ui_document =
        ui_document_setting.value_or("content/ui/demo.rui");
    const bool ui_document_required =
        ui_document_setting.has_value() && !ui_document_setting->empty();

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
            schema.readString(config.settings, "resonate.render.features", "bloom, tonemap");
        const std::string antialiasing =
            schema.readString(config.settings, "resonate.render.antialiasing", "fxaa");
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

    /* The map outlives the run and the input system samples it before gameplay;
       managed reads resolve against that same instance. */
    managed.set_actions(&action_map);
#endif

    const EngineSystemDesc render_desc{"renderer.frame", RESONATE_STAGE_RENDER,
                                       &renderer::RenderSystem::invoke, &render_system};
    /* The retained tree is laid out after the scene's transforms and before the
       renderer extracts its paint list. */
    UiFrame ui_frame;
    const EngineSystemDesc ui_desc{"ui.layout", RESONATE_STAGE_LATE_UPDATE, &UiFrame::invoke,
                                   &ui_frame};
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
    /* The input system samples actions once per frame, before gameplay reads
       them. A run without a window still has the system and releases all. */
    ActionTick action_tick{&action_map, nullptr};
    const EngineSystemDesc actions_desc{"input.actions", RESONATE_STAGE_EARLY_UPDATE,
                                        &ActionTick::invoke, &action_tick};
    const bool actions_configured = !action_map_path.empty();
    config.systems = [&](ModuleHost& host, ecs::World& world)
    {
        if (!hierarchy::registerComponents(world) ||
            !renderer::RenderSystem::registerComponents(world) || !ui::registerComponents(world))
        {
            return RESONATE_E_INVALID;
        }
        /* The layout pass needs the target the renderer will draw into; the
           capability is host-owned and outlives the run. */
        {
            std::uint32_t version = 0;
            void* target = resonate_capability_find(
                host.capabilities(), detail::CapabilityTraits<ResonateRenderCanvas>::id.value(),
                1U, &version);
            ui_frame.target = static_cast<const ResonateRenderCanvas*>(target);
            ui_frame.canvas = loadDemoHud(world, ui_document, ui_document_required);
            if (!ui_frame.canvas.valid())
            {
                return RESONATE_E_INVALID;
            }
        }
        /* The level slices live on this system's stack. The parallel path has
           been seen running a slice after its wait returned on Android, so the
           levels are computed inline until the job system's parallel-context
           lifetime is settled. */
        propagation.jobs = nullptr;
        render_system.setRegistry(host.capabilities());
        if (addEngineSystem(host, propagation_desc) != RESONATE_OK ||
            addEngineSystem(host, ui_desc) != RESONATE_OK ||
            addEngineSystem(host, render_desc) != RESONATE_OK)
        {
            return RESONATE_E_INVALID;
        }
        if (actions_configured)
        {
            std::uint32_t version = 0;
            void* input = resonate_capability_find(
                host.capabilities(), detail::CapabilityTraits<ResonateInput>::id.value(),
                detail::CapabilityTraits<ResonateInput>::version, &version);
            action_tick.input = static_cast<const ResonateInput*>(input);
            if (addEngineSystem(host, actions_desc) != RESONATE_OK)
            {
                return RESONATE_E_INVALID;
            }
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
