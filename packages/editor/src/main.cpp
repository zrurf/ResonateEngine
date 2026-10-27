#include <resonate/startup/startup.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
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
    std::string mirrored_name;
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

/* The selected scene row names the inspected object: the name field and the
   status line follow the selection, reshaped only when it changes. */
void tick(void* context, resonate::ecs::World& world, resonate::ecs::CommandBuffer&, float)
{
    auto* state = static_cast<EditorState*>(context);
    if (!world.alive(state->canvas))
    {
        return;
    }
    mirrorScrub(state->canvas, world, state->opacity);
    mirrorScrub(state->canvas, world, state->roughness);
    const Entity selected = resonate::ui::selectedChild(world, state->scene_list);

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
