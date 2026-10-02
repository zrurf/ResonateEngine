#include <resonate/ecs/command_buffer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "world_internal.h"

namespace resonate::ecs
{

struct CommandBuffer::Impl
{
    enum class Kind : std::uint8_t
    {
        Create,
        Destroy,
        Add,
        Remove,
    };

    struct Command
    {
        Kind kind = Kind::Create;
        Entity entity{};
        ComponentIndex component = kInvalidComponent;
        std::uint32_t payload = 0; /* offset into `bytes` */
        std::uint32_t size = 0;
    };

    World* world = nullptr;
    detail::WorldImpl* impl = nullptr;
    std::vector<Command> commands;
    std::vector<std::byte> bytes;

    /* Handles this buffer reserved and has not handed to the world. */
    std::vector<Entity> holds;

    /* An entity a command may name: alive now, or created here and not yet
       played. */
    bool names(Entity entity) const
    {
        for (const Entity held : holds)
        {
            if (held == entity)
            {
                return true;
            }
        }
        return world->alive(entity);
    }
};

CommandBuffer::CommandBuffer(World& world) : impl_(std::make_unique<Impl>())
{
    impl_->world = &world;
    impl_->impl = world.impl_.get();
}

CommandBuffer::~CommandBuffer()
{
    if (!impl_->holds.empty())
    {
        impl_->impl->say(World::Report::Warning,
                         "%zu recorded create(s) were never played; their handles are released",
                         impl_->holds.size());
    }
    clear();
}

Entity CommandBuffer::create()
{
    const Entity entity = impl_->world->reserveEntity();
    if (!entity.valid())
    {
        return entity;
    }

    impl_->commands.push_back(Impl::Command{Impl::Kind::Create, entity});
    impl_->holds.push_back(entity);
    return entity;
}

void CommandBuffer::destroy(Entity entity)
{
    if (!impl_->names(entity))
    {
        impl_->impl->say(World::Report::Error,
                         "destroy: entity %u:%u is neither alive nor created by this buffer",
                         entity.index, entity.generation);
        return;
    }
    impl_->commands.push_back(Impl::Command{Impl::Kind::Destroy, entity});
}

void CommandBuffer::add(Entity entity, ComponentIndex component, const void* value)
{
    if (!impl_->impl->requireType(component, "add"))
    {
        return;
    }
    if (!impl_->names(entity))
    {
        impl_->impl->say(World::Report::Error,
                         "add: entity %u:%u is neither alive nor created by this buffer",
                         entity.index, entity.generation);
        return;
    }

    const std::uint32_t size = impl_->impl->components[component].size;
    const std::uint32_t offset = static_cast<std::uint32_t>(impl_->bytes.size());
    impl_->bytes.resize(impl_->bytes.size() + size);
    if (value != nullptr)
    {
        std::memcpy(impl_->bytes.data() + offset, value, size);
    }
    else
    {
        std::memset(impl_->bytes.data() + offset, 0, size);
    }

    impl_->commands.push_back(Impl::Command{Impl::Kind::Add, entity, component, offset, size});
}

void CommandBuffer::remove(Entity entity, ComponentIndex component)
{
    if (!impl_->impl->requireType(component, "remove"))
    {
        return;
    }
    if (!impl_->names(entity))
    {
        impl_->impl->say(World::Report::Error,
                         "remove: entity %u:%u is neither alive nor created by this buffer",
                         entity.index, entity.generation);
        return;
    }
    impl_->commands.push_back(Impl::Command{Impl::Kind::Remove, entity, component});
}

std::uint32_t CommandBuffer::commandCount() const noexcept
{
    return static_cast<std::uint32_t>(impl_->commands.size());
}

bool CommandBuffer::empty() const noexcept
{
    return impl_->commands.empty();
}

void CommandBuffer::clear()
{
    impl_->commands.clear();
    impl_->bytes.clear();
    for (const Entity entity : impl_->holds)
    {
        impl_->world->releaseEntity(entity);
    }
    impl_->holds.clear();
}

void World::play(CommandBuffer& buffer)
{
    if (!impl_->structural())
    {
        return;
    }

    CommandBuffer::Impl& source = *buffer.impl_;
    for (const CommandBuffer::Impl::Command& command : source.commands)
    {
        switch (command.kind)
        {
            case CommandBuffer::Impl::Kind::Create:
                /* A create that cannot place its entity gives the reservation back
                   rather than leaking it. */
                if (!impl_->materialize(command.entity))
                {
                    impl_->releaseReservation(command.entity);
                }
                break;
            case CommandBuffer::Impl::Kind::Destroy:
                destroy(command.entity);
                break;
            case CommandBuffer::Impl::Kind::Add:
                add(command.entity, command.component, source.bytes.data() + command.payload);
                break;
            case CommandBuffer::Impl::Kind::Remove:
                remove(command.entity, command.component);
                break;
        }
    }

    source.commands.clear();
    source.bytes.clear();
    source.holds.clear();
}

} // namespace resonate::ecs
