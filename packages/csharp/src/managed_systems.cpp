#include <resonate/csharp/managed_systems.hpp>

#include <resonate/csharp/registration.h>

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
    host_.shutdown();
}

void ManagedSystems::set_user(void* user)
{
    frame_.user = user;
}

void ManagedSystems::invoke(void* context, ecs::World&, ecs::CommandBuffer&,
                            float delta_seconds)
{
    auto* self = static_cast<ManagedSystems*>(context);
    if (self == nullptr)
    {
        return;
    }
    /* The world and the command buffer reach managed code when the Behaviour
       facade lands; the frame context is the whole surface for now. */
    self->frame_.delta_seconds = delta_seconds;
    self->frame_.frame_index = self->frames_;
    self->table_.invoke(bindings::kSystemTick, &self->frame_);
    ++self->frames_;
}

} // namespace resonate::csharp
