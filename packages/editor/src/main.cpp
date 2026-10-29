#include <resonate/startup/startup.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include <resonate/ecs/world.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/pal/io.h>
#include <resonate/startup/cvar.h>
#include "resonate.render/components.gen.h"
#include <resonate/ui/document.h>
#include <resonate/ui/event.h>
#include <resonate/ui/text_runtime.h>

#include "resonate.ui/components.gen.h"

#ifdef RESONATE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

namespace
{

using resonate::ecs::Entity;

/* The editor's own state: the widgets it addresses by key and what the
   inspector currently shows. */
struct EditorState
{
    Entity canvas{};
    Entity scene_list{};
    Entity name_field{};
    Entity status{};
    Entity status_count{};
    Entity status_fps{};
    Entity console{};
    Entity shown_row{};

    /* The scrubbable properties: the label the drag moves and the text that
       shows its value. */
    struct Scrub
    {
        Entity handle{};
        Entity value_label{};
        float shown = std::numeric_limits<float>::quiet_NaN();
    };
    Scrub opacity{};
    Scrub roughness{};
    Scrub metallic{};

    /* The name field mirrors the selection; a user edit writes back. */
    std::string mirrored_name{};

    /* The position fields mirror the selection's LocalTransform; a field the
       user typed a number into writes back. */
    struct AxisField
    {
        Entity handle{};
        std::string mirrored{};
    };
    AxisField position_x{};
    AxisField position_y{};
    AxisField position_z{};

    /* The boxes the opacity scrub dims: one query, built on ready. */
    resonate::ecs::Query boxes{};
    std::vector<std::pair<resonate::ecs::Entity, resonate::ecs::Entity>> box_rows; /* tree row -> box */

    /* The console mirrors the world's report stream: a ring of the latest
       lines, flushed into the label when a new one arrives. */
    std::deque<std::string> console_lines;
    bool console_dirty = false;

    /* The command line: the run button latches, the tick executes after the
       dispatch has finished. */
    Entity command_field{};
    bool command_pending = false;

    /* The status bar's segments, rewritten only when their text changes. */
    std::string shown_count{};
    std::string shown_fps{};

    /* The pane geometry the splitters drive: remembered, persisted to a small
       file beside the executable, and restored on ready. */
    Entity scene_pane{};
    Entity splitter_left{};
    Entity viewport{};
    Entity viewport_hint{};
    Entity splitter_right{};
    Entity inspector{};
    Entity console_pane{};
    float shown_scene_width = 280.0F;
    float shown_inspector_width = 280.0F;
    float save_delay = 0.0F;
};

/* The run's CVars: startup copies the engine's bindings in through the host
   extension before the frame loop starts. */
resonate::startup::CVarHost cvar_host;

/* A scrub value is mirrored into its label only when it changes. */
void mirrorScrub(Entity canvas, resonate::ecs::World& world, EditorState::Scrub& scrub)
{
    float value = 0.0F;
    if (!resonate::ui::scrubValue(world, scrub.handle, value) || value == scrub.shown)
    {
        return;
    }
    scrub.shown = value;
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
    std::string error;
    (void)resonate::ui::setText(world, canvas, scrub.value_label, text, error);
}

/* The material pass: the opacity scrub scales every box's tint and the box
   whose tree row is selected brightens on top, so the selection is visible
   in the viewport itself. The roughness and metallic scrubs write the
   selected box's material; the selection seeds them, so the drag continues
   from the object's own value. */
void applyMaterials(EditorState& state, resonate::ecs::World& world,
                    resonate::ecs::Entity selected_row)
{
    const float opacity = std::clamp(state.opacity.shown, 0.0F, 1.0F);
    resonate::ecs::Entity selected_box{};
    for (const auto& [row, box] : state.box_rows)
    {
        if (row == selected_row)
        {
            selected_box = box;
            break;
        }
    }
    struct OpacityContext
    {
        resonate::ecs::World* world;
        float value;
        resonate::ecs::Entity selected_box;
        float roughness;
        float metallic;
    } context{&world, opacity, selected_box, std::clamp(state.roughness.shown, 0.0F, 1.0F),
              std::clamp(state.metallic.shown, 0.0F, 1.0F)};
    state.boxes.forEachChunk(
        [](void* context, resonate::ecs::ChunkView view)
        {
            auto* state = static_cast<OpacityContext*>(context);
            resonate::ecs::Column<resonate::render::Material> material =
                resonate::ecs::column<resonate::render::Material>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                const bool selected = view.entities[row] == state->selected_box;
                const float value = state->value * (selected ? 2.0F : 1.0F);
                material.data[row].tint = resonate::Vec3{value, value, value};
                if (selected)
                {
                    material.data[row].roughness = state->roughness;
                    material.data[row].metallic = state->metallic;
                }
                state->world->markChanged(
                    view.entities[row],
                    resonate::ecs::ComponentTraits<resonate::render::Material>::index);
            }
        },
        &context);
}

/* One scene-tree row cloned from an authored template: the widgets and their
   compiled styles copy, the caption reshapes through the canvas runtime. */
resonate::ecs::Entity cloneRow(EditorState& state, resonate::ecs::World& world, resonate::ecs::Entity template_row,
                     resonate::ecs::Entity list, const std::string& name)
{
    resonate::ecs::CommandBuffer commands{world};
    const resonate::ecs::Entity row = world.create();
    if (const auto* transform = world.get<resonate::ui::RectTransform>(template_row))
    {
        world.add(row, *transform);
    }
    if (const auto* background = world.get<resonate::ui::Background>(template_row))
    {
        world.add(row, *background);
    }
    if (const auto* input = world.get<resonate::ui::Input>(template_row))
    {
        world.add(row, *input);
    }
    if (const auto* selectable = world.get<resonate::ui::Selectable>(template_row))
    {
        world.add(row, resonate::ui::Selectable{false, selectable->color});
    }
    if (const auto* styles = world.get<resonate::ui::StyleRef>(template_row))
    {
        world.add(row, *styles);
    }
    if (const auto* state_styles = world.get<resonate::ui::StateStyles>(template_row))
    {
        if (world.alive(state_styles->records))
        {
            const void* bytes = world.blobData(state_styles->records);
            const std::uint64_t size = world.blobSize(state_styles->records);
            if (bytes != nullptr && size > 0U)
            {
                /* The record blob is per entity: the clone copies the bytes and
                   points its own StateStyles at the copy. */
                const resonate::ecs::BlobHandle records = world.createBlob(row, size, bytes);
                if (records.valid())
                {
                    world.add(row, resonate::ui::StateStyles{records});
                }
            }
        }
    }
    resonate::hierarchy::setParent(commands, row, list);

    /* The caption: the template's first Text descendant, reshaped to `name`. */
    std::vector<resonate::ecs::Entity> stack{template_row};
    while (!stack.empty())
    {
        const resonate::ecs::Entity current = stack.front();
        stack.erase(stack.begin());
        if (resonate::ui::Text* text = world.get<resonate::ui::Text>(current);
            text != nullptr && current != template_row)
        {
            const resonate::ecs::Entity caption = world.create();
            resonate::ui::Text shaped = *text;
            shaped.content = {};
            world.add(caption, shaped);
            if (const auto* transform = world.get<resonate::ui::RectTransform>(current))
            {
                world.add(caption, *transform);
            }
            std::string error;
            (void)resonate::ui::setText(world, state.canvas, caption, name, error);
            resonate::hierarchy::setParent(commands, caption, row);
            world.play(commands);
            return row;
        }
        for (const resonate::ecs::Entity child : resonate::hierarchy::children(world, current))
        {
            stack.push_back(child);
        }
    }
    world.play(commands);
    return row;
}

/* The world's report sink: the console's ring keeps the latest lines. */
void reportSink(void* context, resonate::ecs::World::Report, const char* message)
{
    auto* state = static_cast<EditorState*>(context);
    state->console_lines.emplace_back(message != nullptr ? message : "");
    while (state->console_lines.size() > 8U)
    {
        state->console_lines.pop_front();
    }
    state->console_dirty = true;
}

/* Flushes the console ring into its label: one line of text, reshaped only
   when a report arrived. */
void mirrorConsole(EditorState& state, resonate::ecs::World& world)
{
    if (!state.console_dirty)
    {
        return;
    }
    state.console_dirty = false;
    std::string joined;
    for (const std::string& line : state.console_lines)
    {
        if (!joined.empty())
        {
            joined += "  |  ";
        }
        joined += line;
    }
    std::string error;
    (void)resonate::ui::setText(world, state.canvas, state.console, joined, error);
}

/* The entity a tree row stands for, or an invalid one for the authored rows
   the document ships (the camera and the sun have no box behind them). */
Entity entityOfRow(const EditorState& state, Entity row)
{
    for (const auto& [tree_row, box] : state.box_rows)
    {
        if (tree_row == row)
        {
            return box;
        }
    }
    return Entity{};
}

/* The editor's pane widths, beside the executable: two numbers the splitters
   own. A missing or malformed file keeps the authored defaults. */
std::string layoutPath()
{
    const char* directory = resonate_pal_io_executable_directory();
    return std::string(directory != nullptr ? directory : ".") + "/resonate.editor.layout";
}

void loadLayout(float& scene_width, float& inspector_width)
{
    std::FILE* file = std::fopen(layoutPath().c_str(), "rb");
    if (file == nullptr)
    {
        return;
    }
    char line[128] = {};
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        char key[64] = {};
        float value = 0.0F;
        if (std::sscanf(line, "%63[^=]=%f", key, &value) == 2)
        {
            const std::string name = key;
            if (name == "scene-width" && value > 0.0F)
            {
                scene_width = value;
            }
            else if (name == "inspector-width" && value > 0.0F)
            {
                inspector_width = value;
            }
        }
    }
    std::fclose(file);
}

void saveLayout(float scene_width, float inspector_width)
{
    std::FILE* file = std::fopen(layoutPath().c_str(), "wb");
    if (file == nullptr)
    {
        return;
    }
    std::fprintf(file, "scene-width=%.0f", static_cast<double>(scene_width));
    std::fputc('\n', file);
    std::fprintf(file, "inspector-width=%.0f", static_cast<double>(inspector_width));
    std::fputc('\n', file);
    std::fclose(file);
}

/* Places the panes around two widths: the scene pane on the left, the
   inspector on the right, the viewport and the console between them. */
void applyLayout(EditorState& state, resonate::ecs::World& world, float scene_width,
                 float inspector_width)
{
    const auto place = [&](Entity entity, float x, float width)
    {
        if (resonate::ui::RectTransform* rect = world.get<resonate::ui::RectTransform>(entity))
        {
            rect->x = x;
            rect->width = width;
            (void)world.markChanged<resonate::ui::RectTransform>(entity);
        }
    };
    place(state.scene_pane, 0.0F, scene_width);
    place(state.splitter_left, scene_width, 6.0F);
    place(state.viewport, scene_width + 6.0F, -(scene_width + inspector_width + 12.0F));
    if (resonate::ui::RectTransform* hint = world.get<resonate::ui::RectTransform>(state.viewport_hint))
    {
        hint->x = scene_width + 22.0F;
        (void)world.markChanged<resonate::ui::RectTransform>(state.viewport_hint);
    }
    place(state.console_pane, scene_width + 6.0F, -(scene_width + inspector_width + 12.0F));
    place(state.splitter_right, -(inspector_width + 6.0F), 6.0F);
    place(state.inspector, -inspector_width, inspector_width);
    state.shown_scene_width = scene_width;
    state.shown_inspector_width = inspector_width;
}

/* The material scrubs follow the selected object: the scrubber takes the
   value the object already carries, and the mirror rewrites its label on the
   next tick (shown resets so the rewrite is not skipped). */
void seedMaterial(const EditorState& state, resonate::ecs::World& world, Entity entity)
{
    const resonate::render::Material* material =
        entity.valid() ? world.get<resonate::render::Material>(entity) : nullptr;
    const float roughness = material != nullptr ? material->roughness : 0.5F;
    const float metallic = material != nullptr ? material->metallic : 0.0F;
    const std::pair<const EditorState::Scrub*, float> seeds[2] = {
        {&state.roughness, roughness}, {&state.metallic, metallic}};
    for (const auto& [scrub, value] : seeds)
    {
        if (resonate::ui::Scrubber* component = world.get<resonate::ui::Scrubber>(scrub->handle))
        {
            component->value = value;
            (void)world.markChanged<resonate::ui::Scrubber>(scrub->handle);
        }
        const_cast<EditorState::Scrub*>(scrub)->shown =
            std::numeric_limits<float>::quiet_NaN();
    }
}

/* The console command: `set <key> <value>`, `get <key>` and `find <pattern>`
   over the run's CVars. The answer echoes into the console's line, and a run
   clears the field. */
void executeCommand(EditorState& state, resonate::ecs::World& world, const std::string& line)
{
    const std::size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return;
    }
    const std::size_t space = line.find(' ', first);
    const std::string command = line.substr(
        first, space == std::string::npos ? std::string::npos : space - first);
    std::string echo;
    std::string error;
    if (command == "set")
    {
        const std::size_t key_begin = line.find_first_not_of(' ', space);
        const std::size_t key_end = key_begin == std::string::npos
                                        ? std::string::npos
                                        : line.find(' ', key_begin);
        if (key_begin == std::string::npos || key_end == std::string::npos)
        {
            echo = "set <key> <value>";
        }
        else
        {
            const std::string key = line.substr(key_begin, key_end - key_begin);
            const std::size_t value_begin = line.find_first_not_of(' ', key_end);
            const std::string value =
                value_begin == std::string::npos ? "" : line.substr(value_begin);
            if (cvar_host.apply(key, value, error))
            {
                echo = key + " = " + value;
            }
            else
            {
                echo = error;
            }
        }
    }
    else if (command == "get")
    {
        const std::size_t key_begin = line.find_first_not_of(' ', space);
        if (key_begin == std::string::npos)
        {
            echo = "get <key>";
        }
        else
        {
            const std::string key = line.substr(key_begin);
            std::string value;
            echo = cvar_host.read(key, value) ? key + " = " + value : key + " is not bound";
        }
    }
    else if (command == "find")
    {
        const std::size_t pattern_begin = line.find_first_not_of(' ', space);
        const std::string pattern =
            pattern_begin == std::string::npos ? "" : line.substr(pattern_begin);
        const std::vector<std::string> keys = cvar_host.find(pattern);
        if (keys.empty())
        {
            echo = "no cvar matches";
        }
        else
        {
            for (const std::string& key : keys)
            {
                if (!echo.empty())
                {
                    echo += "  |  ";
                }
                echo += key;
            }
        }
    }
    else
    {
        echo = "unknown command '" + command + "' (set/get/find)";
    }
    if (!echo.empty())
    {
        (void)resonate::ui::setText(world, state.canvas, state.console, echo, error);
    }
    (void)resonate::ui::setText(world, state.canvas, state.command_field, "", error);
}

/* The run button latches the click; the tick executes once the event
   dispatch has finished (the echo reshapes the console's text). */
bool runCommand(void* context, resonate::ecs::World&, resonate::ecs::Entity,
                resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->command_pending = true;
    return true;
}

/* One axis of the position: the field shows it, a typed number writes it.
   A field that does not parse keeps its text and is not retried until the
   next edit. */
void applyPositionField(resonate::ecs::World& world, EditorState::AxisField& field,
                        resonate::hierarchy::LocalTransform* transform, Entity entity,
                        std::uint32_t axis)
{
    const std::string text = resonate::ui::widgetText(world, field.handle);
    if (text == field.mirrored)
    {
        return;
    }
    field.mirrored = text;
    const char* begin = text.c_str();
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    if (transform == nullptr || end == begin || *end != '\0' || !std::isfinite(value))
    {
        return;
    }
    float* axes[3] = {&transform->position.x, &transform->position.y, &transform->position.z};
    *axes[axis] = value;
    (void)world.markChanged<resonate::hierarchy::LocalTransform>(entity);
}

/* The position fields follow the selected object; an entity without a
   transform zeroes them. */
void seedPosition(EditorState& state, resonate::ecs::World& world,
                  const resonate::hierarchy::LocalTransform* transform)
{
    const float axes[3] = {transform != nullptr ? transform->position.x : 0.0F,
                           transform != nullptr ? transform->position.y : 0.0F,
                           transform != nullptr ? transform->position.z : 0.0F};
    EditorState::AxisField* fields[3] = {&state.position_x, &state.position_y,
                                         &state.position_z};
    for (std::uint32_t axis = 0; axis < 3U; ++axis)
    {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(axes[axis]));
        std::string error;
        (void)resonate::ui::setText(world, state.canvas, fields[axis]->handle, text, error);
        fields[axis]->mirrored = text;
    }
}

/* The selected scene row names the inspected object: the name field and the
   status line follow the selection, reshaped only when it changes. */
void tick(void* context, resonate::ecs::World& world, resonate::ecs::CommandBuffer&,
          float delta_seconds)
{
    auto* state = static_cast<EditorState*>(context);
    if (!world.alive(state->canvas))
    {
        return;
    }
    if (state->command_pending)
    {
        state->command_pending = false;
        executeCommand(*state, world, resonate::ui::widgetText(world, state->command_field));
    }
    /* The status bar's second and third segments: the object count and a
       smoothed frame rate, reshaped only when the shown text changes. */
    char bar_text[32] = {};
    std::snprintf(bar_text, sizeof(bar_text), "%u 个对象",
                  static_cast<unsigned>(state->box_rows.size()));
    if (state->shown_count != bar_text)
    {
        state->shown_count = bar_text;
        std::string error;
        (void)resonate::ui::setText(world, state->canvas, state->status_count, bar_text, error);
    }
    static float fps_average = 60.0F;
    if (delta_seconds > 0.0F)
    {
        fps_average = fps_average * 0.9F + (1.0F / delta_seconds) * 0.1F;
    }
    std::snprintf(bar_text, sizeof(bar_text), "%d fps", static_cast<int>(fps_average + 0.5F));
    if (state->shown_fps != bar_text)
    {
        state->shown_fps = bar_text;
        std::string error;
        (void)resonate::ui::setText(world, state->canvas, state->status_fps, bar_text, error);
    }
    /* The splitters move the panes' RectTransforms directly; the geometry is
       saved once the drag has settled. */
    {
        float scene_width = state->shown_scene_width;
        float inspector_width = state->shown_inspector_width;
        if (const resonate::ui::RectTransform* scene =
                world.get<resonate::ui::RectTransform>(state->scene_pane))
        {
            scene_width = scene->width;
        }
        if (const resonate::ui::RectTransform* inspector =
                world.get<resonate::ui::RectTransform>(state->inspector))
        {
            inspector_width = inspector->width;
        }
        if (std::fabs(scene_width - state->shown_scene_width) > 0.5F ||
            std::fabs(inspector_width - state->shown_inspector_width) > 0.5F)
        {
            state->shown_scene_width = scene_width;
            state->shown_inspector_width = inspector_width;
            state->save_delay = 0.3F;
        }
        else if (state->save_delay > 0.0F)
        {
            state->save_delay -= delta_seconds;
            if (state->save_delay <= 0.0F)
            {
                saveLayout(state->shown_scene_width, state->shown_inspector_width);
            }
        }
    }
    mirrorConsole(*state, world);
    mirrorScrub(state->canvas, world, state->opacity);
    mirrorScrub(state->canvas, world, state->roughness);
    mirrorScrub(state->canvas, world, state->metallic);
    const Entity selected = resonate::ui::selectedChild(world, state->scene_list);
    /* applyMaterials reads all three scrub values, so it waits until the
       first mirror has read them (the scrubbers answer their authored
       defaults on the first call). */
    if (!std::isnan(state->metallic.shown) && state->boxes.valid())
    {
        applyMaterials(*state, world, selected);
    }

    /* The name field edits the selected object: a box row writes the entity's
       Name and its caption follows, an authored row renames the caption only.
       A mirror echo is left alone. */
    if (selected.valid())
    {
        const std::string field_text = resonate::ui::widgetText(world, state->name_field);
        if (field_text != state->mirrored_name)
        {
            std::string error;
            const Entity entity = entityOfRow(*state, selected);
            if (entity.valid())
            {
                (void)resonate::hierarchy::setName(world, entity, field_text, error);
            }
            Entity caption;
            for (const Entity child : resonate::hierarchy::children(world, selected))
            {
                if (world.get<resonate::ui::Text>(child) != nullptr)
                {
                    caption = child;
                    break;
                }
            }
            if (caption.valid())
            {
                (void)resonate::ui::setText(world, state->canvas, caption, field_text, error);
            }
            state->mirrored_name = field_text;
            (void)resonate::ui::setText(world, state->canvas, state->status,
                                        "已选择 " + field_text, error);
        }
        /* The position fields write the selected object's LocalTransform. */
        const Entity entity = entityOfRow(*state, selected);
        resonate::hierarchy::LocalTransform* transform =
            entity.valid()
                ? world.get<resonate::hierarchy::LocalTransform>(entity)
                : nullptr;
        applyPositionField(world, state->position_x, transform, entity, 0U);
        applyPositionField(world, state->position_y, transform, entity, 1U);
        applyPositionField(world, state->position_z, transform, entity, 2U);
    }

    if (selected == state->shown_row)
    {
        return;
    }
    state->shown_row = selected;
    std::string error;
    if (selected.valid())
    {
        /* A box row shows the entity's Name; an authored row shows its
           caption. */
        std::string name;
        const Entity entity = entityOfRow(*state, selected);
        if (entity.valid())
        {
            name = resonate::hierarchy::nameOf(world, entity);
        }
        if (name.empty())
        {
            name = resonate::ui::widgetText(world, selected);
        }
        (void)resonate::ui::setText(world, state->canvas, state->name_field, name, error);
        state->mirrored_name = name;
        (void)resonate::ui::setText(world, state->canvas, state->status, "已选择 " + name, error);
        seedMaterial(*state, world, entity);
        const resonate::hierarchy::LocalTransform* transform =
            entity.valid()
                ? world.get<resonate::hierarchy::LocalTransform>(entity)
                : nullptr;
        seedPosition(*state, world, transform);
    }
    else
    {
        (void)resonate::ui::setText(world, state->canvas, state->name_field, "", error);
        state->mirrored_name.clear();
        (void)resonate::ui::setText(world, state->canvas, state->status, "未选择", error);
        seedPosition(*state, world, nullptr);
    }
}

/* Runs once the document exists: every widget the editor drives is addressed by
   its authored key. */
void ready(void* context, resonate::ecs::World& world, Entity canvas)
{
    auto* state = static_cast<EditorState*>(context);
    state->canvas = canvas;
    state->scene_list = resonate::ui::findByKey(world, canvas, "scene-list");
    state->name_field = resonate::ui::findByKey(world, canvas, "name-field");
    state->status = resonate::ui::findByKey(world, canvas, "status");
    state->opacity.handle = resonate::ui::findByKey(world, canvas, "opacity-scrub");
    state->opacity.value_label = resonate::ui::findByKey(world, canvas, "opacity-value");
    state->roughness.handle = resonate::ui::findByKey(world, canvas, "roughness-scrub");
    state->roughness.value_label = resonate::ui::findByKey(world, canvas, "roughness-value");
    state->metallic.handle = resonate::ui::findByKey(world, canvas, "metallic-scrub");
    state->metallic.value_label = resonate::ui::findByKey(world, canvas, "metallic-value");
    state->position_x.handle = resonate::ui::findByKey(world, canvas, "position-x");
    state->position_y.handle = resonate::ui::findByKey(world, canvas, "position-y");
    state->position_z.handle = resonate::ui::findByKey(world, canvas, "position-z");
    state->command_field = resonate::ui::findByKey(world, canvas, "command-field");
    const resonate::ecs::Entity run_button = resonate::ui::findByKey(world, canvas, "command-run");
    if (run_button.valid())
    {
        world.add(run_button,
                  resonate::ui::Listener{&runCommand, state,
                                         resonate::ui::eventBit(resonate::ui::EventKind::Click)});
    }
    state->console = resonate::ui::findByKey(world, canvas, "console-line");
    state->status_count = resonate::ui::findByKey(world, canvas, "status-count");
    state->scene_pane = resonate::ui::findByKey(world, canvas, "scene-pane");
    state->splitter_left = resonate::ui::findByKey(world, canvas, "splitter-left");
    state->viewport = resonate::ui::findByKey(world, canvas, "viewport");
    state->viewport_hint = resonate::ui::findByKey(world, canvas, "viewport-hint");
    state->splitter_right = resonate::ui::findByKey(world, canvas, "splitter-right");
    state->inspector = resonate::ui::findByKey(world, canvas, "inspector");
    state->console_pane = resonate::ui::findByKey(world, canvas, "console");
    {
        float scene_width = 280.0F;
        float inspector_width = 280.0F;
        loadLayout(scene_width, inspector_width);
        applyLayout(*state, world, scene_width, inspector_width);
    }
    state->status_fps = resonate::ui::findByKey(world, canvas, "status-fps");
    world.setReportSink(state, &reportSink);

    resonate::ecs::QueryDesc desc{};
    const resonate::ecs::ComponentIndex all[] = {
        resonate::ecs::ComponentTraits<resonate::render::MeshInstance>::index,
        resonate::ecs::ComponentTraits<resonate::render::Material>::index};
    desc.all = resonate::Span<const resonate::ecs::ComponentIndex>(all, 2U);
    state->boxes = world.createQuery(desc);

    /* The scene tree lists the real scene: one cloned row per box. */
    const resonate::ecs::Entity template_row =
        resonate::ui::findByKey(world, canvas, "crate-a");
    const resonate::ecs::Entity scene_list = state->scene_list;
    if (template_row.valid() && scene_list.valid())
    {
        struct ListContext
        {
            EditorState* state;
            resonate::ecs::World* world;
            resonate::ecs::Entity template_row;
            resonate::ecs::Entity list;
            std::uint32_t index = 0U;
        } list_context{state, &world, template_row, scene_list, 0U};
        state->boxes.forEachChunk(
            [](void* context, resonate::ecs::ChunkView view)
            {
                auto* list = static_cast<ListContext*>(context);
                for (std::uint32_t row = 0; row < view.count; ++row)
                {
                    ++list->index;
                    /* The row carries the entity's Name; an unnamed entity
                       falls back to its position in the list. */
                    std::string name =
                        resonate::hierarchy::nameOf(*list->world, view.entities[row]);
                    if (name.empty())
                    {
                        name = "箱子 " + std::to_string(list->index);
                    }
                    const resonate::ecs::Entity tree_row = cloneRow(
                        *list->state, *list->world, list->template_row, list->list, name);
                    list->state->box_rows.emplace_back(tree_row, view.entities[row]);
                }
            },
            &list_context);
    }
}

/* The editor runs its own configuration; a command line that names one keeps
   it. */
int runEditor(const std::vector<std::string>& arguments)
{
    std::vector<std::string> editor_arguments = arguments;
    if (std::find(editor_arguments.begin(), editor_arguments.end(), "--config") ==
        editor_arguments.end())
    {
        editor_arguments.emplace_back("--config");
        editor_arguments.emplace_back("resonate.editor.toml");
    }

    EditorState state;
    resonate::startup::HostExtension extension;
    extension.cvars = &cvar_host;
    extension.systems.push_back(resonate::EngineSystemDesc{"editor.selection",
                                                           RESONATE_STAGE_LATE_UPDATE, &tick,
                                                           &state});
    extension.ready = &ready;
    extension.ready_context = &state;
    return resonate::startup::run(editor_arguments, "Resonate Editor", extension);
}

} // namespace

#ifdef RESONATE_PLATFORM_WINDOWS
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return runEditor(resonate::startup::commandLine());
}
#else
int main(int argc, char** argv)
{
    return runEditor(resonate::startup::commandLine(argc, argv));
}
#endif
