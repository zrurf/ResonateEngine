#include <resonate/startup/startup.hpp>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>
#include <string>
#include <vector>

#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include "resonate.render/components.gen.h"
#include <resonate/ui/document.h>
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

    /* The name field mirrors the selection; a user edit writes back. */
    std::string mirrored_name{};

    /* The boxes the opacity scrub dims: one query, built on ready. */
    resonate::ecs::Query boxes{};
    std::vector<std::pair<resonate::ecs::Entity, resonate::ecs::Entity>> box_rows; /* tree row -> box */

    /* The console mirrors the world's report stream: a ring of the latest
       lines, flushed into the label when a new one arrives. */
    std::deque<std::string> console_lines;
    bool console_dirty = false;
};

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
   in the viewport itself. */
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
    } context{&world, opacity, selected_box};
    state.boxes.forEachChunk(
        [](void* context, resonate::ecs::ChunkView view)
        {
            auto* state = static_cast<OpacityContext*>(context);
            resonate::ecs::Column<resonate::render::Material> material =
                resonate::ecs::column<resonate::render::Material>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                const float value =
                    state->value * (view.entities[row] == state->selected_box ? 2.0F : 1.0F);
                material.data[row].tint = resonate::Vec3{value, value, value};
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
                world.createBlob(row, size, bytes);
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

/* The selected scene row names the inspected object: the name field and the
   status line follow the selection, reshaped only when it changes. */
void tick(void* context, resonate::ecs::World& world, resonate::ecs::CommandBuffer&, float)
{
    auto* state = static_cast<EditorState*>(context);
    if (!world.alive(state->canvas))
    {
        return;
    }
    mirrorConsole(*state, world);
    mirrorScrub(state->canvas, world, state->opacity);
    mirrorScrub(state->canvas, world, state->roughness);
    const Entity selected = resonate::ui::selectedChild(world, state->scene_list);
    if (!std::isnan(state->opacity.shown) && state->boxes.valid())
    {
        applyMaterials(*state, world, selected);
    }

    /* The name field edits the selected row's caption: what the user typed
       lands on the row, while a mirror echo is left alone. */
    if (selected.valid())
    {
        const std::string field_text = resonate::ui::widgetText(world, state->name_field);
        if (field_text != state->mirrored_name)
        {
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
                std::string error;
                if (resonate::ui::setText(world, state->canvas, caption, field_text, error))
                {
                    state->mirrored_name = field_text;
                    (void)resonate::ui::setText(world, state->canvas, state->status,
                                                "已选择 " + field_text, error);
                }
            }
        }
    }

    if (selected == state->shown_row)
    {
        return;
    }
    state->shown_row = selected;
    std::string error;
    if (selected.valid())
    {
        const std::string name = resonate::ui::widgetText(world, selected);
        (void)resonate::ui::setText(world, state->canvas, state->name_field, name, error);
        state->mirrored_name = name;
        (void)resonate::ui::setText(world, state->canvas, state->status, "已选择 " + name, error);
    }
    else
    {
        (void)resonate::ui::setText(world, state->canvas, state->name_field, "", error);
        state->mirrored_name.clear();
        (void)resonate::ui::setText(world, state->canvas, state->status, "未选择", error);
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
    state->console = resonate::ui::findByKey(world, canvas, "console-line");
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
                    const std::string name = "箱子 " + std::to_string(list->index);
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
