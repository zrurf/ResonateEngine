#include <resonate/csharp/managed_systems.hpp>

#include <resonate/csharp/registration.h>

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
    auto* api = static_cast<ResonateCSharpHostApi*>(host_api);
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
                                     const char* component, const void* data)
{
    auto* api = static_cast<ResonateCSharpHostApi*>(host_api);
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
    auto* api = static_cast<ResonateCSharpHostApi*>(host_api);
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
    ResonateCSharpRegistration registration{&table_, &managedSystemsRegisterEntry};
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
    registration_ = nullptr;
    table_.resize(64U);
    frames_ = 0U;
    frame_ = bindings::SystemContext{};
    component_ = ecs::kInvalidComponent;
    query_ready_ = false;
    query_ = ecs::Query{};
    host_.shutdown();
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
