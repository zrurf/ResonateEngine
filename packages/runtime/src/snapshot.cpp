#include <resonate/application/snapshot.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

#include <resonate/dom/schema.h>
#include <resonate/ecs/blob.h>
#include <resonate/ecs/entity.h>

namespace resonate::save
{
namespace
{

constexpr char kMagic[4] = {'R', 'S', 'W', 'P'};
constexpr const char* kEntityHandleType = "entity";
constexpr const char* kBlobHandleType = "blob";

struct Writer
{
    std::vector<std::byte>& out;

    void u16(std::uint16_t value)
    {
        for (int byte = 0; byte < 2; ++byte)
        {
            out.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void u32(std::uint32_t value)
    {
        for (int byte = 0; byte < 4; ++byte)
        {
            out.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void u64(std::uint64_t value)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            out.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void bytes(const void* data, std::size_t size)
    {
        if (size == 0U)
        {
            return;
        }
        const auto* first = static_cast<const std::byte*>(data);
        out.insert(out.end(), first, first + size);
    }

    void text(const std::string& value)
    {
        bytes(value.data(), value.size());
    }
};

struct Reader
{
    Span<const std::byte> data;
    std::size_t at = 0;

    [[nodiscard]] bool take(std::size_t size, Span<const std::byte>& out)
    {
        if (size > data.size() - at)
        {
            return false;
        }
        out = Span<const std::byte>(data.data() + at, size);
        at += size;
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& out)
    {
        Span<const std::byte> raw;
        if (!take(2U, raw))
        {
            return false;
        }
        out = 0U;
        for (std::size_t byte = 0U; byte < 2U; ++byte)
        {
            out |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(raw[byte]))
                   << (8U * static_cast<unsigned>(byte));
        }
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& out)
    {
        Span<const std::byte> raw;
        if (!take(4U, raw))
        {
            return false;
        }
        out = 0U;
        for (std::size_t byte = 0U; byte < 4U; ++byte)
        {
            out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(raw[byte]))
                   << (8U * static_cast<unsigned>(byte));
        }
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& out)
    {
        Span<const std::byte> raw;
        if (!take(8U, raw))
        {
            return false;
        }
        out = 0U;
        for (std::size_t byte = 0U; byte < 8U; ++byte)
        {
            out |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(raw[byte]))
                   << (8U * static_cast<unsigned>(byte));
        }
        return true;
    }
};

struct TableEntry
{
    std::string name;
    std::uint32_t version = 1U;
    ecs::ComponentIndex index = ecs::kInvalidComponent;
    const dom::ComponentDesc* desc = nullptr;
};

[[nodiscard]] std::uint64_t handleKey(std::uint32_t index, std::uint32_t generation)
{
    return (static_cast<std::uint64_t>(index) << 32U) | generation;
}

[[nodiscard]] bool isHandleField(const dom::ComponentFieldDesc& field)
{
    return field.type != nullptr && (std::strcmp(field.type, kEntityHandleType) == 0 ||
                                     std::strcmp(field.type, kBlobHandleType) == 0);
}

/* The schema must place every handle field inside the component as eight whole
   bytes; a layout that disagrees would make the loader rewrite unrelated data. */
[[nodiscard]] bool validDescriptor(const dom::ComponentDesc& desc, std::uint32_t world_size,
                                   std::string& error)
{
    if (desc.version == 0U)
    {
        error = std::string("component '") + desc.name + "' names schema version zero";
        return false;
    }
    if (desc.size != world_size)
    {
        error = std::string("component '") + desc.name + "' is " + std::to_string(world_size) +
                " bytes in the ECS and " + std::to_string(desc.size) +
                " bytes in the schema";
        return false;
    }
    for (const dom::ComponentFieldDesc& field : desc.fields)
    {
        if (!isHandleField(field))
        {
            continue;
        }
        if (field.size != 8U || field.offset > desc.size || desc.size - field.offset < 8U)
        {
            error = std::string("component '") + desc.name + "' field '" + field.name +
                    "' is not an eight-byte handle inside the component";
            return false;
        }
    }
    return true;
}

struct StoredComponent
{
    std::uint32_t table = 0U;
    std::vector<std::byte> bytes;
};

struct StoredEntity
{
    ecs::Entity saved;
    std::vector<StoredComponent> components;
};

struct StoredBlob
{
    ecs::BlobHandle saved;
    ecs::Entity owner;
    std::vector<std::byte> bytes;
};

/* Runs the migration the host registered for this component and source
   version; the callback writes the current layout. */
[[nodiscard]] bool migrateToCurrent(const MigrationRegistry& migrations, const std::string& name,
                                    const dom::ComponentDesc& desc, std::uint32_t from,
                                    Span<const std::byte> bytes, std::vector<std::byte>& out,
                                    std::string& error)
{
    const MigrationRegistry::Function migrate = migrations.find(name, from);
    if (migrate == nullptr)
    {
        error = "component '" + name + "' is version " + std::to_string(from) +
                " in the save, this build reads version " + std::to_string(desc.version) +
                " and no migration is registered";
        return false;
    }
    out.assign(desc.size, std::byte{});
    if (!migrate(bytes.data(), bytes.size(), out.data()))
    {
        error = "the migration of component '" + name + "' from version " +
                std::to_string(from) + " failed";
        return false;
    }
    return true;
}

/* Rewrites the handle fields to the values this world issued. A reference the
   payload's entity/blob set does not contain becomes the null handle: the save
   recorded a stale reference, and keeping its writer's index would alias. */
void remapHandles(const dom::ComponentDesc& desc, std::vector<std::byte>& bytes,
                  const std::unordered_map<std::uint64_t, ecs::Entity>& entities,
                  const std::unordered_map<std::uint64_t, ecs::BlobHandle>& blobs)
{
    for (const dom::ComponentFieldDesc& field : desc.fields)
    {
        if (!isHandleField(field))
        {
            continue;
        }
        std::byte* const at = bytes.data() + field.offset;
        std::uint32_t index = 0U;
        std::uint32_t generation = 0U;
        std::memcpy(&index, at, sizeof(index));
        std::memcpy(&generation, at + sizeof(index), sizeof(generation));
        const std::uint64_t key = handleKey(index, generation);

        if (std::strcmp(field.type, kEntityHandleType) == 0)
        {
            const auto found = entities.find(key);
            const ecs::Entity value = found != entities.end() ? found->second : ecs::Entity{};
            std::memcpy(at, &value, sizeof(value));
        }
        else
        {
            const auto found = blobs.find(key);
            const ecs::BlobHandle value =
                found != blobs.end() ? found->second : ecs::BlobHandle{};
            std::memcpy(at, &value, sizeof(value));
        }
    }
}

} // namespace

bool encodeWorld(const ecs::World& world, const dom::ComponentRegistry& schema,
                 std::vector<std::byte>& out, std::string& error)
{
    struct Registered
    {
        ecs::ComponentIndex index = ecs::kInvalidComponent;
        const dom::ComponentDesc* desc = nullptr;
    };
    std::vector<Registered> registered;
    std::vector<std::string> names;

    for (ecs::ComponentIndex index = 0; index < ecs::kMaxComponentTypes; ++index)
    {
        const char* const name = world.componentName(index);
        if (name == nullptr)
        {
            continue;
        }
        const dom::ComponentDesc* const desc = schema.find(name);
        if (desc == nullptr)
        {
            error = std::string("component '") + name +
                    "' is registered without a schema descriptor; the snapshot cannot tell its "
                    "entity and blob handles from data";
            return false;
        }
        if (!validDescriptor(*desc, world.componentSize(index), error))
        {
            return false;
        }
        registered.push_back(Registered{index, desc});
        names.emplace_back(name);
    }

    std::vector<ecs::Entity> entities;
    world.forEachEntity(
        &entities,
        [](void* user_data, ecs::Entity entity)
        {
            static_cast<std::vector<ecs::Entity>*>(user_data)->push_back(entity);
        });

    struct BlobRow
    {
        ecs::Entity owner;
        ecs::BlobHandle handle;
        const void* bytes = nullptr;
        std::size_t size = 0U;
    };
    std::vector<BlobRow> blobs;
    world.forEachBlob(
        &blobs,
        [](void* user_data, ecs::Entity owner, ecs::BlobHandle blob, const void* bytes,
           std::size_t size)
        {
            static_cast<std::vector<BlobRow>*>(user_data)->push_back(BlobRow{owner, blob, bytes, size});
        });

    out.clear();
    Writer writer{out};
    writer.bytes(kMagic, sizeof(kMagic));
    writer.u32(kWorldSnapshotFormat);
    writer.u32(static_cast<std::uint32_t>(entities.size()));
    writer.u32(static_cast<std::uint32_t>(blobs.size()));
    writer.u32(static_cast<std::uint32_t>(registered.size()));
    for (std::size_t entry = 0; entry < registered.size(); ++entry)
    {
        if (names[entry].size() > 0xFFFFU)
        {
            error = std::string("component name '") + names[entry] + "' is too long to record";
            return false;
        }
        writer.u16(static_cast<std::uint16_t>(names[entry].size()));
        writer.text(names[entry]);
        writer.u32(registered[entry].desc->version);
    }

    std::vector<std::uint32_t> present;
    for (const ecs::Entity entity : entities)
    {
        present.clear();
        for (std::uint32_t entry = 0; entry < registered.size(); ++entry)
        {
            if (world.has(entity, registered[entry].index))
            {
                present.push_back(entry);
            }
        }
        writer.u32(entity.index);
        writer.u32(entity.generation);
        writer.u32(static_cast<std::uint32_t>(present.size()));
        for (const std::uint32_t entry : present)
        {
            const Registered& component = registered[entry];
            const void* const data = world.get(entity, component.index);
            if (data == nullptr)
            {
                error = std::string("component '") + names[entry] + "' of entity " +
                        std::to_string(entity.index) + " is reported present but unreadable";
                return false;
            }
            writer.u32(entry);
            writer.u32(component.desc->size);
            writer.bytes(data, component.desc->size);
        }
    }

    for (const BlobRow& blob : blobs)
    {
        writer.u32(blob.handle.index);
        writer.u32(blob.handle.generation);
        writer.u32(blob.owner.index);
        writer.u32(blob.owner.generation);
        writer.u64(blob.size);
        if (blob.size != 0U && blob.bytes == nullptr)
        {
            error = "a blob reports a size but no bytes";
            return false;
        }
        writer.bytes(blob.bytes, blob.size);
    }
    return true;
}

bool MigrationRegistry::add(std::string_view component, std::uint32_t from_version,
                            Function migrate)
{
    if (component.empty() || migrate == nullptr || from_version == 0U)
    {
        return false;
    }
    for (const Entry& entry : entries_)
    {
        if (entry.from_version == from_version && entry.component == component)
        {
            return false;
        }
    }
    entries_.push_back(Entry{std::string(component), from_version, migrate});
    return true;
}

MigrationRegistry::Function MigrationRegistry::find(std::string_view component,
                                                    std::uint32_t from_version) const noexcept
{
    for (const Entry& entry : entries_)
    {
        if (entry.from_version == from_version && entry.component == component)
        {
            return entry.migrate;
        }
    }
    return nullptr;
}

bool decodeWorld(ecs::World& world, const dom::ComponentRegistry& schema,
                 const MigrationRegistry& migrations, Span<const std::byte> payload,
                 std::string& error)
{
    if (payload.size() < sizeof(kMagic) + 16U)
    {
        error = "the world snapshot is truncated";
        return false;
    }
    for (std::size_t index = 0; index < sizeof(kMagic); ++index)
    {
        if (std::to_integer<std::uint8_t>(payload[index]) !=
            static_cast<std::uint8_t>(kMagic[index]))
        {
            error = "the payload is not a world snapshot";
            return false;
        }
    }

    Reader reader{payload, sizeof(kMagic)};
    std::uint32_t format = 0U;
    std::uint32_t entity_count = 0U;
    std::uint32_t blob_count = 0U;
    std::uint32_t table_count = 0U;
    if (!reader.u32(format) || !reader.u32(entity_count) || !reader.u32(blob_count) ||
        !reader.u32(table_count))
    {
        error = "the world snapshot header is truncated";
        return false;
    }
    if (format != kWorldSnapshotFormat)
    {
        error = "the world snapshot format " + std::to_string(format) +
                " is not the one this build reads";
        return false;
    }

    std::vector<TableEntry> table(table_count);
    for (std::size_t position = 0; position < table.size(); ++position)
    {
        TableEntry& entry = table[position];
        std::uint16_t length = 0U;
        Span<const std::byte> name;
        if (!reader.u16(length) || !reader.take(length, name) || !reader.u32(entry.version))
        {
            error = "the world snapshot's component table is truncated";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
        if (entry.name.empty() || entry.version == 0U)
        {
            error = "the world snapshot names an empty component or schema version zero";
            return false;
        }
        for (std::size_t earlier = 0; earlier < position; ++earlier)
        {
            if (table[earlier].name == entry.name)
            {
                error = "the world snapshot names component '" + entry.name + "' twice";
                return false;
            }
        }
        entry.index = world.findComponent(entry.name.c_str());
        if (entry.index != ecs::kInvalidComponent)
        {
            entry.desc = schema.find(entry.name);
        }
    }

    std::vector<StoredEntity> entities;
    entities.reserve(std::min<std::uint32_t>(entity_count, 65536U));
    for (std::uint32_t index = 0; index < entity_count; ++index)
    {
        StoredEntity stored;
        std::uint32_t component_count = 0U;
        if (!reader.u32(stored.saved.index) || !reader.u32(stored.saved.generation) ||
            !reader.u32(component_count))
        {
            error = "the world snapshot's entity table is truncated";
            return false;
        }
        if (!stored.saved.valid())
        {
            error = "the world snapshot names a dead entity handle";
            return false;
        }
        stored.components.resize(component_count);
        for (StoredComponent& component : stored.components)
        {
            std::uint32_t size = 0U;
            Span<const std::byte> bytes;
            if (!reader.u32(component.table) || !reader.u32(size) || !reader.take(size, bytes))
            {
                error = "the world snapshot's component payload is truncated";
                return false;
            }
            if (component.table >= table.size())
            {
                error = "the world snapshot names component table entry " +
                        std::to_string(component.table) + " of " +
                        std::to_string(table.size());
                return false;
            }
            component.bytes.assign(bytes.begin(), bytes.end());
        }
        entities.push_back(std::move(stored));
    }

    std::vector<StoredBlob> blobs;
    blobs.reserve(std::min<std::uint32_t>(blob_count, 65536U));
    for (std::uint32_t index = 0; index < blob_count; ++index)
    {
        StoredBlob blob;
        std::uint64_t size = 0U;
        Span<const std::byte> bytes;
        if (!reader.u32(blob.saved.index) || !reader.u32(blob.saved.generation) ||
            !reader.u32(blob.owner.index) || !reader.u32(blob.owner.generation) ||
            !reader.u64(size))
        {
            error = "the world snapshot's blob table is truncated";
            return false;
        }
        if (!blob.saved.valid() || !blob.owner.valid())
        {
            error = "the world snapshot names a dead blob or owner handle";
            return false;
        }
        if (size > reader.data.size() - reader.at ||
            !reader.take(static_cast<std::size_t>(size), bytes))
        {
            error = "the world snapshot's blob bytes are truncated";
            return false;
        }
        blob.bytes.assign(bytes.begin(), bytes.end());
        blobs.push_back(std::move(blob));
    }
    if (reader.at != payload.size())
    {
        error = "the world snapshot has trailing bytes";
        return false;
    }

    /* Every handle is created before any component is restored: a component may
       reference an entity or blob the payload lists later. */
    std::unordered_map<std::uint64_t, ecs::Entity> entity_map;
    entity_map.reserve(entities.size());
    for (const StoredEntity& stored : entities)
    {
        const ecs::Entity created = world.create();
        if (!created.valid())
        {
            error = "the world could not create the restored entities";
            return false;
        }
        if (!entity_map.emplace(handleKey(stored.saved.index, stored.saved.generation), created)
                 .second)
        {
            error = "the world snapshot names an entity handle twice";
            return false;
        }
    }

    std::unordered_map<std::uint64_t, ecs::BlobHandle> blob_map;
    blob_map.reserve(blobs.size());
    for (const StoredBlob& stored : blobs)
    {
        const auto owner = entity_map.find(handleKey(stored.owner.index, stored.owner.generation));
        if (owner == entity_map.end())
        {
            error = "a blob names owner " + std::to_string(stored.owner.index) +
                    ", which the snapshot does not restore";
            return false;
        }
        const ecs::BlobHandle created =
            world.createBlob(owner->second, stored.bytes.size(), stored.bytes.data());
        if (!created.valid())
        {
            error = "the world could not restore a blob";
            return false;
        }
        if (!blob_map.emplace(handleKey(stored.saved.index, stored.saved.generation), created)
                 .second)
        {
            error = "the world snapshot names a blob handle twice";
            return false;
        }
    }

    std::vector<std::byte> component_bytes;
    for (std::size_t entity_index = 0; entity_index < entities.size(); ++entity_index)
    {
        const StoredEntity& stored = entities[entity_index];
        const ecs::Entity target =
            entity_map.at(handleKey(stored.saved.index, stored.saved.generation));
        for (const StoredComponent& stored_component : stored.components)
        {
            const TableEntry& entry = table[stored_component.table];
            if (entry.index == ecs::kInvalidComponent)
            {
                error = "component '" + entry.name +
                        "' is not registered in this build; the save cannot be restored";
                return false;
            }
            if (entry.desc == nullptr)
            {
                error = "component '" + entry.name +
                        "' has no schema descriptor; its handles cannot be restored";
                return false;
            }
            if (!validDescriptor(*entry.desc, world.componentSize(entry.index), error))
            {
                return false;
            }

            if (entry.version == entry.desc->version)
            {
                if (stored_component.bytes.size() != entry.desc->size)
                {
                    error = "component '" + entry.name + "' is " +
                            std::to_string(stored_component.bytes.size()) + " bytes in the save, " +
                            std::to_string(entry.desc->size) + " in this build";
                    return false;
                }
                component_bytes.assign(stored_component.bytes.begin(),
                                       stored_component.bytes.end());
            }
            else if (entry.version < entry.desc->version)
            {
                if (!migrateToCurrent(migrations, entry.name, *entry.desc, entry.version,
                                      Span<const std::byte>(stored_component.bytes.data(),
                                                            stored_component.bytes.size()),
                                      component_bytes, error))
                {
                    return false;
                }
            }
            else
            {
                error = "component '" + entry.name + "' is version " +
                        std::to_string(entry.version) + " in the save, this build reads " +
                        std::to_string(entry.desc->version);
                return false;
            }

            if (world.has(target, entry.index))
            {
                error = "component '" + entry.name + "' appears twice on one entity";
                return false;
            }
            remapHandles(*entry.desc, component_bytes, entity_map, blob_map);
            if (world.add(target, entry.index, component_bytes.data()) == nullptr)
            {
                error = "the world could not restore component '" + entry.name + "'";
                return false;
            }
        }
    }
    return true;
}

} // namespace resonate::save
