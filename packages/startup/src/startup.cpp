#include <resonate/startup/startup.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#include <resonate/pal/platform.h>
#include <resonate/application/systems.h>
#include <resonate/ecs/world.h>
#include <resonate/animation/player.h>
#include <resonate/asset/baked_manifest.h>
#include <resonate/asset/vfs.h>
#include <resonate/hierarchy/system.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/startup/cvar.h>
#include <resonate/dom/document.h>
#include <resonate/render/canvas.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/renderer/renderer.h>
#include <resonate/text/font.h>
#include <resonate/scene/document.h>
#include <resonate/ui/document.h>
#include <resonate/ui/binding.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/event.h>
#include <resonate/ui/icon_registry.h>
#include <resonate/ui/image_registry.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/string_table.h>
#include <resonate/ui/style.h>
#include <resonate/ui/text_runtime.h>
#include <resonate/ui/tooltip.h>
#include <resonate/ui/transition.h>
#include <resonate/window/input.h>
#include <resonate/window/session.hpp>

#include "resonate.animation/components.gen.h"
#include "resonate.animation/nodes.gen.h"
#include "resonate.gameplay/nodes.gen.h"
#include "resonate.hierarchy/nodes.gen.h"
#include "resonate.render/nodes.gen.h"
#include "resonate.scene/nodes.gen.h"
#include "resonate.ui/components.gen.h"
#include "resonate.ui/nodes.gen.h"

namespace resonate::startup
{
namespace
{

/* The CVar appliers: the schema has validated the text, so parsing here is
   safe. The reads answer what the run is doing now, not what the config file
   asked for. */
std::string formatFloat(float value)
{
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
    return text;
}

bool applyRenderScale(void* context, const std::string& value, std::string&)
{
    static_cast<renderer::RenderSystem*>(context)->setRenderScale(
        std::strtof(value.c_str(), nullptr));
    return true;
}

std::string readRenderScale(void* context)
{
    return formatFloat(static_cast<const renderer::RenderSystem*>(context)->renderScale());
}

bool applyTargetFrameMs(void* context, const std::string& value, std::string&)
{
    static_cast<renderer::RenderSystem*>(context)->setTargetFrameMs(
        std::strtof(value.c_str(), nullptr));
    return true;
}

std::string readTargetFrameMs(void* context)
{
    return formatFloat(static_cast<const renderer::RenderSystem*>(context)->targetFrameMs());
}

bool applyMsaa(void* context, const std::string& value, std::string&)
{
    static_cast<renderer::RenderSystem*>(context)->setMsaa(
        static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)));
    return true;
}

std::string readMsaa(void* context)
{
    return std::to_string(static_cast<const renderer::RenderSystem*>(context)->msaa());
}

bool applyShadowSize(void* context, const std::string& value, std::string&)
{
    static_cast<renderer::RenderSystem*>(context)->setShadowSize(
        static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)));
    return true;
}

std::string readShadowSize(void* context)
{
    return std::to_string(static_cast<const renderer::RenderSystem*>(context)->shadowSize());
}

bool applyHdrFormat(void* context, const std::string& value, std::string& error)
{
    return static_cast<renderer::RenderSystem*>(context)->setHdrFormat(value, error);
}

std::string readHdrFormat(void* context)
{
    return static_cast<const renderer::RenderSystem*>(context)->hdrFormatName();
}

bool applyFeatureChain(void* context, const std::string& value, std::string& error)
{
    auto* render = static_cast<renderer::RenderSystem*>(context);
    renderer::FeatureStack stack;
    if (!renderer::parseFeatureStack(value, render->antialiasing(), stack, error))
    {
        return false;
    }
    render->setFeatures(stack);
    return true;
}

std::string readFeatureChain(void* context)
{
    return static_cast<const renderer::RenderSystem*>(context)->featureChain();
}

bool applyAntialiasing(void* context, const std::string& value, std::string& error)
{
    auto* render = static_cast<renderer::RenderSystem*>(context);
    renderer::FeatureStack stack;
    if (!renderer::parseFeatureStack(render->featureChain(), value, stack, error))
    {
        return false;
    }
    render->setFeatures(stack);
    return true;
}

std::string readAntialiasing(void* context)
{
    return static_cast<const renderer::RenderSystem*>(context)->antialiasing();
}

bool applyBloomThreshold(void* context, const std::string& value, std::string& error)
{
    auto* render = static_cast<renderer::RenderSystem*>(context);
    renderer::FeatureStack stack;
    if (!renderer::parseFeatureStack(render->featureChain(), render->antialiasing(), stack,
                                     error))
    {
        return false;
    }
    stack.bloom_threshold = std::strtof(value.c_str(), nullptr);
    render->setFeatures(stack);
    return true;
}

std::string readBloomThreshold(void* context)
{
    return formatFloat(static_cast<const renderer::RenderSystem*>(context)->bloomThreshold());
}

/* The hot-reload sinks: a changed key reaches the same appliers a console
   command does, so the file and the console cannot drift apart. */
struct LiveConfig
{
    startup::CVarHost* cvars = nullptr;
    Application::Config* config = nullptr;
    const SettingsSchema* schema = nullptr;
};

std::string dottedName(const char* section, const char* key)
{
    std::string full = section != nullptr ? section : "";
    if (key != nullptr)
    {
        if (!full.empty())
        {
            full.push_back('.');
        }
        full.append(key);
    }
    return full;
}

void applyLiveSetting(void* user, const char* section, const char* key, const char* value)
{
    auto* live = static_cast<LiveConfig*>(user);
    const std::string full = dottedName(section, key);
    std::string text = value != nullptr ? value : "";
    if (text.empty())
    {
        /* The key lost its value: the declaration's default applies. A key
           nothing declares has no default and is left alone. */
        const SettingDecl* declaration = live->schema->find(full.c_str());
        if (declaration == nullptr)
        {
            return;
        }
        text = declaration->default_value;
    }
    if (full == "resonate.loop.max-fps")
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
        live->config->max_fps = end != text.c_str() && *end == '\0' ? parsed : 0U;
        return;
    }
    /* Only bound keys have a live applier; the rest apply at the next start. */
    std::string probe;
    if (!live->cvars->read(full, probe))
    {
        return;
    }
    std::string error;
    if (!live->cvars->apply(full, text, error))
    {
        std::fprintf(stderr, "resonate: config: %s: %s\n", full.c_str(), error.c_str());
    }
}

/* Unknown keys warn and pass (a stale name must not block a reload); a bad
   value refuses it, which is what validate() does at start-up. */
bool validateLiveSetting(void* user, const char* section, const char* key, const char* value,
                         std::string& error)
{
    auto* live = static_cast<LiveConfig*>(user);
    const std::string full = dottedName(section, key);
    if (live->schema->find(full.c_str()) == nullptr)
    {
        std::fprintf(stderr, "resonate: config: unknown key '%s'\n", full.c_str());
        return true;
    }
    return live->schema->checkValue(full.c_str(), value != nullptr ? value : "", error);
}

void reportConfigMessage(void*, const char* message)
{
    std::fprintf(stderr, "resonate: %s\n", message != nullptr ? message : "");
}

/* The input system: one sample per frame, before anything gameplay reads the
   actions. A run without the input capability (headless) releases everything. */
/* Drives the demo machine's parameters from the keyboard: holding right
   writes 1 into input_x, left writes -1, nothing writes 0. A machine without
   that parameter, or a box whose machine was swapped, is left alone. */
struct MachineDrive
{
    ecs::Entity box{};
    animation::AnimationPlayer* player = nullptr;
    const ResonateInput* input = nullptr;
    std::uint32_t machine = 0U;
    std::uint32_t parameter = animation::kUnknownParameter;
    bool armed = false;

    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer&, float)
    {
        auto* drive = static_cast<MachineDrive*>(context);
        if (!drive->armed || drive->input == nullptr || !world.alive(drive->box))
        {
            return;
        }
        const auto* machine_state = world.get<animation::MachineState>(drive->box);
        if (machine_state == nullptr || machine_state->machine != drive->machine)
        {
            return;
        }
        const bool left = drive->input->key_down(drive->input->self, RESONATE_KEY_LEFT) != 0 ||
                          drive->input->key_down(drive->input->self, RESONATE_KEY_A) != 0;
        const bool right = drive->input->key_down(drive->input->self, RESONATE_KEY_RIGHT) != 0 ||
                           drive->input->key_down(drive->input->self, RESONATE_KEY_D) != 0;
        const float value = (right ? 1.0F : 0.0F) - (left ? 1.0F : 0.0F);
        auto* writable = const_cast<animation::MachineState*>(machine_state);
        writable->runtime.setValue(drive->parameter, value);
    }
};

/* The entity a document key was instantiated as. A load replaces the scene's
   handles, so a host that holds one (the demo's machine drive) finds its
   object again through the key the adapter stored beside it. */
ecs::Entity findSceneKey(ecs::World& world, std::string_view key)
{
    struct Search
    {
        ecs::World* world = nullptr;
        std::string_view key;
        ecs::Entity found{};
    };
    Search search{&world, key, {}};
    world.forEachEntity(
        &search,
        [](void* user_data, ecs::Entity entity)
        {
            auto* state = static_cast<Search*>(user_data);
            if (state->found.valid())
            {
                return;
            }
            const auto* source = state->world->get<scene::SourceKey>(entity);
            if (source == nullptr)
            {
                return;
            }
            const std::size_t size = state->world->blobSize(source->text);
            const void* const bytes = state->world->blobData(source->text);
            if (size == state->key.size() &&
                std::memcmp(bytes, state->key.data(), size) == 0)
            {
                state->found = entity;
            }
        });
    return search.found;
}

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
    const ResonateInput* input = nullptr;
    ui::TextRuntime* text = nullptr;
    /* Animated state changes advance before layout each frame. */
    ui::TransitionSystem* transitions = nullptr;
    /* The dwell timer of the hovered widget's tooltip; also before layout. */
    ui::TooltipSystem* tooltips = nullptr;
    /* The field the platform editor mirrored on the previous frame, so a field
       that takes focus fills the editor once — a direct move between fields
       must not leave the previous field's buffer behind. */
    ecs::Entity editor_entity{};
    /* The caret's blink phase and the field it belongs to; editing resets
       both, so the caret never flickers away mid-keystroke. */
    float blink_time = 0.0F;
    ecs::Entity blink_focus{};
    /* The run's clock as the input system sees it: it times double clicks. */
    double input_clock = 0.0;
    /* The pointer shape last handed to the platform. */
    ResonateCursorShape cursor = RESONATE_CURSOR_DEFAULT;
    /* The host's shortcut pass, after the UI has taken the keys it owns. */
    bool (*keys)(void* context, ecs::World& world, const ResonateInput* input) = nullptr;
    void* keys_context = nullptr;

    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer&, float delta_seconds)
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
        if (frame->transitions != nullptr)
        {
            frame->transitions->advance(world, delta_seconds);
        }
        frame->input_clock += delta_seconds;
        bool text_activity = false;
        /* The pointer sample the input system and the tooltip dwell share. */
        ResonateVec2 position{};
        if (frame->input != nullptr)
        {
            frame->input->pointer_position(frame->input->self, &position);
            const bool down =
                frame->input->mouse_down(frame->input->self, RESONATE_MOUSE_LEFT) != 0;
            if (!ui::updateInput(world, frame->canvas, position.x, position.y, down,
                                 frame->input_clock))
            {
                return;
            }
            /* The pointer's shape follows the widget under it and the gesture
               in flight; the platform is told only when it changes. */
            if (frame->input->set_cursor != nullptr)
            {
                ResonateCursorShape shape = RESONATE_CURSOR_DEFAULT;
                switch (ui::cursorShape(world, frame->canvas))
                {
                    case ui::CursorShape::Pointer:
                        shape = RESONATE_CURSOR_POINTER;
                        break;
                    case ui::CursorShape::Text:
                        shape = RESONATE_CURSOR_TEXT;
                        break;
                    case ui::CursorShape::ResizeHorizontal:
                        shape = RESONATE_CURSOR_RESIZE_H;
                        break;
                    case ui::CursorShape::ResizeVertical:
                        shape = RESONATE_CURSOR_RESIZE_V;
                        break;
                    case ui::CursorShape::NotAllowed:
                        shape = RESONATE_CURSOR_NOT_ALLOWED;
                        break;
                    case ui::CursorShape::Default:
                        break;
                }
                if (shape != frame->cursor)
                {
                    frame->cursor = shape;
                    frame->input->set_cursor(frame->input->self, shape);
                }
            }
            if (frame->input->pointer_wheel != nullptr)
            {
                ResonateVec2 wheel{};
                frame->input->pointer_wheel(frame->input->self, &wheel);
                (void)ui::scrollInput(world, frame->canvas, position.x, position.y, wheel.y);
            }
            if (frame->input->key_pressed(frame->input->self, RESONATE_KEY_TAB) != 0)
            {
                const bool backwards =
                    frame->input->key_down(frame->input->self, RESONATE_KEY_LSHIFT) != 0 ||
                    frame->input->key_down(frame->input->self, RESONATE_KEY_RSHIFT) != 0;
                (void)ui::focusNext(world, frame->canvas, backwards);
                /* Tab may land on a row below the fold: its viewport brings it
                   into view before this frame lays the tree out. */
                const ecs::Entity focused = ui::focusedEntity(world, frame->canvas);
                if (focused.valid())
                {
                    (void)ui::reveal(world, focused);
                }
            }
            /* Return and Space activate the focused widget; a text field takes
               the space as a character instead. */
            if (frame->input->key_pressed(frame->input->self, RESONATE_KEY_RETURN) != 0 ||
                (frame->input->key_pressed(frame->input->self, RESONATE_KEY_SPACE) != 0 &&
                 !ui::focusedTextEditable(world, frame->canvas)))
            {
                (void)ui::activateFocused(world, frame->canvas);
            }
            if (frame->text != nullptr)
            {
                /* The modifiers a caret key reads: Shift holds the anchor so
                   the range grows, Ctrl steps by word. */
                const bool shift =
                    frame->input->key_down(frame->input->self, RESONATE_KEY_LSHIFT) != 0 ||
                    frame->input->key_down(frame->input->self, RESONATE_KEY_RSHIFT) != 0;
                const bool ctrl =
                    frame->input->key_down(frame->input->self, RESONATE_KEY_LCTRL) != 0 ||
                    frame->input->key_down(frame->input->self, RESONATE_KEY_RCTRL) != 0;
                struct CaretKey
                {
                    ResonateKey key;
                    ui::CaretMotion plain;
                    ui::CaretMotion word;
                };
                static constexpr CaretKey kCaretKeys[] = {
                    {RESONATE_KEY_LEFT, ui::CaretMotion::Left, ui::CaretMotion::WordLeft},
                    {RESONATE_KEY_RIGHT, ui::CaretMotion::Right, ui::CaretMotion::WordRight},
                    {RESONATE_KEY_HOME, ui::CaretMotion::Home, ui::CaretMotion::Home},
                    {RESONATE_KEY_END, ui::CaretMotion::End, ui::CaretMotion::End},
                };
                for (const CaretKey& caret_key : kCaretKeys)
                {
                    if (frame->input->key_pressed(frame->input->self, caret_key.key) != 0)
                    {
                        (void)ui::moveCaret(world, frame->canvas,
                                            ctrl ? caret_key.word : caret_key.plain, shift);
                        text_activity = true;
                    }
                }
                if (ctrl &&
                    frame->input->key_pressed(frame->input->self, RESONATE_KEY_A) != 0)
                {
                    (void)ui::selectAll(world, frame->canvas);
                    text_activity = true;
                }
                /* Clipboard chords on the focused field: copy and cut take the
                   selection, paste lands where the caret is and replaces
                   whatever is selected. */
                if (ctrl && frame->input->set_clipboard != nullptr &&
                    frame->input->get_clipboard != nullptr)
                {
                    const ecs::Entity focused = ui::focusedEntity(world, frame->canvas);
                    std::uint32_t select_begin = 0U;
                    std::uint32_t select_end = 0U;
                    const bool selected =
                        focused.valid() &&
                        ui::selectionRange(world, focused, select_begin, select_end) &&
                        select_begin < select_end;
                    if (frame->input->key_pressed(frame->input->self, RESONATE_KEY_C) != 0 ||
                        frame->input->key_pressed(frame->input->self, RESONATE_KEY_X) != 0)
                    {
                        if (selected)
                        {
                            char clipboard[4096] = {};
                            std::string error;
                            const std::string content = ui::focusedText(world, frame->canvas);
                            const std::size_t begin = select_begin < content.size()
                                                          ? select_begin
                                                          : content.size();
                            const std::size_t end = select_end < content.size()
                                                        ? select_end
                                                        : content.size();
                            if (begin < end)
                            {
                                frame->input->set_clipboard(frame->input->self,
                                                            content.data() + begin,
                                                            static_cast<std::uint32_t>(end - begin));
                            }
                            if (frame->input->key_pressed(frame->input->self,
                                                          RESONATE_KEY_X) != 0)
                            {
                                (void)ui::updateTextInput(world, frame->canvas, *frame->text, "",
                                                          true, false, error);
                                text_activity = true;
                            }
                        }
                    }
                    else if (frame->input->key_pressed(frame->input->self, RESONATE_KEY_V) != 0)
                    {
                        char clipboard[4096] = {};
                        const std::uint32_t length =
                            frame->input->get_clipboard(frame->input->self, clipboard,
                                                        sizeof(clipboard));
                        if (length > 0U)
                        {
                            std::string error;
                            if (!ui::updateTextInput(world, frame->canvas, *frame->text,
                                                     std::string_view(clipboard, length), false,
                                                     false, error))
                            {
                                std::fprintf(stderr, "resonate: ui paste: %s\n", error.c_str());
                            }
                            text_activity = true;
                        }
                    }
                }
            }
            if (frame->text != nullptr && frame->input->text_editing != nullptr)
            {
                char editing[256] = {};
                const std::uint32_t editing_length = frame->input->text_editing(
                    frame->input->self, editing, sizeof(editing), nullptr);
                std::string error;
                if (!ui::updateTextComposition(world, frame->canvas, *frame->text,
                                               std::string_view(editing, editing_length), error))
                {
                    std::fprintf(stderr, "resonate: ui text composition: %s\n", error.c_str());
                }
            }
            if (frame->text != nullptr && frame->input->set_text_focus != nullptr)
            {
                /* The soft keyboard follows focus: it belongs to the text widget
                   that owns the caret, not to the window. */
                const bool editable = ui::focusedTextEditable(world, frame->canvas);
                const ecs::Entity focused = editable ? ui::focusedEntity(world, frame->canvas)
                                                     : ecs::Entity{};
                if (focused.valid() && focused != frame->editor_entity &&
                    frame->input->set_text_buffer != nullptr)
                {
                    /* The editor owns the buffer from here on, so it starts
                       from what the field already shows. */
                    const std::string field_text = ui::focusedText(world, frame->canvas);
                    frame->input->set_text_buffer(frame->input->self, field_text.data(),
                                                  static_cast<std::uint32_t>(field_text.size()));
                }
                frame->editor_entity = focused;
                frame->input->set_text_focus(frame->input->self, editable ? 1 : 0);
            }
            if (frame->text != nullptr && frame->input->text_snapshot != nullptr)
            {
                /* A platform editor reports its whole buffer; mirror it. An
                   empty answer is an emptied buffer and must clear the field. */
                char snapshot[512] = {};
                const std::uint32_t snapshot_length =
                    frame->input->text_snapshot(frame->input->self, snapshot, sizeof(snapshot));
                std::string error;
                if (!ui::setFocusedText(world, frame->canvas, *frame->text,
                                        std::string_view(snapshot, snapshot_length), error))
                {
                    std::fprintf(stderr, "resonate: ui text: %s\n", error.c_str());
                }
            }
            if (frame->text != nullptr)
            {
                char typed[256] = {};
                std::uint32_t typed_length = 0;
                if (frame->input->text_input != nullptr)
                {
                    typed_length = frame->input->text_input(frame->input->self, typed,
                                                            sizeof(typed));
                }
                const bool backspace =
                    frame->input->key_pressed(frame->input->self, RESONATE_KEY_BACKSPACE) != 0;
                const bool delete_forward =
                    frame->input->key_pressed(frame->input->self, RESONATE_KEY_DELETE) != 0;
                if (typed_length > 0U || backspace || delete_forward)
                {
                    text_activity = true;
                    std::string error;
                    if (!ui::updateTextInput(world, frame->canvas, *frame->text,
                                             std::string_view(typed, typed_length), backspace,
                                             delete_forward, error))
                    {
                        std::fprintf(stderr, "resonate: ui text input: %s\n", error.c_str());
                    }
                }
            }
            /* The host's own chords, polled after the UI took everything it
               owns: a field with focus sees its keys first. */
            if (frame->keys != nullptr)
            {
                (void)frame->keys(frame->keys_context, world, frame->input);
            }
        }
        /* The caret blinks while the field sits still and stays lit while it is
           being edited; a pointer press also lights it. */
        {
            ui::CaretBlink* blink = world.get<ui::CaretBlink>(frame->canvas);
            if (blink == nullptr)
            {
                blink = world.add(frame->canvas, ui::CaretBlink{});
            }
            if (blink != nullptr)
            {
                const ecs::Entity focused = ui::focusedEntity(world, frame->canvas);
                bool reset = focused != frame->blink_focus;
                frame->blink_focus = focused;
                if (frame->input != nullptr &&
                    frame->input->mouse_down(frame->input->self, RESONATE_MOUSE_LEFT) != 0)
                {
                    reset = true;
                }
                if (reset || text_activity)
                {
                    frame->blink_time = 0.0F;
                    blink->visible = true;
                }
                else
                {
                    frame->blink_time += delta_seconds;
                    if (frame->blink_time >= 0.53F)
                    {
                        frame->blink_time = 0.0F;
                        blink->visible = !blink->visible;
                    }
                }
            }
        }
        ui::updateBindings(world, frame->canvas);
        if (frame->text != nullptr)
        {
            std::string error;
            if (!ui::syncDropdowns(world, frame->canvas, error))
            {
                std::fprintf(stderr, "resonate: ui dropdown: %s\n", error.c_str());
            }
        }
        /* The dwell is sampled after the pointer's target: a tooltip that just
           appeared is placed and laid out in this same frame. */
        if (frame->tooltips != nullptr)
        {
            frame->tooltips->advance(world, frame->canvas, position.x, position.y, delta_seconds);
        }
        ui::layout(world, frame->canvas, static_cast<float>(width), static_cast<float>(height));
    }
};

ecs::Entity composeBuiltinHud(ecs::World& world);

} // namespace

/* The content vocabulary is every module's generated table behind one
   registry: a document may mix UI, hierarchy and render nodes. */
const dom::SchemaRegistry& contentSchema()
{
    static const dom::TableSchemaRegistry hierarchy_table(
        resonate::Span<const dom::NodeTypeDesc>(hierarchy::generated::kNodeTypes,
                                                hierarchy::generated::kNodeTypeCount),
        resonate::Span<const char* const>(hierarchy::generated::kComponents,
                                          hierarchy::generated::kComponentCount));
    static const dom::TableSchemaRegistry render_table(
        resonate::Span<const dom::NodeTypeDesc>(render::generated::kNodeTypes,
                                                render::generated::kNodeTypeCount),
        resonate::Span<const char* const>(render::generated::kComponents,
                                          render::generated::kComponentCount));
    static const dom::TableSchemaRegistry gameplay_table(
        resonate::Span<const dom::NodeTypeDesc>(gameplay::generated::kNodeTypes,
                                                gameplay::generated::kNodeTypeCount),
        resonate::Span<const char* const>(gameplay::generated::kComponents,
                                          gameplay::generated::kComponentCount));
    static const dom::TableSchemaRegistry animation_table(
        resonate::Span<const dom::NodeTypeDesc>(animation::generated::kNodeTypes,
                                                animation::generated::kNodeTypeCount),
        resonate::Span<const char* const>(animation::generated::kComponents,
                                          animation::generated::kComponentCount));
    static const dom::TableSchemaRegistry scene_table(
        resonate::Span<const dom::NodeTypeDesc>(scene::generated::kNodeTypes,
                                                scene::generated::kNodeTypeCount),
        resonate::Span<const char* const>(scene::generated::kComponents,
                                          scene::generated::kComponentCount));
    static const std::array<const dom::SchemaRegistry*, 6> tables{
        &ui::documentSchema(),      &hierarchy_table, &render_table,
        &gameplay_table,            &animation_table, &scene_table};
    static const dom::CompositeSchemaRegistry registry(
        resonate::Span<const dom::SchemaRegistry* const>(tables.data(), tables.size()));
    return registry;
}

const dom::ComponentRegistry& contentComponents()
{
    static const dom::TableComponentRegistry hierarchy_table(
        resonate::Span<const dom::ComponentDesc>(hierarchy::generated::kComponentDescs,
                                                 hierarchy::generated::kComponentDescCount));
    static const dom::TableComponentRegistry render_table(
        resonate::Span<const dom::ComponentDesc>(render::generated::kComponentDescs,
                                                 render::generated::kComponentDescCount));
    static const dom::TableComponentRegistry gameplay_table(
        resonate::Span<const dom::ComponentDesc>(gameplay::generated::kComponentDescs,
                                                 gameplay::generated::kComponentDescCount));
    static const dom::TableComponentRegistry animation_table(
        resonate::Span<const dom::ComponentDesc>(animation::generated::kComponentDescs,
                                                 animation::generated::kComponentDescCount));
    static const std::array<const dom::ComponentRegistry*, 5> tables{
        &ui::documentComponents(), &hierarchy_table, &render_table, &gameplay_table,
        &animation_table};
    static const dom::CompositeComponentRegistry registry(
        resonate::Span<const dom::ComponentRegistry* const>(tables.data(), tables.size()));
    return registry;
}

namespace
{

/* The run's UI string table: composition parses it, markup resolves its keys
   while the document instantiates, and a host reads message keys from it at
   runtime. Owned for the process because those messages outlive the frame
   loop's setup. */
std::unique_ptr<ui::StringTable> g_ui_strings;

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

/* Packaged platforms stage content into a flat directory and name it through
   `environment`; the staged file wins over the executable-relative walk. */
std::string resolveContentFile(const std::string& relative, const char* environment)
{
    if (const char* staged = std::getenv(environment))
    {
        const std::filesystem::path candidate =
            std::filesystem::path(staged) / std::filesystem::path(relative).filename();
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return candidate.string();
        }
    }
    return findContentFile(relative);
}

/* The content directory a shipped layout keeps beside the executable and the
   project tree keeps in the repository; empty when neither exists. */
std::string findContentDirectory(const std::string& relative)
{
    std::filesystem::path directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 6; ++level)
    {
        const std::filesystem::path candidate = directory / relative;
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error))
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

/* The staged-directory form of resolveContentFile: the environment names a
   whole directory the platform host extracted. */
std::string resolveContentDirectory(const std::string& relative, const char* environment)
{
    if (const char* staged = std::getenv(environment))
    {
        std::error_code error;
        if (std::filesystem::is_directory(std::filesystem::path(staged), error))
        {
            return staged;
        }
    }
    return findContentDirectory(relative);
}

bool readTextFile(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/* A document arrives as XML in the source tree and as the cooked binary in a
   packaged tree; the file's magic picks the codec, so the same load path serves
   both. */
std::unique_ptr<dom::Document> loadDocument(const std::string& path,
                                            std::vector<dom::Diagnostic>& diagnostics)
{
    std::string bytes;
    if (!readTextFile(path, bytes))
    {
        /* The callers print the diagnostics; a file that exists but cannot be
           read (locked, denied) must say so, not exit silently. */
        dom::Diagnostic diagnostic;
        diagnostic.severity = dom::Diagnostic::Severity::Error;
        diagnostic.message = "cannot read " + path;
        diagnostics.push_back(std::move(diagnostic));
        return nullptr;
    }
    const auto* first = reinterpret_cast<const std::byte*>(bytes.data());
    if (dom::Document::isBinary(Span<const std::byte>(first, bytes.size())))
    {
        return dom::Document::parseBinary(Span<const std::byte>(first, bytes.size()),
                                          diagnostics);
    }
    return dom::Document::parse(bytes, &contentSchema(), diagnostics);
}

/* The demo scene: content/scene/demo.rscene instantiated through the scene
   adapter. The middle box's state machine and the ceiling light grid stay
   procedural: animation authoring and stress fixtures are not content yet. */
void composeDemoScene(ecs::World& world, animation::AnimationPlayer* player,
                      std::vector<ecs::Entity>& out_roots, MachineDrive* machine_drive)
{
    /* A host that stages its assets elsewhere (Android) names the document
       directly; the setting and the content walk cover desktop runs. */
    const char* const staged = std::getenv("RESONATE_SCENE_DOCUMENT");
    const std::string path = staged != nullptr && *staged != '\0'
                                 ? std::string(staged)
                                 : findContentFile("content/scene/demo.rscene");
    if (path.empty())
    {
        std::fprintf(stderr, "resonate: scene: content/scene/demo.rscene is missing\n");
        return;
    }
    std::vector<dom::Diagnostic> diagnostics;
    auto document = loadDocument(path, diagnostics);
    if (document == nullptr)
    {
        for (const dom::Diagnostic& diagnostic : diagnostics)
        {
            std::fprintf(stderr, "resonate: scene: %s:%u:%u: %s\n", path.c_str(),
                         diagnostic.line, diagnostic.column, diagnostic.message.c_str());
        }
        return;
    }

    ecs::Entity root{};
    std::string error;

    /* The manifest loads before the scene: the adapter resolves machine clips
       through it while instantiating. */
    if (player != nullptr)
    {
        const char* const staged_products = std::getenv("RESONATE_PRODUCTS");
        const std::string products =
            staged_products != nullptr && *staged_products != '\0'
                ? std::string(staged_products)
                : findContentFile("build/content/products.toml");
        if (!products.empty())
        {
            std::string products_error;
            if (!player->loadManifest(products, products_error))
            {
                std::fprintf(stderr, "resonate: animation: %s\n", products_error.c_str());
            }
        }
    }

    if (!scene::instantiate(world, *document, &contentComponents(), root, error, &out_roots,
                            player))
    {
        std::fprintf(stderr, "resonate: scene: %s: %s\n", path.c_str(), error.c_str());
        return;
    }

    /* The demo machine's input_x parameter follows the keyboard, so the
       authored conditions actually fire. */
    if (machine_drive != nullptr)
    {
        const ecs::Entity box = scene::findByKey(world, root, "box-center");
        const auto* machine_state =
            box.valid() ? world.get<animation::MachineState>(box) : nullptr;
        if (machine_state != nullptr)
        {
            const animation::StateMachine* definition =
                player != nullptr ? player->machine(machine_state->machine) : nullptr;
            if (definition != nullptr)
            {
                machine_drive->box = box;
                machine_drive->machine = machine_state->machine;
                machine_drive->parameter = definition->parameterIndex("input_x");
                machine_drive->armed = machine_drive->parameter != animation::kUnknownParameter;
            }
        }
    }

    /* The cluster acceptance wants a hundred lights and change: a ceiling grid
       of warm and cool points over the field. */
    for (int x = 0; x < 12; ++x)
    {
        for (int z = 0; z < 12; ++z)
        {
            const ecs::Entity light = world.create();
            const Vec3 position{static_cast<float>(x - 5) * 2.2F, 2.2F,
                                static_cast<float>(z - 5) * 2.2F};
            const float mix = (x + z) % 2 == 0 ? 1.0F : 0.0F;
            const Vec3 color{0.30F + 0.65F * mix, 0.45F, 0.85F - 0.45F * mix};
            world.add(light, hierarchy::LocalTransform{position, resonate::Quat{},
                                                       Vec3{1.0F, 1.0F, 1.0F}});
            world.add(light, hierarchy::WorldTransform{});
            world.add(light, render::PointLight{color, 1.4F, 5.0F});
        }
    }
}

/* The configured UI document, instantiated into the world. A missing default
   document falls back to the built-in HUD; a configured one or a broken one
   ends the run with the reason. */
ecs::Entity loadDemoHud(ecs::World& world, const std::string& configured, bool required,
                        ui::TextRuntime& text_runtime, const ui::StringTable* strings,
                        const ui::ImageRegistry* images, const ui::IconRegistry* icons)
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

    std::vector<dom::Diagnostic> diagnostics;
    auto document = loadDocument(path, diagnostics);
    if (document == nullptr)
    {
        for (const dom::Diagnostic& diagnostic : diagnostics)
        {
            std::fprintf(stderr, "resonate: ui document: %s:%u:%u: %s\n", path.c_str(),
                         diagnostic.line, diagnostic.column, diagnostic.message.c_str());
        }
        return ecs::Entity{};
    }

    /* External stylesheets: a src resolves beside the document first, then
       through the content walk. A broken sheet ends the document the same way
       a broken document does. */
    struct StylesheetSource
    {
        std::string directory;
    };
    StylesheetSource stylesheet_source{std::filesystem::path(path).parent_path().string()};
    std::vector<dom::Diagnostic> style_diagnostics;
    const bool styles_ok = ui::loadStylesheets(
        *document, &stylesheet_source,
        [](void* context, std::string_view source_path, std::string& out_text)
        {
            auto* source = static_cast<StylesheetSource*>(context);
            std::error_code ignored;
            const std::filesystem::path base(source->directory);
            if (!base.empty())
            {
                /* Beside the document, by path and then by name: a host that
                   stages the UI assets into one flat directory answers the
                   second. */
                for (const std::filesystem::path& candidate :
                     {base / std::filesystem::path(source_path),
                      base / std::filesystem::path(source_path).filename()})
                {
                    if (std::filesystem::is_regular_file(candidate, ignored) &&
                        readTextFile(candidate.string(), out_text))
                    {
                        return true;
                    }
                }
            }
            const std::string found = findContentFile(std::string(source_path));
            return !found.empty() && readTextFile(found, out_text);
        },
        style_diagnostics);
    for (const dom::Diagnostic& diagnostic : style_diagnostics)
    {
        std::fprintf(stderr, "resonate: ui stylesheet: %s\n", diagnostic.message.c_str());
    }
    if (!styles_ok)
    {
        return ecs::Entity{};
    }
    const ecs::Entity canvas = ui::instantiate(world, *document, &contentComponents(),
                                               text_runtime, strings, images, icons);
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

const ui::StringTable* stringTable()
{
    return g_ui_strings.get();
}

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

int run(const std::vector<std::string>& arguments, const char* default_title,
        const HostExtension& extension)
{
#if defined(RESONATE_PLATFORM_WINDOWS)
    installCrashHandler();
#endif
    Application::Config config = Application::configFromCommandLine(arguments);
    const DisplayOptions display = displayOptionsFromCommandLine(arguments);

    /* The configuration loads here, before anything consumes it: the window's
       size is a setting too. No file (or an unparsable one) means defaults
       everywhere. */
    if (config.config_file.empty())
    {
        config.config_file = "resonate.toml";
    }
    /* Sparse layers, low to high: platform, project, user. A missing layer is
       normal; the CVar layer sits above these in the store itself. */
    config.settings.load(std::string("resonate.") + RESONATE_PLATFORM_NAME + ".toml");
    config.settings.addFile(config.config_file);
    config.settings.addFile("resonate.user.toml");

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
    animation::AnimationPlayer animation_player;
    /* The descriptor needs a plain function: a shim forwards the frame's
       delta to the player. */
    const EngineSystemDesc animation_desc{
        "animation.player", RESONATE_STAGE_UPDATE,
        [](void* context, ecs::World& world, ecs::CommandBuffer&, float delta)
        { static_cast<animation::AnimationPlayer*>(context)->invoke(world, delta); },
        &animation_player};
    /* The asset store the player resolves authored clips through. It outlives
       the run like the player; the directory backend reads the products
       manifest the build staged. The dev tree keeps it under build/; a shipped
       layout names it inside the content tree beside the products. */
    std::unique_ptr<asset::Vfs> asset_store;
    {
        const char* const staged_products = std::getenv("RESONATE_PRODUCTS");
        std::string products =
            staged_products != nullptr && *staged_products != '\0'
                ? std::string(staged_products)
                : findContentFile("build/content/products.toml");
        if (products.empty())
        {
            products = findContentFile("content/products.toml");
        }
        if (!products.empty())
        {
            std::string store_error;
            asset_store = asset::DirectoryVfs::open(products, store_error);
            if (asset_store == nullptr)
            {
                std::fprintf(stderr, "resonate: assets: %s\n", store_error.c_str());
            }
        }
    }
    animation_player.setAssetStore(asset_store.get());
    /* The demo scene's driver: both the context and its descriptor outlive the
       run, because the host keeps the descriptor's address for every frame. */
    MachineDrive machine_drive;
    const EngineSystemDesc machine_drive_desc{"demo.machine_drive", RESONATE_STAGE_UPDATE,
                                              &MachineDrive::invoke, &machine_drive};
    /* The texture cook writes its own manifest; every product in it is decoded
       once by the renderer, so authored TextureRefs resolve by id. The store
       lives longer than the render system: a declaration after the render
       system's would tear it down under the render thread's loads. */
    std::unique_ptr<asset::Vfs> texture_store;
    {
        /* The dev tree keeps the manifest under build/; a shipped layout walks
           up from the executable like the content walk, and a staged
           deployment names the manifest outright. */
        const char* const staged = std::getenv("RESONATE_TEXTURE_PRODUCTS");
        std::string products = staged != nullptr && *staged != '\0'
                                   ? std::string(staged)
                                   : findContentFile("build/content/textures/products.toml");
        if (products.empty())
        {
            std::filesystem::path directory = resonate_pal_io_executable_directory();
            for (int level = 0; level < 6 && products.empty(); ++level)
            {
                const std::filesystem::path candidate =
                    directory / "content" / "textures" / "products.toml";
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error))
                {
                    products = candidate.string();
                }
                else if (!directory.has_parent_path())
                {
                    break;
                }
                else
                {
                    directory = directory.parent_path();
                }
            }
        }
        if (!products.empty())
        {
            std::string store_error;
            texture_store = asset::DirectoryVfs::open(products, store_error);
            if (texture_store == nullptr)
            {
                std::fprintf(stderr, "resonate: textures: %s\n", store_error.c_str());
            }
        }
    }
    renderer::RenderSystem render_system;
    render_system.setAssetStore(texture_store.get());
    /* Resolution trades against frame time; a handheld GPU cannot afford the
       full 2400-wide target for this pipeline, so the budget adapts the scale
       instead of leaving the device at 14 fps. */
    render_system.setRenderScale(
        schema.readFloat(config.settings, "resonate.render.scale", 1.0F));
    render_system.setTargetFrameMs(
        schema.readFloat(config.settings, "resonate.render.target-frame-ms", 0.0F));

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

    /* The HDR target's format and the shadow map's extent are configurably
       data too: an unknown name ends the run with the reason. */
    {
        std::string error;
        if (!render_system.setHdrFormat(
                schema.readString(config.settings, "resonate.render.hdr-format", "auto"), error))
        {
            std::fprintf(stderr, "resonate: hdr format: %s\n", error.c_str());
            return EXIT_FAILURE;
        }
    }
    render_system.setShadowSize(
        schema.readUInt(config.settings, "resonate.render.shadow-size", 0U));
    render_system.setMsaa(schema.readUInt(config.settings, "resonate.render.msaa", 0U));

    /* The run's CVars: settings keys bound to the live systems, so a console
       command reaches the running renderer instead of only the config file.
       The schema validates a command's value before the applier runs. */
    CVarHost cvars;
    cvars.setSchema(&schema);
    cvars.bind({"resonate.render.scale", &render_system, &applyRenderScale, &readRenderScale});
    cvars.bind({"resonate.render.target-frame-ms", &render_system, &applyTargetFrameMs,
                &readTargetFrameMs});
    cvars.bind({"resonate.render.msaa", &render_system, &applyMsaa, &readMsaa});
    cvars.bind({"resonate.render.shadow-size", &render_system, &applyShadowSize, &readShadowSize});
    cvars.bind({"resonate.render.hdr-format", &render_system, &applyHdrFormat, &readHdrFormat});
    cvars.bind({"resonate.render.features", &render_system, &applyFeatureChain, &readFeatureChain});
    cvars.bind({"resonate.render.antialiasing", &render_system, &applyAntialiasing,
                &readAntialiasing});
    cvars.bind({"resonate.render.bloom-threshold", &render_system, &applyBloomThreshold,
                &readBloomThreshold});
    if (extension.cvars != nullptr)
    {
        *extension.cvars = cvars;
    }

    /* Hot reload: the watched layer files feed the same appliers. The sinks
       and `schema` outlive the run (they end with this function, after
       Application::run returns). */
    LiveConfig live{&cvars, &config, &schema};
    config.settings.setChangeSink(&live, &applyLiveSetting);
    config.settings.setValidation(&live, &validateLiveSetting);
    config.settings.setMessageSink(&live, &reportConfigMessage);
    config.settings.watchFiles();

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
    std::unique_ptr<ui::TextRuntime> ui_text;
    /* Bound to the world inside the systems hook; the frame keeps the raw
       pointer, so the owner lives as long as the run. */
    std::unique_ptr<ui::TransitionSystem> ui_transitions;
    std::unique_ptr<ui::TooltipSystem> ui_tooltips;
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

    /* The scene's persistence state: the roots the document produced, the world
       the run hooks receive, and the input the save keys read. The world only
       exists inside Application::run, so the systems hook records its address
       for the per-frame handling below. */
    std::vector<ecs::Entity> scene_roots;
    ecs::World* run_world = nullptr;
    const ResonateInput* run_input = nullptr;
    config.systems = [&](ModuleHost& host, ecs::World& world)
    {
        run_world = &world;
        if (!hierarchy::registerComponents(world) ||
            !animation::AnimationPlayer::registerComponents(world) ||
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
            std::uint32_t input_version = 0;
            void* input = resonate_capability_find(
                host.capabilities(), detail::CapabilityTraits<ResonateInput>::id.value(),
                detail::CapabilityTraits<ResonateInput>::version, &input_version);
            run_input = static_cast<const ResonateInput*>(input);
            ui_frame.input = run_input;
            /* Primary font first; the runtime walks the chain per code point. */
            std::vector<std::shared_ptr<const text::Font>> ui_fonts;
            std::vector<std::string> ui_font_paths;
            /* The chain pairs one face per weight bucket; shaping walks it per
               code point and prefers the face matching the run's weight. One
               family covers Latin and Chinese, so the design stays coherent. */
            for (const char* name :
                 {"content/ui/fonts/NotoSansSC-Regular.ttf",
                  "content/ui/fonts/NotoSansSC-Medium.ttf",
                  "content/ui/fonts/NotoSansSC-Bold.ttf"})
            {
                const std::string font_path = resolveContentFile(name, "RESONATE_FONT_PATH");
                if (font_path.empty())
                {
                    continue;
                }
                ui_font_paths.push_back(font_path);
                std::string font_error;
                std::shared_ptr<const text::Font> font =
                    text::Font::loadFile(font_path, font_error);
                if (font == nullptr)
                {
                    std::fprintf(stderr, "resonate: ui font %s: %s\n", name,
                                 font_error.c_str());
                    continue;
                }
                ui_fonts.push_back(std::move(font));
            }
            render_system.setUiFontPaths(std::move(ui_font_paths));
            ui_text = std::make_unique<ui::TextRuntime>(std::move(ui_fonts));
            ui_frame.transitions = nullptr;
            ui_transitions = std::make_unique<ui::TransitionSystem>();
            if (ui_transitions->bind(world))
            {
                ui_frame.transitions = ui_transitions.get();
            }
            ui_frame.tooltips = nullptr;
            ui_tooltips = std::make_unique<ui::TooltipSystem>();
            if (ui_tooltips->bind(world))
            {
                ui_frame.tooltips = ui_tooltips.get();
            }
            /* Picture images: every file under content/ui/images registers
               under its file name; sorted, so the ids are deterministic. */
            auto ui_images = std::make_unique<ui::ImageRegistry>();
            const std::string images_directory =
                resolveContentDirectory("content/ui/images", "RESONATE_IMAGE_PATH");
            if (!images_directory.empty())
            {
                std::vector<std::filesystem::path> files;
                std::error_code scan_error;
                for (const std::filesystem::directory_entry& entry :
                     std::filesystem::directory_iterator(images_directory, scan_error))
                {
                    if (entry.is_regular_file(scan_error))
                    {
                        files.push_back(entry.path());
                    }
                }
                std::sort(files.begin(), files.end());
                for (const std::filesystem::path& file : files)
                {
                    render_system.setUiImageTexture(ui_images->registerImage(
                                                        file.filename().string()),
                                                    file.string());
                }
            }
            g_ui_strings.reset();
            const std::string strings_path =
                resolveContentFile("content/ui/strings.rstrings", "RESONATE_STRINGS_PATH");
            if (!strings_path.empty())
            {
                std::string xml;
                std::string strings_error;
                if (!readTextFile(strings_path, xml))
                {
                    std::fprintf(stderr, "resonate: ui strings: cannot read %s\n",
                                 strings_path.c_str());
                    return RESONATE_E_INVALID;
                }
                const auto* const bytes = reinterpret_cast<const std::byte*>(xml.data());
                if ((g_ui_strings = ui::StringTable::parseBytes(
                         Span<const std::byte>(bytes, xml.size()), strings_error)) == nullptr)
                {
                    std::fprintf(stderr, "resonate: ui strings: %s\n", strings_error.c_str());
                    return RESONATE_E_INVALID;
                }
            }
            /* Icons: the cooker's map names the sub-rect of icons.png each icon
               lives in, and the image scan above registered that file. */
            std::unique_ptr<ui::IconRegistry> ui_icons = std::make_unique<ui::IconRegistry>();
            const std::string icons_path =
                resolveContentFile("content/ui/icons.icons", "RESONATE_ICONS_PATH");
            if (!icons_path.empty())
            {
                std::string icons_text;
                std::string icons_error;
                if (readTextFile(icons_path, icons_text) && ui_icons->load(icons_text, icons_error))
                {
                    ui_icons->setTexture(ui_images->find("icons.png"));
                }
                else
                {
                    std::fprintf(stderr, "resonate: ui icons: %s\n",
                                 icons_error.empty() ? icons_path.c_str() : icons_error.c_str());
                }
            }
            ui_frame.text = ui_text.get();
            ui_frame.keys = extension.keys;
            ui_frame.keys_context = extension.keys_context;
            ui_frame.canvas = loadDemoHud(world, ui_document, ui_document_required, *ui_text,
                                          g_ui_strings.get(), ui_images.get(), ui_icons.get());
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
            addEngineSystem(host, render_desc) != RESONATE_OK ||
            addEngineSystem(host, animation_desc) != RESONATE_OK)
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
        machine_drive.player = &animation_player;
        composeDemoScene(world, &animation_player, scene_roots, &machine_drive);
        if (void* input = resonate_capability_find(
                host.capabilities(), detail::CapabilityTraits<ResonateInput>::id.value(),
                detail::CapabilityTraits<ResonateInput>::version, nullptr);
            input != nullptr)
        {
            run_input = static_cast<const ResonateInput*>(input);
            machine_drive.input = run_input;
        }
        if (addEngineSystem(host, machine_drive_desc) != RESONATE_OK)
        {
            return RESONATE_E_INVALID;
        }
        if (extension.scene != nullptr)
        {
            extension.scene(extension.scene_context, world, scene_roots);
        }
        /* The host's own schedule entries come after the engine's, so they see
           this frame's layout results; the ready hook runs once the document
           exists. */
        for (const EngineSystemDesc& system : extension.systems)
        {
            if (addEngineSystem(host, system) != RESONATE_OK)
            {
                return RESONATE_E_INVALID;
            }
        }
        if (extension.ready != nullptr)
        {
            extension.ready(extension.ready_context, world, ui_frame.canvas);
        }
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

    /* F5/F9: the demo's quick save and load. A host that owns the scene itself
       (the editor) handles its own persistence; the demo is the one that keeps
       its scene in `scene_roots`. The keys are polled after the pump, so the
       frame that saw the press is the frame that acts on it. */
    const bool save_keys = extension.scene == nullptr;
    auto handleSaveKeys = [&]
    {
        if (!save_keys || run_input == nullptr || run_world == nullptr)
        {
            return;
        }
        if (run_input->key_pressed(run_input->self, RESONATE_KEY_F5) != 0)
        {
            std::string save_error;
            if (saveScene(*run_world, scene_roots, "quicksave", save_error))
            {
                std::fprintf(stderr, "resonate: save: quicksave written\n");
            }
            else
            {
                std::fprintf(stderr, "resonate: save: %s\n", save_error.c_str());
            }
        }
        if (run_input->key_pressed(run_input->self, RESONATE_KEY_F9) != 0)
        {
            std::vector<ecs::Entity> loaded;
            std::string load_error;
            if (loadScene(*run_world, scene_roots, "quicksave", loaded, load_error))
            {
                scene_roots = std::move(loaded);
                /* The demo's machine drive holds a handle the load replaced;
                   the box it drives is found again through its authored key. */
                machine_drive.box = findSceneKey(*run_world, "box-center");
                std::fprintf(stderr, "resonate: save: quicksave loaded\n");
            }
            else
            {
                std::fprintf(stderr, "resonate: save: %s\n", load_error.c_str());
            }
        }
    };

    if (display.headless)
    {
        config.publish = [width, height](ResonateCapabilityRegistry* registry)
        { return render::publishOffscreen(registry, width, height); };

        /* Nothing to pump, so the run ends when it is asked to. */
        config.tick = [&]
        {
            handleSaveKeys();
            return true;
        };
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
        config.tick = [&]
        {
            if (!session.pump())
            {
                return false;
            }
            handleSaveKeys();
            return true;
        };

        /* Presses latch until a step reads them; the loop clears them after. */
        config.clear_presses = [&session] { session.clearKeyPresses(); };
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
