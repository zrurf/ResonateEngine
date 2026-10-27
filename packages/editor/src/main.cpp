#include <resonate/startup/startup.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <resonate/ecs/world.h>
#include <resonate/ui/document.h>
#include <resonate/ui/text_runtime.h>

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
};

/* The selected scene row names the inspected object: the name field and the
   status line follow the selection, reshaped only when it changes. */
void tick(void* context, resonate::ecs::World& world, resonate::ecs::CommandBuffer&, float)
{
    auto* state = static_cast<EditorState*>(context);
    if (!world.alive(state->canvas))
    {
        return;
    }
    const Entity selected = resonate::ui::selectedChild(world, state->scene_list);
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
        (void)resonate::ui::setText(world, state->canvas, state->status, "已选择 " + name, error);
    }
    else
    {
        (void)resonate::ui::setText(world, state->canvas, state->name_field, "", error);
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
