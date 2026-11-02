#ifndef RESONATE_APPLICATION_SNAPSHOT_H
#define RESONATE_APPLICATION_SNAPSHOT_H

/*
 * A world snapshot for the save payload: entities, their component bytes and
 * their blobs in one versioned payload.
 *
 * Components travel by schema name and version, not by the writer's component
 * index: the indices are process-local, the names are the contract. Entity and
 * blob handles inside a component are not portable either, so the schema's
 * field descriptors tell the loader where the 8-byte handles are and it
 * rewrites them to the handles the restoring world issued. A registered
 * component with no descriptor is refused — without a layout the writer cannot
 * know which bytes are handles.
 *
 * Payload layout (little-endian):
 *
 *   "RSWP" | u32 format | u32 entity count | u32 blob count
 *   | u32 component count
 *   | component table: u16 name length, name bytes, u32 schema version
 *   | entities, in slot order:
 *       u32 index | u32 generation | u32 component count
 *       | each: u32 table entry | u32 byte length | bytes
 *   | blobs, in slot order:
 *       u32 index | u32 generation | u32 owner index | u32 owner generation
 *       | u64 byte length | bytes
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/core/span.h>
#include <resonate/ecs/world.h>

namespace resonate::dom
{
class ComponentRegistry;
}

namespace resonate::save
{

inline constexpr std::uint32_t kWorldSnapshotFormat = 1U;

/* The migrations a load may run, assembled by the host from the modules that
   own the components: one entry per component and source version, writing the
   current layout. A version with no entry is refused, never guessed. */
class MigrationRegistry
{
  public:
    using Function = bool (*)(const void* payload, std::size_t size, void* out);

    /* False for a duplicate entry or a null callback. */
    bool add(std::string_view component, std::uint32_t from_version, Function migrate);

    /* Null when no registered migration starts at `from_version`. */
    [[nodiscard]] Function find(std::string_view component,
                                std::uint32_t from_version) const noexcept;

  private:
    struct Entry
    {
        std::string component;
        std::uint32_t from_version = 0U;
        Function migrate = nullptr;
    };

    std::vector<Entry> entries_;
};

/* Writes the world's state. The world must not change while this runs: it
   walks the slot and blob tables directly. */
[[nodiscard]] bool encodeWorld(const ecs::World& world, const dom::ComponentRegistry& schema,
                               std::vector<std::byte>& out, std::string& error);

/* Restores a payload into `world`, which should hold no entities: the handles a
   save names belong to the world that wrote it, and every restored handle gets
   the value this world issued. On failure the world may hold part of the
   payload; callers load into a world created for the load and discard it on
   failure, instead of loading into a live world.

   A component the payload names but this build does not register is an error
   once an entity carries it. An older component version runs the registry's
   migration for that version; a version with no migration, or a newer one, is
   refused. */
[[nodiscard]] bool decodeWorld(ecs::World& world, const dom::ComponentRegistry& schema,
                               const MigrationRegistry& migrations,
                               Span<const std::byte> payload, std::string& error);

} // namespace resonate::save

#endif /* RESONATE_APPLICATION_SNAPSHOT_H */
