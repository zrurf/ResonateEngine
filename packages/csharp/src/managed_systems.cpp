#include <resonate/csharp/managed_systems.hpp>

#include <resonate/ecs/query.h>

namespace resonate::csharp
{
namespace
{

/* The engine hands this to the managed assembly; the host sits behind the
   opaque context and routes the entry to its stage's table. */
extern "C" std::int32_t managedSystemsRegisterEntry(void* context, std::uint32_t stage,
                                                    std::uint32_t id, void* entry)
{
    auto* systems = static_cast<ManagedSystems*>(context);
    return systems->register_entry(stage, id, reinterpret_cast<DispatchEntry>(entry)) ? 0 : 1;
}

/* Handles cross as one 64-bit value: index low, generation high, like the
   C++ Entity. */
ecs::Entity entityFromHandle(std::uint64_t handle)
{
    return ecs::Entity{static_cast<std::uint32_t>(handle),
                       static_cast<std::uint32_t>(handle >> 32)};
}

std::uint64_t handleFromEntity(ecs::Entity entity)
{
    return (static_cast<std::uint64_t>(entity.generation) << 32) | entity.index;
}

extern "C" std::int32_t spawnEntity(void* host_api, std::uint64_t* out_entity)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    auto* commands = static_cast<ecs::CommandBuffer*>(api->commands);
    if (commands == nullptr)
    {
        return 1;
    }
    const ecs::Entity entity = commands->create();
    if (out_entity != nullptr)
    {
        *out_entity = handleFromEntity(entity);
    }
    return 0;
}

extern "C" std::int32_t addComponent(void* host_api, std::uint64_t handle,
                                     const char* component, void* data)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    auto* commands = static_cast<ecs::CommandBuffer*>(api->commands);
    auto* world = static_cast<ecs::World*>(api->world);
    if (commands == nullptr || world == nullptr || component == nullptr || data == nullptr)
    {
        return 1;
    }
    const ecs::ComponentIndex index = world->findComponent(component);
    if (index == ecs::kInvalidComponent)
    {
        return 1;
    }
    commands->add(entityFromHandle(handle), index, data);
    return 0;
}

extern "C" std::int32_t removeComponent(void* host_api, std::uint64_t handle,
                                        const char* component)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    auto* commands = static_cast<ecs::CommandBuffer*>(api->commands);
    auto* world = static_cast<ecs::World*>(api->world);
    if (commands == nullptr || world == nullptr || component == nullptr)
    {
        return 1;
    }
    const ecs::ComponentIndex index = world->findComponent(component);
    if (index == ecs::kInvalidComponent)
    {
        return 1;
    }
    commands->remove(entityFromHandle(handle), index);
    return 0;
}

extern "C" std::int32_t despawnEntity(void* host_api, std::uint64_t handle)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    auto* commands = static_cast<ecs::CommandBuffer*>(api->commands);
    if (commands == nullptr)
    {
        return 1;
    }
    commands->destroy(entityFromHandle(handle));
    return 0;
}

/* The other-entity read path: the pointer stays valid for the frame, and the
   managed side only reads through it. */
extern "C" std::int32_t readComponent(void* host_api, std::uint64_t handle,
                                      const char* component, void* out_data)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    const auto* world = static_cast<const ecs::World*>(api->world);
    if (world == nullptr || component == nullptr || out_data == nullptr)
    {
        return 1;
    }
    const ecs::ComponentIndex index = world->findComponent(component);
    if (index == ecs::kInvalidComponent)
    {
        return 1;
    }
    const void* data = world->get(entityFromHandle(handle), index);
    if (data == nullptr)
    {
        return 1;
    }
    *static_cast<const void**>(out_data) = data;
    return 0;
}

/* The frame's actions, already sampled by the input system; the managed side
   reads pressed/held/released and cannot write them. */
extern "C" std::int32_t readAction(void* host_api, const char* name, void* out_state)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    const auto* actions = static_cast<const gameplay::ActionMap*>(api->actions);
    auto* state = static_cast<bindings::ActionState*>(out_state);
    if (actions == nullptr || name == nullptr || state == nullptr)
    {
        return 1;
    }
    const gameplay::ActionState* found = actions->state(name);
    if (found == nullptr)
    {
        return 1;
    }
    state->pressed = found->pressed ? 1 : 0;
    state->held = found->held ? 1 : 0;
    state->released = found->released ? 1 : 0;
    state->value = found->value;
    return 0;
}

/* A dense write over a chunk cannot know which rows it touched, so its owner
   marks them; the observer dispatch stays the world's. The chunk's rows ride
   the call, so the frame may hold many chunks. */
extern "C" std::int32_t markChunkChanged(void* host_api, void* entities, std::uint64_t count)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    auto* world = static_cast<ecs::World*>(api->world);
    const auto* rows = static_cast<const ecs::Entity*>(entities);
    if (world == nullptr || rows == nullptr)
    {
        return 1;
    }
    for (std::uint64_t row = 0; row < count; ++row)
    {
        world->markChanged(rows[row], api->component);
    }
    return 0;
}

/* The request the managed patch host reads; it mirrors the bootstrap's
   InstallRequest field for field. */
struct PatchRequest
{
    const char* path;
    const char* entry_type;
    void* registration;
    void* context;
};

/* The registration callback a patch reaches: the displaced entry is
   remembered, so an unload can put it back. */
extern "C" std::int32_t patchEntry(void* context, std::uint32_t stage, std::uint32_t id,
                                   void* entry)
{
    auto* systems = static_cast<ManagedSystems*>(context);
    return systems->replace_tracked(stage, id, reinterpret_cast<DispatchEntry>(entry)) ? 0 : 1;
}

/* The one system id a stage's table holds; the registration generator names
   the same contract entry on the managed side. */
std::uint32_t systemId(ResonateStage stage)
{
    switch (stage)
    {
    case RESONATE_STAGE_EARLY_UPDATE:
        return bindings::kSystemEarlyUpdate;
    case RESONATE_STAGE_UPDATE:
        return bindings::kSystemUpdate;
    case RESONATE_STAGE_PHYSICS:
        return bindings::kSystemPhysics;
    case RESONATE_STAGE_LATE_UPDATE:
        return bindings::kSystemLateUpdate;
    case RESONATE_STAGE_RENDER:
        return bindings::kSystemRender;
    case RESONATE_STAGE_PRESENT:
        return bindings::kSystemPresent;
    default:
        return 0U;
    }
}

void invokeEarlyUpdate(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                       float delta_seconds)
{
    ManagedSystems::invokeStage(RESONATE_STAGE_EARLY_UPDATE, context, world, commands,
                                delta_seconds);
}

void invokeUpdate(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                  float delta_seconds)
{
    ManagedSystems::invokeStage(RESONATE_STAGE_UPDATE, context, world, commands, delta_seconds);
}

void invokePhysics(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                   float delta_seconds)
{
    ManagedSystems::invokeStage(RESONATE_STAGE_PHYSICS, context, world, commands, delta_seconds);
}

void invokeLateUpdate(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                      float delta_seconds)
{
    ManagedSystems::invokeStage(RESONATE_STAGE_LATE_UPDATE, context, world, commands,
                                delta_seconds);
}

void invokeRender(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                  float delta_seconds)
{
    ManagedSystems::invokeStage(RESONATE_STAGE_RENDER, context, world, commands, delta_seconds);
}

void invokePresent(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                   float delta_seconds)
{
    ManagedSystems::invokeStage(RESONATE_STAGE_PRESENT, context, world, commands, delta_seconds);
}

const ManagedSystems::StageEntry kStageEntries[ManagedSystems::kStageEntryCount] = {
    {RESONATE_STAGE_EARLY_UPDATE, "csharp.early_update", &invokeEarlyUpdate},
    {RESONATE_STAGE_UPDATE, "csharp.update", &invokeUpdate},
    {RESONATE_STAGE_PHYSICS, "csharp.physics", &invokePhysics},
    {RESONATE_STAGE_LATE_UPDATE, "csharp.late_update", &invokeLateUpdate},
    {RESONATE_STAGE_RENDER, "csharp.render", &invokeRender},
    {RESONATE_STAGE_PRESENT, "csharp.present", &invokePresent},
};

} // namespace

const ManagedSystems::StageEntry* ManagedSystems::stageEntries()
{
    return kStageEntries;
}

ManagedSystems::~ManagedSystems()
{
    unload();
}

bool ManagedSystems::load(const std::string& runtime_config, const std::string& assembly,
                          const std::string& type_name, std::string& error)
{
    error.clear();
    unload();

    if (!host_.initialize(runtime_config, error))
    {
        return false;
    }
    registration_ = host_.load_function(assembly, type_name, kRegisterEntry, error);
    if (registration_ == nullptr)
    {
        host_.shutdown();
        return false;
    }

    for (DispatchTable& table : tables_)
    {
        table.resize(64U);
    }
    /* A 16 KiB chunk holds a few hundred rows, so a million entities stay well
       inside this reservation. */
    chunk_spans_.reserve(4096U);
    const auto register_fn = reinterpret_cast<int (*)(void*)>(registration_);
    bindings::Registration registration{this, &managedSystemsRegisterEntry};
    if (register_fn(&registration) != 0)
    {
        error = "the managed assembly refused to register its systems";
        registration_ = nullptr;
        host_.shutdown();
        return false;
    }
    return true;
}

void ManagedSystems::unload()
{
    /* The slots point into managed code; restore them before the runtime
       closes. The contexts themselves die with the process. */
    while (!patch_frames_.empty())
    {
        PatchFrame frame = std::move(patch_frames_.back());
        patch_frames_.pop_back();
        restoreFrame(frame);
    }
    bootstrap_unload_ = nullptr;
    registration_ = nullptr;
    for (DispatchTable& table : tables_)
    {
        table.resize(64U);
    }
    frames_ = 0U;
    frame_ = bindings::SystemContext{};
    component_ = ecs::kInvalidComponent;
    query_world_ = nullptr;
    query_ready_ = false;
    query_ = ecs::Query{};
    chunk_spans_.clear();
    actions_ = nullptr;
    host_.shutdown();
}

bool ManagedSystems::load_patch(const std::string& bootstrap_assembly,
                                const std::string& bootstrap_type,
                                const std::string& patch_assembly, const std::string& entry_type,
                                std::string& error)
{
    error.clear();
    if (!host_.initialized())
    {
        error = "the runtime is not initialized";
        return false;
    }
    if (!patch_frames_.empty())
    {
        error = "a patch is already installed";
        return false;
    }

    void* install = host_.load_function(bootstrap_assembly, bootstrap_type, "Install", error);
    if (install == nullptr)
    {
        return false;
    }
    void* unload = host_.load_function(bootstrap_assembly, bootstrap_type, "Unload", error);
    if (unload == nullptr)
    {
        return false;
    }

    /* The frame exists before Install: the patch's Register runs inside it and
       records what it displaces. */
    patch_frames_.push_back(PatchFrame{});
    bindings::Registration registration{this, &patchEntry};
    PatchRequest request{patch_assembly.c_str(), entry_type.c_str(), &registration, nullptr};
    const int status = reinterpret_cast<int (*)(void*)>(install)(&request);
    if (status != 0 || request.context == nullptr)
    {
        PatchFrame frame = std::move(patch_frames_.back());
        patch_frames_.pop_back();
        restoreFrame(frame);
        error = "the patch host refused the patch (" + std::to_string(status) + ")";
        return false;
    }
    patch_frames_.back().context = request.context;
    bootstrap_unload_ = unload;
    return true;
}

bool ManagedSystems::unload_patch(std::string& error)
{
    error.clear();
    if (patch_frames_.empty() || bootstrap_unload_ == nullptr)
    {
        error = "no patch is installed";
        return false;
    }

    PatchFrame frame = std::move(patch_frames_.back());
    patch_frames_.pop_back();
    /* Restore first: the slots point into the code the unload frees. */
    restoreFrame(frame);
    const int status = reinterpret_cast<int (*)(void*)>(bootstrap_unload_)(frame.context);
    if (status != 0)
    {
        error = "the patch host refused the unload (" + std::to_string(status) + ")";
        return false;
    }
    if (patch_frames_.empty())
    {
        bootstrap_unload_ = nullptr;
    }
    return true;
}

bool ManagedSystems::register_entry(std::uint32_t stage, std::uint32_t id, DispatchEntry entry)
{
    if (stage >= tables_.size() || stage == RESONATE_STAGE_LOADING)
    {
        return false;
    }
    return tables_[stage].set(id, entry);
}

bool ManagedSystems::usesStage(ResonateStage stage) const
{
    const std::size_t index = static_cast<std::size_t>(stage);
    return index < tables_.size() && tables_[index].any();
}

bool ManagedSystems::replace_tracked(std::uint32_t stage, std::uint32_t id, DispatchEntry entry)
{
    if (patch_frames_.empty() || stage >= tables_.size() ||
        stage == RESONATE_STAGE_LOADING)
    {
        return false;
    }
    PatchFrame& frame = patch_frames_.back();
    bool tracked = false;
    for (const PreviousEntry& previous : frame.previous)
    {
        if (previous.stage == stage && previous.id == id)
        {
            tracked = true;
            break;
        }
    }
    if (!tracked)
    {
        frame.previous.push_back(PreviousEntry{stage, id, tables_[stage].get(id)});
    }
    return tables_[stage].replace(id, entry);
}

void ManagedSystems::restoreFrame(PatchFrame& frame)
{
    for (auto previous = frame.previous.rbegin(); previous != frame.previous.rend(); ++previous)
    {
        DispatchTable& table = tables_[previous->stage];
        if (previous->entry != nullptr)
        {
            table.replace(previous->id, previous->entry);
        }
        else
        {
            table.clear(previous->id);
        }
    }
}

void ManagedSystems::set_user(void* user)
{
    frame_.user = user;
}

void ManagedSystems::set_actions(const gameplay::ActionMap* actions)
{
    actions_ = actions;
}

void ManagedSystems::set_component(const char* name)
{
    component_name_ = name != nullptr ? name : "";
    component_ = ecs::kInvalidComponent;
    /* The name decides the query's component; the old one is stale even when
       it would still name a registered type. */
    query_ = ecs::Query{};
    query_world_ = nullptr;
    query_ready_ = false;
}

void ManagedSystems::invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                            float delta_seconds)
{
    invokeStage(RESONATE_STAGE_UPDATE, context, world, commands, delta_seconds);
}

void ManagedSystems::invokeStage(ResonateStage stage, void* context, ecs::World& world,
                                 ecs::CommandBuffer& commands, float delta_seconds)
{
    auto* self = static_cast<ManagedSystems*>(context);
    const std::size_t index = static_cast<std::size_t>(stage);
    if (self == nullptr || index <= static_cast<std::size_t>(RESONATE_STAGE_LOADING) ||
        index >= self->tables_.size() || !self->tables_[index].any())
    {
        return;
    }

    const std::uint32_t id = systemId(stage);
    self->frame_.delta_seconds = delta_seconds;
    self->frame_.frame_index = self->frames_;
    self->api_.world = &world;
    self->api_.mark_chunk_changed = &markChunkChanged;
    self->api_.commands = &commands;
    self->api_.spawn = &spawnEntity;
    self->api_.add_component = &addComponent;
    self->api_.remove_component = &removeComponent;
    self->api_.despawn = &despawnEntity;
    self->api_.read_component = &readComponent;
    self->api_.actions = const_cast<gameplay::ActionMap*>(self->actions_);
    self->api_.read_action = &readAction;
    /* The self view checks its type against the name the host actually handed
       over, so a wrong T cannot alias the chunk. */
    self->api_.component_name =
        self->component_name_.empty() ? nullptr : self->component_name_.c_str();
    self->api_.component_name_size = static_cast<std::uint32_t>(self->component_name_.size());
    self->frame_.host_api = &self->api_;

    /* The read path is the named component, gathered into one list and handed
       over with a single boundary call: the FFI budget is per stage, not per
       chunk. The managed side iterates the dense arrays without copying. The
       query is created once and catches up with new archetypes itself; a stage
       with no matching chunk still runs once with an empty list, which is what
       lets a behaviour spawn the first entity of the component. */
    self->chunk_spans_.clear();
    if (!self->component_name_.empty())
    {
        if (self->component_ == ecs::kInvalidComponent)
        {
            self->component_ = world.findComponent(self->component_name_.c_str());
        }
        if (self->component_ != ecs::kInvalidComponent)
        {
            self->api_.component = self->component_;
            if (!self->query_ready_ || self->query_world_ != &world)
            {
                const ecs::ComponentIndex all[] = {self->component_};
                ecs::QueryDesc desc;
                desc.all = resonate::Span<const ecs::ComponentIndex>(all, 1U);
                self->query_ = world.createQuery(desc);
                self->query_world_ = &world;
                self->query_ready_ = self->query_.valid();
            }
            if (self->query_ready_)
            {
                self->query_.forEachChunk(
                    [](void* chunk_context, ecs::ChunkView view)
                    {
                        auto* systems = static_cast<ManagedSystems*>(chunk_context);
                        systems->chunk_spans_.push_back(bindings::ChunkSpan{
                            view.componentData(systems->component_),
                            const_cast<ecs::Entity*>(view.entities), view.count});
                    },
                    self);
            }
        }
    }
    self->frame_.chunks =
        self->chunk_spans_.empty() ? nullptr : self->chunk_spans_.data();
    self->frame_.chunk_count = self->chunk_spans_.size();
    self->tables_[index].invoke(id, &self->frame_);

    if (stage == RESONATE_STAGE_UPDATE)
    {
        ++self->frames_;
    }
}

} // namespace resonate::csharp
