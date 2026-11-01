#include <resonate/startup/startup.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/dom/document.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/scene/capture.h>
#include <resonate/scene/document.h>
#include <resonate/asset/identity.h>
#include <resonate/pal/dialog.h>
#include <resonate/pal/io.h>
#include <resonate/startup/cvar.h>
#include "resonate.render/components.gen.h"
#include "resonate.animation/components.gen.h"
#include "resonate.scene/components.gen.h"
#include <resonate/ui/document.h>
#include <resonate/ui/event.h>
#include <resonate/ui/string_table.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/text_runtime.h>

#include "resonate.ui/components.gen.h"

#ifdef RESONATE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

namespace
{

using resonate::ecs::Entity;

/* The editor's pane geometry, beside the executable: the splitters own the
   three sizes, the toolbar owns the three visibility flags. A missing or
   malformed file keeps the authored defaults. */
struct Layout
{
    float scene_width = 280.0F;
    float inspector_width = 280.0F;
    float console_height = 240.0F;
    bool scene_shown = true;
    bool inspector_shown = true;
    bool console_shown = true;
};

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
    bool save_pending = false;
    bool undo_pending = false;
    bool redo_pending = false;
    bool delete_pending = false;
    bool duplicate_pending = false;
    /* Where the scene was saved last: empty until the chooser picked a path,
       so the first save asks and later ones write straight to it. */
    std::string document_path{};
    bool open_pending = false;
    /* The tree's drag gesture. The host key pass records the pointer and the
       rows it moves between; the tick performs the reparent on release. */
    float pointer_x = 0.0F;
    float pointer_y = 0.0F;
    Entity row_press{};
    Entity row_hover{};
    bool row_dragging = false;
    Entity reparent_child{};
    Entity reparent_parent{};
    bool reparent_pending = false;
    /* The scene roots the run or the last open produced, and the hidden row
       every visible tree row is cloned from: both let an open replace the
       scene without losing the list's shape. */
    std::vector<Entity> scene_roots{};
    Entity row_prototype{};
    /* The inspector's animation line: the widget it writes and the manifest
       the clip ids resolve through. */
    Entity animation_value{};
    resonate::asset::Identity asset_ids;
    bool asset_ids_loaded = false;

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
    Entity splitter_console{};
    Entity inspector{};
    Entity console_pane{};
    /* The geometry the splitters and the visibility toggles own, mirrored from
       the layout file and written back when it changes. */
    Layout layout{};
    float save_delay = 0.0F;

    /* Reversible edits. Closures borrow the world through the editor state and
       run only from the tick, the same discipline as the command latch. */
    struct UndoEntry
    {
        std::string name;
        std::function<void(resonate::ecs::World&)> undo;
        std::function<void(resonate::ecs::World&)> redo;
    };
    std::vector<UndoEntry> undo_stack;
    std::vector<UndoEntry> redo_stack;

    /* The material scrub's undo coalescing: one drag is one entry, so the
       value before the press is snapshotted and pushed on release. */
    struct ScrubUndo
    {
        bool dragging = false;
        float before = 0.0F;
    };
    ScrubUndo roughness_undo{};
    ScrubUndo metallic_undo{};
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
    /* The layout writes into an existing ComputedRect; a cloned widget brings
       its own, as the adapter's instantiation does. */
    world.add(row, resonate::ui::ComputedRect{});
    resonate::hierarchy::setParent(commands, row, list);

    /* The row's own widgets copy with it: the glyph keeps its image and tint,
       the caption reshapes to `name` through the canvas runtime. */
    for (const resonate::ecs::Entity child : resonate::hierarchy::children(world, template_row))
    {
        const resonate::ui::Text* text = world.get<resonate::ui::Text>(child);
        const resonate::ecs::Entity copy = world.create();
        if (text != nullptr)
        {
            resonate::ui::Text shaped = *text;
            shaped.content = {};
            world.add(copy, shaped);
        }
        if (const auto* image = world.get<resonate::ui::Image>(child))
        {
            world.add(copy, *image);
        }
        world.add(copy, resonate::ui::ComputedRect{});
        if (const auto* transform = world.get<resonate::ui::RectTransform>(child))
        {
            world.add(copy, *transform);
        }
        if (const auto* styles = world.get<resonate::ui::StyleRef>(child))
        {
            world.add(copy, *styles);
        }
        if (text != nullptr)
        {
            std::string error;
            (void)resonate::ui::setText(world, state.canvas, copy, name, error);
        }
        resonate::hierarchy::setParent(commands, copy, row);
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

/* A user-facing string from the run's table: the key itself when the table or
   the entry is missing, so a stale key shows up instead of silence. */
std::string localized(std::string_view key)
{
    const resonate::ui::StringTable* table = resonate::startup::stringTable();
    const std::string* found = table != nullptr ? table->find(key) : nullptr;
    return found != nullptr ? *found : std::string(key);
}

/* One reversible edit per push; a push clears the redo stack, and the stacks
   cap so a long session cannot grow without bound. Executed only from the
   tick: the closures read the world through the editor state. */
constexpr std::size_t kUndoDepth = 64U;

void pushUndo(EditorState& state, std::string name, std::function<void(resonate::ecs::World&)> undo,
              std::function<void(resonate::ecs::World&)> redo)
{
    state.undo_stack.push_back(
        EditorState::UndoEntry{std::move(name), std::move(undo), std::move(redo)});
    state.redo_stack.clear();
    while (state.undo_stack.size() > kUndoDepth)
    {
        state.undo_stack.erase(state.undo_stack.begin());
    }
}

void runUndo(EditorState& state, resonate::ecs::World& world)
{
    if (state.undo_stack.empty())
    {
        return;
    }
    EditorState::UndoEntry entry = std::move(state.undo_stack.back());
    state.undo_stack.pop_back();
    entry.undo(world);
    state.redo_stack.push_back(std::move(entry));
    std::string status_error;
    (void)resonate::ui::setText(world, state.canvas, state.status,
                                localized("editor.undo") + " " + state.redo_stack.back().name, status_error);
}

void runRedo(EditorState& state, resonate::ecs::World& world)
{
    if (state.redo_stack.empty())
    {
        return;
    }
    EditorState::UndoEntry entry = std::move(state.redo_stack.back());
    state.redo_stack.pop_back();
    entry.redo(world);
    state.undo_stack.push_back(std::move(entry));
    std::string status_error;
    (void)resonate::ui::setText(world, state.canvas, state.status,
                                localized("editor.redo") + " " + state.undo_stack.back().name, status_error);
}

/* Deletes an entity with everything under it: the hierarchy's own destroy
   command detaches the children first, so a post-order walk keeps the subtree
   together. */
void destroySubtree(resonate::ecs::World& world, Entity entity)
{
    if (!world.alive(entity))
    {
        return;
    }
    std::vector<Entity> order;
    std::vector<Entity> stack{entity};
    while (!stack.empty())
    {
        const Entity current = stack.back();
        stack.pop_back();
        order.push_back(current);
        for (const Entity child : resonate::hierarchy::children(world, current))
        {
            stack.push_back(child);
        }
    }
    resonate::ecs::CommandBuffer commands(world);
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        resonate::hierarchy::destroyEntity(commands, *it);
    }
    world.play(commands);
}

/* One row selected, every other cleared: the scene list keeps exactly one
   choice whether the click or the undo made it. */
void selectRow(resonate::ecs::World& world, Entity container, Entity row)
{
    for (const Entity child : resonate::hierarchy::children(world, container))
    {
        auto* selectable = world.get<resonate::ui::Selectable>(child);
        if (selectable == nullptr)
        {
            continue;
        }
        const bool selected = child == row;
        if (selectable->selected != selected)
        {
            selectable->selected = selected;
            (void)world.markChanged<resonate::ui::Selectable>(child);
        }
    }
}

void rebuildSceneList(EditorState& state, resonate::ecs::World& world);

/* The row showing `entity`, or an invalid one when the scene no longer has it
   (a rebuild replaces every row). */
Entity rowOf(const EditorState& state, Entity entity)
{
    for (const auto& [row, object] : state.box_rows)
    {
        if (object == entity)
        {
            return row;
        }
    }
    return Entity{};
}

/* Points the inspector at `entity`: what an undo or a duplicate leaves
   selected. */
void selectEntity(EditorState& state, resonate::ecs::World& world, Entity entity)
{
    selectRow(world, state.scene_list, rowOf(state, entity));
}

/* The scene row a hit belongs to: the pointer lands on the row's caption, and
   the row is the child of the list the caption hangs from. */
Entity rowUnderList(resonate::ecs::World& world, Entity list, Entity hit)
{
    Entity current = hit;
    for (std::uint32_t depth = 0U; current.valid() && depth <= resonate::hierarchy::kMaxDepth;
         ++depth)
    {
        const Entity parent = resonate::hierarchy::parentOf(world, current);
        if (parent != list)
        {
            current = parent;
            continue;
        }
        return current;
    }
    return Entity{};
}

/* Delete removes the selected object from the scene and from the tree list.
   The object is captured as a document first, so undo reinstantiates it under
   the parent it had. The list rebuilds around the change. */
void deleteSelected(EditorState& state, resonate::ecs::World& world)
{
    const Entity row = resonate::ui::selectedChild(world, state.scene_list);
    if (!row.valid())
    {
        return;
    }
    const Entity entity = entityOfRow(state, row);
    if (!entity.valid() || !world.alive(entity))
    {
        return;
    }
    std::string xml;
    std::string error;
    if (!resonate::scene::capture(world, entity, xml, error))
    {
        std::fprintf(stderr, "editor: delete: %s\n", error.c_str());
        return;
    }
    const Entity parent = resonate::hierarchy::parentOf(world, entity);
    const std::string name = resonate::hierarchy::nameOf(world, entity);
    destroySubtree(world, entity);
    /* Rows are derived from the hierarchy, so the subtree's rows went with it. */
    rebuildSceneList(state, world);
    {
        std::string status_error;
        (void)resonate::ui::setText(world, state.canvas, state.status,
                                    localized("editor.deleted") + " " + name, status_error);
    }

    /* Both closures need the object the other side created: an undo makes a
       new entity, and the redo has to delete that one. */
    auto current = std::make_shared<Entity>();
    pushUndo(
        state, localized("editor.delete") + " " + name,
        [&state, parent, xml, current](resonate::ecs::World& target)
        {
            std::vector<resonate::dom::Diagnostic> diagnostics;
            auto document = resonate::dom::Document::parse(
                xml, &resonate::startup::contentSchema(), diagnostics);
            const Entity anchor = target.alive(parent) ? parent : Entity{};
            Entity restored{};
            std::string restore_error;
            if (document == nullptr ||
                !resonate::scene::instantiateUnder(target, *document,
                                                   &resonate::startup::contentComponents(), anchor,
                                                   restored, restore_error))
            {
                std::fprintf(stderr, "editor: undo delete: %s\n", restore_error.c_str());
                return;
            }
            *current = restored;
            rebuildSceneList(state, target);
            selectEntity(state, target, restored);
        },
        [&state, current](resonate::ecs::World& target)
        {
            if (target.alive(*current))
            {
                destroySubtree(target, *current);
            }
            rebuildSceneList(state, target);
        });
}

/* Duplicate copies the selected object through the same capture the delete
   uses and instantiates it beside its original. The copy drops the authored
   key -- a stable id names one object -- and takes a copy-suffixed name; the
   list rebuilds and selects it. */
void duplicateSelected(EditorState& state, resonate::ecs::World& world)
{
    const Entity row = resonate::ui::selectedChild(world, state.scene_list);
    if (!row.valid())
    {
        return;
    }
    const Entity entity = entityOfRow(state, row);
    if (!entity.valid() || !world.alive(entity))
    {
        return;
    }
    std::string xml;
    std::string error;
    if (!resonate::scene::capture(world, entity, xml, error))
    {
        std::fprintf(stderr, "editor: duplicate: %s\n", error.c_str());
        return;
    }
    const Entity parent = resonate::hierarchy::parentOf(world, entity);
    const std::string source_name = resonate::hierarchy::nameOf(world, entity);
    const std::string suffix = localized("editor.copy_suffix");
    const std::string name = source_name.empty() ? suffix : source_name + " " + suffix;

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(xml, &resonate::startup::contentSchema(), diagnostics);
    Entity copy{};
    std::string copy_error;
    if (document == nullptr ||
        !resonate::scene::instantiateUnder(world, *document,
                                           &resonate::startup::contentComponents(),
                                           world.alive(parent) ? parent : Entity{}, copy,
                                           copy_error))
    {
        std::fprintf(stderr, "editor: duplicate: %s\n", copy_error.c_str());
        return;
    }
    (void)world.remove<resonate::scene::SourceKey>(copy);
    (void)world.remove<resonate::ui::WidgetKey>(copy);
    std::string name_error;
    (void)resonate::hierarchy::setName(world, copy, name, name_error);

    rebuildSceneList(state, world);
    selectEntity(state, world, copy);
    {
        std::string status_error;
        (void)resonate::ui::setText(world, state.canvas, state.status,
                                    localized("editor.copied") + " " + name, status_error);
    }

    /* The redo reinstantiates the copy as it stands now (its own name, no
       key), so the entry stores a capture of the copy itself. */
    std::string copy_xml;
    if (!resonate::scene::capture(world, copy, copy_xml, error))
    {
        copy_xml = xml;
    }
    auto current = std::make_shared<Entity>(copy);
    pushUndo(
        state, localized("editor.duplicate") + " " + name,
        [&state, current](resonate::ecs::World& target)
        {
            if (target.alive(*current))
            {
                destroySubtree(target, *current);
            }
            rebuildSceneList(state, target);
        },
        [&state, current, parent, copy_xml](resonate::ecs::World& target)
        {
            std::vector<resonate::dom::Diagnostic> redo_diagnostics;
            auto redo_document = resonate::dom::Document::parse(
                copy_xml, &resonate::startup::contentSchema(), redo_diagnostics);
            const Entity anchor = target.alive(parent) ? parent : Entity{};
            Entity restored{};
            std::string redo_error;
            if (redo_document == nullptr ||
                !resonate::scene::instantiateUnder(target, *redo_document,
                                                   &resonate::startup::contentComponents(),
                                                   anchor, restored, redo_error))
            {
                std::fprintf(stderr, "editor: redo duplicate: %s\n", redo_error.c_str());
                return;
            }
            *current = restored;
            rebuildSceneList(state, target);
            selectEntity(state, target, restored);
        });
}

/* A drop moves `entity` under `new_parent`: nesting is the scene format's
   parenting, so the gesture is one hierarchy command, and the undo entry
   records the parent it came from. */
void reparentObject(EditorState& state, resonate::ecs::World& world, Entity entity,
                    Entity new_parent)
{
    if (!world.alive(entity) || !world.alive(new_parent) || entity == new_parent)
    {
        return;
    }
    /* A drop inside the moved subtree would fold the tree; ignore it rather
       than let the command's refusal be the only answer. */
    if (resonate::hierarchy::isAncestorOf(world, entity, new_parent))
    {
        return;
    }
    const Entity old_parent = resonate::hierarchy::parentOf(world, entity);
    if (old_parent == new_parent)
    {
        return;
    }
    const std::string name = resonate::hierarchy::nameOf(world, entity);
    {
        resonate::ecs::CommandBuffer commands(world);
        resonate::hierarchy::setParent(commands, entity, new_parent);
        world.play(commands);
    }
    rebuildSceneList(state, world);
    selectEntity(state, world, entity);
    const auto move = [&state, entity](resonate::ecs::World& target, Entity parent)
    {
        if (!target.alive(entity) || !parent.valid() || !target.alive(parent))
        {
            return;
        }
        resonate::ecs::CommandBuffer commands(target);
        resonate::hierarchy::setParent(commands, entity, parent);
        target.play(commands);
        rebuildSceneList(state, target);
        selectEntity(state, target, entity);
    };
    pushUndo(
        state, localized("editor.move") + " " + name,
        [&state, entity, old_parent, move](resonate::ecs::World& target)
        { move(target, old_parent); },
        [&state, entity, new_parent, move](resonate::ecs::World& target)
        { move(target, new_parent); });
}

/* Every object under the scene roots, parents before children, with its depth:
   the tree's row order. */
void collectSceneTree(resonate::ecs::World& world, const std::vector<Entity>& roots,
                      std::vector<std::pair<Entity, std::uint32_t>>& out)
{
    std::vector<std::pair<Entity, std::uint32_t>> stack;
    for (std::size_t index = roots.size(); index > 0U; --index)
    {
        stack.emplace_back(roots[index - 1U], 0U);
    }
    while (!stack.empty())
    {
        const std::pair<Entity, std::uint32_t> entry = stack.back();
        stack.pop_back();
        if (!world.alive(entry.first))
        {
            continue;
        }
        out.push_back(entry);
        const auto children = resonate::hierarchy::children(world, entry.first);
        for (std::size_t index = children.size(); index > 0U; --index)
        {
            stack.emplace_back(children[index - 1U], entry.second + 1U);
        }
    }
}

/* Rebuilds the scene tree: one row per object, its depth as a left margin, all
   cloned from the hidden prototype. Rows are derived state, so every edit that
   changes the hierarchy calls this rather than patching rows. */
void rebuildSceneList(EditorState& state, resonate::ecs::World& world)
{
    if (!state.row_prototype.valid() || !world.alive(state.row_prototype))
    {
        return;
    }
    for (const auto& [row, object] : state.box_rows)
    {
        (void)object;
        if (world.alive(row))
        {
            destroySubtree(world, row);
        }
    }
    state.box_rows.clear();

    std::vector<Entity> roots = state.scene_roots;
    if (roots.empty())
    {
        /* A scene nobody declared: the root each mesh object leads to. */
        std::vector<Entity> boxes;
        state.boxes.forEachChunk(
            [](void* context, resonate::ecs::ChunkView view)
            {
                auto* collected = static_cast<std::vector<Entity>*>(context);
                for (std::uint32_t row = 0U; row < view.count; ++row)
                {
                    collected->push_back(view.entities[row]);
                }
            },
            &boxes);
        for (const Entity box : boxes)
        {
            const Entity root = resonate::scene::rootOf(world, box);
            if (root.valid() && std::find(roots.begin(), roots.end(), root) == roots.end())
            {
                roots.push_back(root);
            }
        }
    }

    std::vector<std::pair<Entity, std::uint32_t>> objects;
    collectSceneTree(world, roots, objects);
    for (std::size_t index = 0U; index < objects.size(); ++index)
    {
        const Entity object = objects[index].first;
        const std::uint32_t depth = objects[index].second;
        std::string name = resonate::hierarchy::nameOf(world, object);
        if (name.empty())
        {
            name = localized("editor.object_fallback") + " " + std::to_string(index + 1U);
        }
        const Entity row = cloneRow(state, world, state.row_prototype, state.scene_list, name);
        if (depth > 0U)
        {
            resonate::ui::LayoutItem item;
            item.margin_left = 12.0F * static_cast<float>(depth);
            (void)world.add(row, item);
        }
        state.box_rows.emplace_back(row, object);
    }
}

/* Replaces the scene with the document at `path`. The new one parses and
   instantiates first, so a broken file leaves the running scene alone. */
bool loadSceneDocument(EditorState& state, resonate::ecs::World& world, const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        std::fprintf(stderr, "editor: %s: cannot read the scene\n", path.c_str());
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::startup::contentSchema(), diagnostics);
    Entity root{};
    std::string error;
    std::vector<Entity> roots;
    if (document == nullptr ||
        !resonate::scene::instantiate(world, *document, &resonate::startup::contentComponents(),
                                      root, error, &roots))
    {
        std::fprintf(stderr, "editor: %s: %s\n", path.c_str(),
                     error.empty() ? "the document did not parse" : error.c_str());
        return false;
    }
    if (roots.empty())
    {
        world.report(resonate::ecs::World::Report::Error,
                     "editor: the document has no scene objects");
        return false;
    }
    for (const Entity each : state.scene_roots)
    {
        destroySubtree(world, each);
    }
    state.scene_roots = roots;
    state.document_path = path;
    state.shown_row = Entity{};
    /* The history belongs to the document that produced it: its closures name
       entities this load just destroyed. */
    state.undo_stack.clear();
    state.redo_stack.clear();
    rebuildSceneList(state, world);
    return true;
}


/* The document the demo was loaded from: beside the executable in a shipped
   layout, in the source tree during development; the walk covers both. This is
   what the save chooser opens at, and what a platform without a chooser falls
   back to. */
/* The products manifest the run's asset indexer wrote; a shipped or staged
   layout may name it through RESONATE_PRODUCTS instead. */
std::string productsPath()
{
    const char* staged = std::getenv("RESONATE_PRODUCTS");
    if (staged != nullptr && *staged != '\0')
    {
        return staged;
    }
    std::filesystem::path directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 6; ++level)
    {
        const std::filesystem::path candidate =
            directory / "build" / "content" / "products.toml";
        std::error_code probe;
        if (std::filesystem::is_regular_file(candidate, probe))
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

/* What the inspector's animation line says for `entity`: the clip's name from
   the manifest, the machine's first authored state, or the no-reference dash.
   An id the manifest does not know stays visible as the guid. */
std::string animationLabel(EditorState& state, resonate::ecs::World& world, Entity entity)
{
    const auto blobText = [&world](resonate::ecs::BlobHandle handle) -> std::string_view
    {
        if (!handle.valid() || !world.alive(handle))
        {
            return {};
        }
        const auto* bytes = static_cast<const char*>(world.blobData(handle));
        const std::size_t size = world.blobSize(handle);
        if (bytes == nullptr || size == 0U)
        {
            return {};
        }
        return std::string_view(bytes, size);
    };
    if (const auto* animation = world.get<resonate::animation::Animation>(entity);
        animation != nullptr &&
        (animation->clip_low != 0U || animation->clip_high != 0U))
    {
        const resonate::asset::AssetId id{animation->clip_low, animation->clip_high, 1U};
        if (state.asset_ids_loaded)
        {
            if (const auto path = state.asset_ids.locate(id))
            {
                return std::filesystem::path(*path).stem().string();
            }
        }
        return "@" + resonate::asset::formatGuid(id);
    }
    if (world.get<resonate::animation::MachineState>(entity) != nullptr)
    {
        if (const auto* source = world.get<resonate::scene::MachineSource>(entity);
            source != nullptr)
        {
            const std::string_view text = blobText(source->text);
            constexpr std::string_view kMarker = "<State name=\"";
            if (const std::size_t at = text.find(kMarker); at != std::string_view::npos)
            {
                const std::size_t begin = at + kMarker.size();
                const std::size_t end = text.find('"', begin);
                if (end != std::string_view::npos)
                {
                    return std::string(text.substr(begin, end - begin));
                }
            }
        }
        return localized("editor.animation");
    }
    return localized("editor.none");
}

std::string defaultDocumentPath()
{
    std::filesystem::path directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 6; ++level)
    {
        const std::filesystem::path candidate = directory / "content" / "scene" / "demo.rscene";
        std::error_code probe;
        if (std::filesystem::is_regular_file(candidate, probe))
        {
            return candidate.string();
        }
        if (!directory.has_parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    return (resonate_pal_io_executable_directory() / std::filesystem::path("demo.rscene"))
        .string();
}

std::string layoutPath()
{
    const char* directory = resonate_pal_io_executable_directory();
    return std::string(directory != nullptr ? directory : ".") + "/resonate.editor.layout";
}

Layout loadLayout()
{
    Layout layout;
    std::FILE* file = std::fopen(layoutPath().c_str(), "rb");
    if (file == nullptr)
    {
        return layout;
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
                layout.scene_width = value;
            }
            else if (name == "inspector-width" && value > 0.0F)
            {
                layout.inspector_width = value;
            }
            else if (name == "console-height" && value > 0.0F)
            {
                layout.console_height = value;
            }
            else if (name == "scene-shown")
            {
                layout.scene_shown = value != 0.0F;
            }
            else if (name == "inspector-shown")
            {
                layout.inspector_shown = value != 0.0F;
            }
            else if (name == "console-shown")
            {
                layout.console_shown = value != 0.0F;
            }
        }
    }
    std::fclose(file);
    return layout;
}

void saveLayout(const Layout& layout)
{
    std::FILE* file = std::fopen(layoutPath().c_str(), "wb");
    if (file == nullptr)
    {
        return;
    }
    std::fprintf(file, "scene-width=%.0f\n", static_cast<double>(layout.scene_width));
    std::fprintf(file, "inspector-width=%.0f\n", static_cast<double>(layout.inspector_width));
    std::fprintf(file, "console-height=%.0f\n", static_cast<double>(layout.console_height));
    std::fprintf(file, "scene-shown=%d\n", layout.scene_shown ? 1 : 0);
    std::fprintf(file, "inspector-shown=%d\n", layout.inspector_shown ? 1 : 0);
    std::fprintf(file, "console-shown=%d\n", layout.console_shown ? 1 : 0);
    std::fclose(file);
}

/* Places the panes around the geometry the splitters own: the scene pane on
   the left, the inspector on the right, the console at the bottom above the
   status bar, and the viewport filling what is left. A hidden pane collapses
   to nothing and takes its divider with it. Every pane is written whole, so a
   divider's own drag damage is corrected on the same tick. */
void applyLayout(const EditorState& state, resonate::ecs::World& world, const Layout& layout)
{
    const float scene_width = layout.scene_shown ? layout.scene_width : 0.0F;
    const float inspector_width = layout.inspector_shown ? layout.inspector_width : 0.0F;
    const float console_height = layout.console_shown ? layout.console_height : 0.0F;
    const auto place = [&](Entity entity, float x, float width)
    {
        if (resonate::ui::RectTransform* rect = world.get<resonate::ui::RectTransform>(entity))
        {
            rect->x = x;
            rect->width = width;
            (void)world.markChanged<resonate::ui::RectTransform>(entity);
        }
    };
    /* A pane's y is measured from the top when its anchor starts at 0, and
       from the bottom (negative) when it ends at 1. */
    const auto placeVertical = [&](Entity entity, bool from_bottom, float y, float height)
    {
        if (resonate::ui::RectTransform* rect = world.get<resonate::ui::RectTransform>(entity))
        {
            const float value = from_bottom ? -y : y;
            if (rect->y == value && rect->height == height)
            {
                return;
            }
            rect->y = value;
            rect->height = height;
            (void)world.markChanged<resonate::ui::RectTransform>(entity);
        }
    };

    place(state.scene_pane, 0.0F, scene_width);
    place(state.splitter_left, scene_width, layout.scene_shown ? 6.0F : 0.0F);
    const float middle_width = -(scene_width + inspector_width + 12.0F);
    place(state.viewport, scene_width + 6.0F, middle_width);
    place(state.console_pane, scene_width + 6.0F, middle_width);
    place(state.splitter_console, scene_width + 6.0F, middle_width);
    place(state.splitter_right, -(inspector_width + 6.0F), layout.inspector_shown ? 6.0F : 0.0F);
    place(state.inspector, -inspector_width, inspector_width);
    if (resonate::ui::RectTransform* hint =
            world.get<resonate::ui::RectTransform>(state.viewport_hint))
    {
        hint->x = scene_width + 22.0F;
        (void)world.markChanged<resonate::ui::RectTransform>(state.viewport_hint);
    }

    /* Between the toolbar (37) and the status bar (26): the console takes its
       height off the bottom, its divider sits on top of it, and the viewport
       keeps the rest. */
    constexpr float kToolbarHeight = 37.0F;
    constexpr float kStatusHeight = 26.0F;
    const float divider_height = layout.console_shown ? 6.0F : 0.0F;
    placeVertical(state.console_pane, true, console_height + kStatusHeight, console_height);
    placeVertical(state.splitter_console, true,
                  console_height + kStatusHeight + divider_height, divider_height);
    placeVertical(state.viewport, false, kToolbarHeight,
                  -(kToolbarHeight + console_height + kStatusHeight + divider_height));
    placeVertical(state.scene_pane, false, kToolbarHeight, -(kToolbarHeight + kStatusHeight));
    placeVertical(state.splitter_left, false, kToolbarHeight, -(kToolbarHeight + kStatusHeight));
    placeVertical(state.splitter_right, false, kToolbarHeight, -(kToolbarHeight + kStatusHeight));
    placeVertical(state.inspector, false, kToolbarHeight, -(kToolbarHeight + kStatusHeight));

    resonate::ui::setShown(world, state.scene_pane, layout.scene_shown);
    resonate::ui::setShown(world, state.splitter_left, layout.scene_shown);
    resonate::ui::setShown(world, state.inspector, layout.inspector_shown);
    resonate::ui::setShown(world, state.splitter_right, layout.inspector_shown);
    resonate::ui::setShown(world, state.console_pane, layout.console_shown);
    resonate::ui::setShown(world, state.splitter_console, layout.console_shown);
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

/* The scene save: capture the subtree under the demo scene's root and write it
   back to the document it came from, found by the same walk the start-up used.
   A dev-loop editor writes the source tree; a shipped one would stage. */
bool saveScene(void* context, resonate::ecs::World& world, resonate::ecs::Entity,
               resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->save_pending = true;
    return true;
}

bool openScene(void* context, resonate::ecs::World&, resonate::ecs::Entity,
               resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->open_pending = true;
    return true;
}

bool undoEdit(void* context, resonate::ecs::World&, resonate::ecs::Entity,
              resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->undo_pending = true;
    return true;
}

bool redoEdit(void* context, resonate::ecs::World&, resonate::ecs::Entity,
              resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->redo_pending = true;
    return true;
}

bool duplicateEdit(void* context, resonate::ecs::World&, resonate::ecs::Entity,
                   resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->duplicate_pending = true;
    return true;
}

bool deleteEdit(void* context, resonate::ecs::World&, resonate::ecs::Entity,
                resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    static_cast<EditorState*>(context)->delete_pending = true;
    return true;
}

/* The run hands over the roots its scene document produced, before ready
   addresses the scene: a save or an open replaces the whole list at once. */
void sceneRootsLoaded(void* context, resonate::ecs::World&, const std::vector<Entity>& roots)
{
    static_cast<EditorState*>(context)->scene_roots = roots;
}

/* The editor's keyboard pass: Ctrl+S saves, Ctrl+Z and Ctrl+Y run the same
   undo and redo the toolbar buttons do, unless a text field owns the keys (a
   field's own editing is not the scene's). */
bool editorKeys(void* context, resonate::ecs::World& world, const ResonateInput* input)
{
    auto* state = static_cast<EditorState*>(context);
    if (input == nullptr)
    {
        return false;
    }
    /* The pointer's sample and the tree's drag gesture run every frame: rows
       are not text fields, so focus does not gate them. The release only arms
       a latch -- structural changes wait for the tick. */
    {
        ResonateVec2 position{};
        input->pointer_position(input->self, &position);
        state->pointer_x = position.x;
        state->pointer_y = position.y;
        const auto* pointer = world.get<resonate::ui::PointerState>(state->canvas);
        if (pointer != nullptr)
        {
            if (pointer->down && !state->row_press.valid())
            {
                if (rowOf(*state, pointer->press).valid())
                {
                    state->row_press = pointer->press;
                }
            }
            else if (pointer->down && state->row_press.valid())
            {
                const Entity hit =
                    resonate::ui::hitTest(world, state->canvas, state->pointer_x,
                                          state->pointer_y);
                const Entity hover = rowUnderList(world, state->scene_list, hit);
                if (hover.valid() && hover != state->row_press)
                {
                    state->row_dragging = true;
                    state->row_hover = hover;
                }
            }
            else if (!pointer->down && state->row_press.valid())
            {
                if (state->row_dragging && state->row_hover.valid())
                {
                    state->reparent_child = entityOfRow(*state, state->row_press);
                    state->reparent_parent = entityOfRow(*state, state->row_hover);
                    state->reparent_pending = state->reparent_child.valid() &&
                                              state->reparent_parent.valid();
                }
                state->row_press = {};
                state->row_hover = {};
                state->row_dragging = false;
            }
        }
    }
    if (resonate::ui::focusedTextEditable(world, state->canvas))
    {
        return false;
    }
    if (input->key_pressed(input->self, RESONATE_KEY_DELETE) != 0)
    {
        state->delete_pending = true;
        return true;
    }
    const bool ctrl = input->key_down(input->self, RESONATE_KEY_LCTRL) != 0 ||
                      input->key_down(input->self, RESONATE_KEY_RCTRL) != 0;
    if (!ctrl)
    {
        return false;
    }
    if (input->key_pressed(input->self, RESONATE_KEY_S) != 0)
    {
        state->save_pending = true;
        return true;
    }
    if (input->key_pressed(input->self, RESONATE_KEY_D) != 0)
    {
        state->duplicate_pending = true;
        return true;
    }
    if (input->key_pressed(input->self, RESONATE_KEY_O) != 0)
    {
        state->open_pending = true;
        return true;
    }
    if (input->key_pressed(input->self, RESONATE_KEY_Z) != 0)
    {
        state->undo_pending = true;
        return true;
    }
    if (input->key_pressed(input->self, RESONATE_KEY_Y) != 0)
    {
        state->redo_pending = true;
        return true;
    }
    return false;
}

/* A pane toggle flips its flag, re-derives the geometry and records the result
   at once: a click is deliberate, unlike a drag that settles first. */
void togglePane(EditorState& state, resonate::ecs::World& world, bool Layout::*flag)
{
    state.layout.*flag = !(state.layout.*flag);
    applyLayout(state, world, state.layout);
    saveLayout(state.layout);
}

bool toggleScene(void* context, resonate::ecs::World& world, resonate::ecs::Entity,
                 resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    togglePane(*static_cast<EditorState*>(context), world, &Layout::scene_shown);
    return true;
}

bool toggleInspector(void* context, resonate::ecs::World& world, resonate::ecs::Entity,
                     resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    togglePane(*static_cast<EditorState*>(context), world, &Layout::inspector_shown);
    return true;
}

bool toggleConsole(void* context, resonate::ecs::World& world, resonate::ecs::Entity,
                   resonate::ui::EventPhase phase, resonate::ui::Event&)
{
    if (phase != resonate::ui::EventPhase::Target)
    {
        return false;
    }
    togglePane(*static_cast<EditorState*>(context), world, &Layout::console_shown);
    return true;
}

/* One axis of the position: the field shows it, a typed number writes it.
   A field that does not parse keeps its text and is not retried until the
   next edit. Each accepted write pushes one undo entry whose closures also
   refresh the field, so an undo does not fight the text still standing in it. */
void applyPositionField(resonate::ecs::World& world, EditorState& state,
                        EditorState::AxisField& field,
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
    const float previous = *axes[axis];
    if (previous == value)
    {
        return;
    }
    *axes[axis] = value;
    (void)world.markChanged<resonate::hierarchy::LocalTransform>(entity);

    /* The closures re-resolve the transform at execution time: a component
       pointer would dangle across the structural changes in between. */
    const auto apply = [&state, &field, entity,
                        axis](float amount, resonate::ecs::World& world_ref)
    {
        if (auto* target = world_ref.get<resonate::hierarchy::LocalTransform>(entity);
            target != nullptr)
        {
            float* target_axes[3] = {&target->position.x, &target->position.y,
                                     &target->position.z};
            *target_axes[axis] = amount;
            (void)world_ref.markChanged<resonate::hierarchy::LocalTransform>(entity);
        }
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(amount));
        std::string field_error;
        (void)resonate::ui::setText(world_ref, state.canvas, field.handle, text, field_error);
        field.mirrored = text;
    };
    static const char* const kAxisKeys[3] = {"editor.axis.x", "editor.axis.y", "editor.axis.z"};
    pushUndo(state, localized(kAxisKeys[axis]),
             [apply, previous](resonate::ecs::World& world_ref)
             { apply(previous, world_ref); },
             [apply, value](resonate::ecs::World& world_ref) { apply(value, world_ref); });
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
    if (state->undo_pending)
    {
        state->undo_pending = false;
        runUndo(*state, world);
    }
    if (state->redo_pending)
    {
        state->redo_pending = false;
        runRedo(*state, world);
    }
    if (state->delete_pending)
    {
        state->delete_pending = false;
        deleteSelected(*state, world);
    }
    if (state->duplicate_pending)
    {
        state->duplicate_pending = false;
        duplicateSelected(*state, world);
    }
    if (state->reparent_pending)
    {
        state->reparent_pending = false;
        reparentObject(*state, world, state->reparent_child, state->reparent_parent);
        state->reparent_child = {};
        state->reparent_parent = {};
    }
    if (state->open_pending)
    {
        state->open_pending = false;
        char chosen[1024] = {};
        int32_t selected = 0;
        const ResonatePalStatus status = resonate_pal_open_dialog(
            "Open Scene", "rscene", chosen, static_cast<std::uint32_t>(sizeof(chosen)),
            &selected);
        if (status == RESONATE_PAL_OK && selected != 0)
        {
            if (loadSceneDocument(*state, world, chosen))
            {
                std::string status_error;
                (void)resonate::ui::setText(world, state->canvas, state->status,
                                            localized("editor.opened"), status_error);
            }
        }
        else if (status != RESONATE_PAL_OK)
        {
            world.report(resonate::ecs::World::Report::Warning,
                         "editor: no file chooser on this platform");
        }
    }
    if (state->save_pending)
    {
        state->save_pending = false;
        /* Any scene object reaches the root: the capture walks the subtree of
           the demo document, and the procedural light grid (outside it) stays
           procedural. */
        Entity any_object{};
        for (const auto& [row, box] : state->box_rows)
        {
            (void)row;
            if (box.valid() && world.alive(box))
            {
                any_object = box;
                break;
            }
        }
        std::string xml;
        std::string error;
        /* The roots the run loaded or the last open installed; a scene the
           editor never learned about falls back to the one the first object
           leads to. */
        std::vector<Entity> roots = state->scene_roots;
        if (roots.empty() && any_object.valid())
        {
            roots.push_back(resonate::scene::rootOf(world, any_object));
        }
        if (!roots.empty() &&
            resonate::scene::capture(world,
                                     resonate::Span<const Entity>(roots.data(), roots.size()),
                                     xml, error))
        {
            /* A desktop editor asks the platform where the document goes; the
               path it answers is remembered, so only the first save asks. A
               platform without a chooser falls back to where the demo lives. */
            if (state->document_path.empty())
            {
                const std::string offered = defaultDocumentPath();
                char chosen[1024] = {};
                int32_t selected = 0;
                const ResonatePalStatus status =
                    resonate_pal_save_dialog("Save Scene", offered.c_str(), "rscene", chosen,
                                             static_cast<std::uint32_t>(sizeof(chosen)),
                                             &selected);
                if (status == RESONATE_PAL_OK && selected != 0)
                {
                    state->document_path = chosen;
                }
                else if (status != RESONATE_PAL_OK)
                {
                    state->document_path = offered;
                }
                /* A cancel leaves the path unset: the next save asks again. */
            }
            const std::string target = state->document_path;
            if (!target.empty())
            {
                std::ofstream out(target, std::ios::binary);
                if (out.is_open())
                {
                    out << xml;
                    std::string status_error;
                    (void)resonate::ui::setText(world, state->canvas, state->status,
                                                localized("editor.saved"), status_error);
                }
                else
                {
                    std::fprintf(stderr, "editor: %s: cannot write the scene\n",
                                 target.c_str());
                }
            }
        }
        else
        {
            std::fprintf(stderr, "editor: scene capture failed: %s\n", error.c_str());
        }
    }
    if (state->command_pending)
    {
        state->command_pending = false;
        executeCommand(*state, world, resonate::ui::widgetText(world, state->command_field));
    }
    /* The status bar's second and third segments: the object count and a
       smoothed frame rate, reshaped only when the shown text changes. */
    char bar_text[32] = {};
    std::snprintf(bar_text, sizeof(bar_text), "%u %s",
                  static_cast<unsigned>(state->box_rows.size()),
                  localized("editor.objects").c_str());
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
       read back, re-applied so every pane agrees, and saved once the drag has
       settled. A hidden pane is collapsed, so its divider is what a drag
       would move and there is nothing to read. */
    {
        Layout next = state->layout;
        if (next.scene_shown)
        {
            if (const resonate::ui::RectTransform* scene =
                    world.get<resonate::ui::RectTransform>(state->scene_pane))
            {
                next.scene_width = scene->width;
            }
        }
        if (next.inspector_shown)
        {
            if (const resonate::ui::RectTransform* inspector =
                    world.get<resonate::ui::RectTransform>(state->inspector))
            {
                next.inspector_width = inspector->width;
            }
        }
        /* The console's divider sizes the console itself, so its height is the
           signal; the clamp keeps a drag from eating the viewport. */
        if (next.console_shown)
        {
            if (const resonate::ui::RectTransform* console =
                    world.get<resonate::ui::RectTransform>(state->console_pane))
            {
                float available = 800.0F;
                if (const resonate::ui::ComputedRect* canvas_rect =
                        world.get<resonate::ui::ComputedRect>(state->canvas))
                {
                    available = canvas_rect->height;
                }
                next.console_height = std::clamp(console->height, 96.0F,
                                                 std::fmax(available - 37.0F - 32.0F - 120.0F,
                                                           96.0F));
            }
        }
        if (std::fabs(next.scene_width - state->layout.scene_width) > 0.5F ||
            std::fabs(next.inspector_width - state->layout.inspector_width) > 0.5F ||
            std::fabs(next.console_height - state->layout.console_height) > 0.5F)
        {
            state->layout = next;
            applyLayout(*state, world, state->layout);
            state->save_delay = 0.3F;
        }
        else if (state->save_delay > 0.0F)
        {
            state->save_delay -= delta_seconds;
            if (state->save_delay <= 0.0F)
            {
                saveLayout(state->layout);
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
        /* A material drag coalesces into one undo entry: the value before the
           press is snapshotted, the entry is pushed on release. The closures
           only restore the scrubber -- applyMaterials writes the material from
           it on the next tick, which is also what keeps a later selection from
           being overwritten by a stale pointer. */
        if (const auto* pointer = world.get<resonate::ui::PointerState>(state->canvas);
            pointer != nullptr)
        {
            const auto track = [&](EditorState::ScrubUndo& undo, const EditorState::Scrub& scrub,
                                   const float current, const std::string& name)
            {
                const bool dragging = pointer->down && pointer->press == scrub.handle;
                if (dragging && !undo.dragging)
                {
                    undo.before = current;
                }
                if (!dragging && undo.dragging && std::fabs(current - undo.before) > 1.0e-6F)
                {
                    const float before = undo.before;
                    const float after = current;
                    const Entity handle = scrub.handle;
                    pushUndo(*state, name,
                             [state, handle, before](resonate::ecs::World& world_ref)
                             { (void)resonate::ui::setScrubValue(world_ref, handle, before); },
                             [state, handle, after](resonate::ecs::World& world_ref)
                             { (void)resonate::ui::setScrubValue(world_ref, handle, after); });
                }
                undo.dragging = dragging;
            };
            track(state->roughness_undo, state->roughness, state->roughness.shown, localized("editor.property.roughness"));
            track(state->metallic_undo, state->metallic, state->metallic.shown, localized("editor.property.metallic"));
        }

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
                /* The rename is reversible down to the caption and the field's
                   mirror, or the field's next edit would re-apply the undone
                   text. */
                const std::string previous = state->mirrored_name;
                const std::string current = field_text;
                const Entity row = selected;
                auto apply_name = [state, entity, row](const std::string& name,
                                                       resonate::ecs::World& world_ref)
                {
                    if (!world_ref.alive(entity))
                    {
                        return;
                    }
                    std::string name_error;
                    (void)resonate::hierarchy::setName(world_ref, entity, name, name_error);
                    Entity caption;
                    for (const Entity child : resonate::hierarchy::children(world_ref, row))
                    {
                        if (world_ref.get<resonate::ui::Text>(child) != nullptr)
                        {
                            caption = child;
                            break;
                        }
                    }
                    if (caption.valid())
                    {
                        (void)resonate::ui::setText(world_ref, state->canvas, caption, name,
                                                    name_error);
                    }
                    (void)resonate::ui::setText(world_ref, state->canvas, state->name_field,
                                                name, name_error);
                    state->mirrored_name = name;
                };
                pushUndo(*state, localized("editor.rename"),
                         [apply_name, previous](resonate::ecs::World& world_ref)
                         { apply_name(previous, world_ref); },
                         [apply_name, current](resonate::ecs::World& world_ref)
                         { apply_name(current, world_ref); });
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
                                        localized("editor.selected") + " " + field_text, error);
        }
        /* The position fields write the selected object's LocalTransform. */
        const Entity entity = entityOfRow(*state, selected);
        resonate::hierarchy::LocalTransform* transform =
            entity.valid()
                ? world.get<resonate::hierarchy::LocalTransform>(entity)
                : nullptr;
        applyPositionField(world, *state, state->position_x, transform, entity, 0U);
        applyPositionField(world, *state, state->position_y, transform, entity, 1U);
        applyPositionField(world, *state, state->position_z, transform, entity, 2U);
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
        (void)resonate::ui::setText(world, state->canvas, state->status, localized("editor.selected") + " " + name, error);
        seedMaterial(*state, world, entity);
        const resonate::hierarchy::LocalTransform* transform =
            entity.valid()
                ? world.get<resonate::hierarchy::LocalTransform>(entity)
                : nullptr;
        seedPosition(*state, world, transform);
        (void)resonate::ui::setText(world, state->canvas, state->animation_value,
                                    animationLabel(*state, world, entity), error);
    }
    else
    {
        (void)resonate::ui::setText(world, state->canvas, state->name_field, "", error);
        state->mirrored_name.clear();
        (void)resonate::ui::setText(world, state->canvas, state->status, localized("editor.no_selection"), error);
        seedPosition(*state, world, nullptr);
        (void)resonate::ui::setText(world, state->canvas, state->animation_value,
                                    localized("editor.none"), error);
    }
}

/* Runs once the document exists: every widget the editor drives is addressed by
   its authored key. */
/* The scene tree lists the real scene: one row per object, cloned from the
   hidden prototype so the list can be rebuilt without losing its shape. The
   query only collects, because cloning is a structural change and the world
   refuses one while a query is walking its chunks. */
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
    state->splitter_console = resonate::ui::findByKey(world, canvas, "splitter-console");
    state->inspector = resonate::ui::findByKey(world, canvas, "inspector");
    state->console_pane = resonate::ui::findByKey(world, canvas, "console");
    /* The pane toggles flip a component field, so every pane carries one from
       the start and a click never adds a component mid-frame. */
    for (const Entity pane : {state->scene_pane, state->splitter_left, state->inspector,
                              state->splitter_right, state->console_pane,
                              state->splitter_console})
    {
        if (pane.valid())
        {
            (void)world.add(pane, resonate::ui::Shown{true});
        }
    }
    state->layout = loadLayout();
    applyLayout(*state, world, state->layout);
    const auto attachToggle = [&](const char* key, resonate::ui::EventHandler listener)
    {
        const Entity button = resonate::ui::findByKey(world, canvas, key);
        if (button.valid())
        {
            world.add(button, resonate::ui::Listener{
                                  listener, state,
                                  resonate::ui::eventBit(resonate::ui::EventKind::Click)});
        }
    };
    const auto attachClick = [&](const char* key, resonate::ui::EventHandler handler)
    {
        const Entity button = resonate::ui::findByKey(world, canvas, key);
        if (button.valid())
        {
            world.add(button, resonate::ui::Listener{
                                  handler, state,
                                  resonate::ui::eventBit(resonate::ui::EventKind::Click)});
        }
    };
    attachClick("file", &saveScene);
    attachClick("tb-open", &openScene);
    attachClick("tb-undo", &undoEdit);
    attachClick("tb-redo", &redoEdit);
    attachClick("tb-duplicate", &duplicateEdit);
    attachClick("tb-delete", &deleteEdit);
    attachToggle("tb-scene-pane", &toggleScene);
    attachToggle("tb-inspector", &toggleInspector);
    attachToggle("tb-console", &toggleConsole);
    state->status_fps = resonate::ui::findByKey(world, canvas, "status-fps");
    state->animation_value = resonate::ui::findByKey(world, canvas, "animation-value");
    if (const std::string products = productsPath(); !products.empty())
    {
        std::string products_error;
        state->asset_ids_loaded = state->asset_ids.loadFile(products, products_error);
    }
    /* The console keeps the last lines only; a host bisecting a report reads
       the same stream on stderr instead. */
    if (std::getenv("RESONATE_EDITOR_REPORTS") == nullptr)
    {
        world.setReportSink(state, &reportSink);
    }

    resonate::ecs::QueryDesc desc{};
    const resonate::ecs::ComponentIndex all[] = {
        resonate::ecs::ComponentTraits<resonate::render::MeshInstance>::index,
        resonate::ecs::ComponentTraits<resonate::render::Material>::index};
    desc.all = resonate::Span<const resonate::ecs::ComponentIndex>(all, 2U);
    state->boxes = world.createQuery(desc);

    /* The scene tree lists the real scene. The authored row becomes the hidden
       prototype every visible row is cloned from, so an open can rebuild the
       list after destroying the old rows. */
    const resonate::ecs::Entity template_row = resonate::ui::findByKey(world, canvas, "crate-a");
    const resonate::ecs::Entity scene_list = state->scene_list;
    if (template_row.valid() && scene_list.valid())
    {
        resonate::ui::setShown(world, template_row, false);
        state->row_prototype = template_row;
        rebuildSceneList(*state, world);
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
    extension.keys = &editorKeys;
    extension.keys_context = &state;
    extension.scene = &sceneRootsLoaded;
    extension.scene_context = &state;
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
