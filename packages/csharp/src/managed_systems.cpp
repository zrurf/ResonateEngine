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

void ManagedSystems::invoke(void* context, ecs::World& world, ecs::CommandBuffer&,
                            float delta_seconds)
{
    auto* self = static_cast<ManagedSystems*>(context);
    if (self == nullptr)
    {
        return;
    }
    /* The command buffer reaches managed code when structural behaviour
       lands. The read path is the named component's first non-empty chunk:
       a dense array the managed side wraps in a Span without copying. */
    self->frame_.chunk_data = nullptr;
    self->frame_.chunk_count = 0U;
    if (!self->component_name_.empty())
    {
        if (self->component_ == ecs::kInvalidComponent)
        {
            self->component_ = world.findComponent(self->component_name_.c_str());
        }
        if (self->component_ != ecs::kInvalidComponent)
        {
            const ecs::ComponentIndex all[] = {self->component_};
            ecs::QueryDesc desc;
            desc.all = resonate::Span<const ecs::ComponentIndex>(all, 1U);
            ecs::Query query = world.createQuery(desc);
            struct FirstChunk
            {
                ecs::ComponentIndex component;
                void* data;
                std::uint64_t count;
            };
            FirstChunk first{self->component_, nullptr, 0U};
            query.forEachChunk(
                [](void* chunk_context, ecs::ChunkView view)
                {
                    auto* out = static_cast<FirstChunk*>(chunk_context);
                    if (out->data != nullptr)
                    {
                        return;
                    }
                    out->data = view.componentData(out->component);
                    out->count = view.count;
                },
                &first);
            self->frame_.chunk_data = first.data;
            self->frame_.chunk_count = first.count;
        }
    }
    self->frame_.delta_seconds = delta_seconds;
    self->frame_.frame_index = self->frames_;
    self->table_.invoke(bindings::kSystemTick, &self->frame_);
    ++self->frames_;
}

} // namespace resonate::csharp
