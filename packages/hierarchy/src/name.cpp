#include <resonate/hierarchy/name.h>

#include <resonate/hierarchy/tree.h>

namespace resonate::hierarchy
{

std::string nameOf(const ecs::World& world, ecs::Entity entity)
{
    const Name* name = world.get<Name>(entity);
    if (name == nullptr || !world.alive(name->text))
    {
        return {};
    }
    const void* bytes = world.blobData(name->text);
    const std::uint64_t size = world.blobSize(name->text);
    if (bytes == nullptr || size == 0U)
    {
        return {};
    }
    return std::string(static_cast<const char*>(bytes), static_cast<std::size_t>(size));
}

bool setName(ecs::World& world, ecs::Entity entity, std::string_view utf8,
             std::string& error)
{
    if (!world.alive(entity))
    {
        error = "the entity is not alive";
        return false;
    }
    ecs::BlobHandle content{};
    if (!utf8.empty())
    {
        content = world.createBlob(entity, utf8.size(), utf8.data());
        if (!content.valid())
        {
            error = "the name bytes could not be attached";
            return false;
        }
    }
    if (Name* name = world.get<Name>(entity))
    {
        const ecs::BlobHandle old = name->text;
        name->text = content;
        if (world.alive(old))
        {
            (void)world.destroyBlob(old);
        }
        (void)world.markChanged<Name>(entity);
        return true;
    }
    if (utf8.empty())
    {
        /* An empty name needs no component; nothing was stored. */
        return true;
    }
    if (world.add(entity, Name{content}) == nullptr)
    {
        (void)world.destroyBlob(content);
        error = "the name component could not be attached";
        return false;
    }
    return true;
}

} // namespace resonate::hierarchy
