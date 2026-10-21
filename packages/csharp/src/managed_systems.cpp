#include <resonate/csharp/managed_systems.hpp>

#include <resonate/ecs/query.h>

namespace resonate::csharp
{
namespace
{

/* The engine hands this to the managed assembly; the table sits behind the
   opaque context. */
extern "C" std::int32_t managedSystemsRegisterEntry(void* context, std::uint32_t id, void* entry)
{
    auto* table = static_cast<DispatchTable*>(context);
    return table->set(id, reinterpret_cast<DispatchEntry>(entry)) ? 0 : 1;
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
        *out_entity = (static_cast<std::uint64_t>(entity.generation) << 32) | entity.index;
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
    /* The handle packs index low, generation high, like the C++ Entity. */
    const ecs::Entity entity{static_cast<std::uint32_t>(handle),
                             static_cast<std::uint32_t>(handle >> 32)};
    commands->add(entity, index, data);
    return 0;
}

/* A dense write over a chunk cannot know which rows it touched, so its owner
   marks them; the observer dispatch stays the world's. */
extern "C" std::int32_t markChunkChanged(void* host_api)
{
    auto* api = static_cast<bindings::HostApi*>(host_api);
    auto* world = static_cast<ecs::World*>(api->world);
    const auto* entities = static_cast<const ecs::Entity*>(api->entity_data);
    if (world == nullptr || entities == nullptr)
    {
        return 1;
    }
    for (std::uint64_t row = 0; row < api->chunk_count; ++row)
    {
        world->markChanged(entities[row], api->component);
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
extern "C" std::int32_t patchEntry(void* context, std::uint32_t id, void* entry)
{
    auto* systems = static_cast<ManagedSystems*>(context);
    return systems->replace_tracked(id, reinterpret_cast<DispatchEntry>(entry)) ? 0 : 1;
}

} // namespace

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

    table_.resize(64U);
    const auto register_fn = reinterpret_cast<int (*)(void*)>(registration_);
    bindings::Registration registration{&table_, &managedSystemsRegisterEntry};
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
    table_.resize(64U);
    frames_ = 0U;
    frame_ = bindings::SystemContext{};
    component_ = ecs::kInvalidComponent;
    query_ready_ = false;
    query_ = ecs::Query{};
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

bool ManagedSystems::replace_tracked(std::uint32_t id, DispatchEntry entry)
{
    if (patch_frames_.empty())
    {
        return false;
    }
    PatchFrame& frame = patch_frames_.back();
    bool tracked = false;
    for (const std::pair<std::uint32_t, DispatchEntry>& previous : frame.previous)
    {
        if (previous.first == id)
        {
            tracked = true;
            break;
        }
    }
    if (!tracked)
    {
        frame.previous.emplace_back(id, table_.get(id));
    }
    return table_.replace(id, entry);
}

void ManagedSystems::restoreFrame(PatchFrame& frame)
{
    for (auto previous = frame.previous.rbegin(); previous != frame.previous.rend(); ++previous)
    {
        if (previous->second != nullptr)
        {
            table_.replace(previous->first, previous->second);
        }
        else
        {
            table_.clear(previous->first);
        }
    }
}

void ManagedSystems::set_user(void* user)
{
    frame_.user = user;
}

void ManagedSystems::set_component(const char* name)
{
    component_name_ = name != nullptr ? name : "";
    component_ = ecs::kInvalidComponent;
}

void ManagedSystems::invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                            float delta_seconds)
{
    auto* self = static_cast<ManagedSystems*>(context);
    if (self == nullptr)
    {
        return;
    }

    self->frame_.delta_seconds = delta_seconds;
    self->frame_.frame_index = self->frames_;
    self->api_.world = &world;
    self->api_.mark_chunk_changed = &markChunkChanged;
    self->api_.commands = &commands;
    self->api_.spawn = &spawnEntity;
    self->api_.add_component = &addComponent;
    self->frame_.host_api = &self->api_;

    /* The read path is the named component, one call per non-empty chunk: the
       body sees a dense array it wraps in a Span without copying. The query is
       created once and catches up with new archetypes itself. */
    bool called = false;
    if (!self->component_name_.empty())
    {
        if (self->component_ == ecs::kInvalidComponent)
        {
            self->component_ = world.findComponent(self->component_name_.c_str());
        }
        if (self->component_ != ecs::kInvalidComponent)
        {
            self->api_.component = self->component_;
            if (!self->query_ready_)
            {
                const ecs::ComponentIndex all[] = {self->component_};
                ecs::QueryDesc desc;
                desc.all = resonate::Span<const ecs::ComponentIndex>(all, 1U);
                self->query_ = world.createQuery(desc);
                self->query_ready_ = self->query_.valid();
            }
            if (self->query_ready_)
            {
                self->query_.forEachChunk(
                    [](void* chunk_context, ecs::ChunkView view)
                    {
                        auto* systems = static_cast<ManagedSystems*>(chunk_context);
                        systems->frame_.chunk_data =
                            view.componentData(systems->component_);
                        systems->frame_.chunk_count = view.count;
                        systems->frame_.entity_data =
                            const_cast<ecs::Entity*>(view.entities);
                        systems->api_.chunk_data = systems->frame_.chunk_data;
                        systems->api_.chunk_count = view.count;
                        systems->api_.entity_data =
                            const_cast<ecs::Entity*>(view.entities);
                        systems->table_.invoke(bindings::kSystemTick, &systems->frame_);
                    },
                    self);
                called = true;
            }
        }
    }
    if (!called)
    {
        self->frame_.chunk_data = nullptr;
        self->frame_.chunk_count = 0U;
        self->frame_.entity_data = nullptr;
        self->api_.chunk_data = nullptr;
        self->api_.chunk_count = 0U;
        self->api_.entity_data = nullptr;
        self->table_.invoke(bindings::kSystemTick, &self->frame_);
    }
    ++self->frames_;
}

} // namespace resonate::csharp
