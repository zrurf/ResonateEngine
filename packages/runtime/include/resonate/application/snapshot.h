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
 * know which bytes are handles — unless the writer names it opaque: the host
 * declares the runtime components it keeps across a save (animation playback,
 * machine clocks), and those bytes are copied verbatim. An opaque component
 * must not contain entity or blob handles.
 *
 * Payload layout (little-endian):
 *
 *   "RSWP" | u32 format | u32 entity count | u32 blob count
 *   | u32 component count
 *   | component table: u16 name length, name bytes, u32 schema version,
 *       u8 opaque (1 = the bytes carry no handles)
 *   | entities, in the order the writer listed them:
 *       u32 index | u32 generation | u32 component count
 *       | each: u32 table entry | u32 byte length | bytes
 *   | blobs, in slot order, owned by one of those entities:
 *       u32 index | u32 generation | u32 owner index | u32 owner generation
 *       | u64 byte length | bytes
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <resonate/core/span.h>
#include <resonate/ecs/world.h>

namespace resonate::dom
{
class ComponentRegistry;
}

namespace resonate::save
{

inline constexpr std::uint32_t kWorldSnapshotFormat = 2U;

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

/* Writes `entities` and only the blobs they own. Every handle must be alive and
   appear once. `opaque_components` names the types whose bytes are copied
   without a schema descriptor; the caller guarantees they hold no handles. The
   world must not change while this runs. */
[[nodiscard]] bool encodeWorld(const ecs::World& world, const dom::ComponentRegistry& schema,
                               Span<const ecs::Entity> entities,
                               Span<const ecs::ComponentIndex> opaque_components,
                               std::vector<std::byte>& out, std::string& error);

/* Every alive entity in slot order, with no opaque components. */
[[nodiscard]] bool encodeWorld(const ecs::World& world, const dom::ComponentRegistry& schema,
                               std::vector<std::byte>& out, std::string& error);

/* Restores a payload into `world`. The handles a save names belong to the world
   that wrote it, so every restored handle gets the value this world issued.
   Entities already in `world` are untouched: a load can happen beside the
   scene it replaces, which is what lets a corrupt payload fail without taking
   the live scene with it. On failure every entity this call created is
   destroyed again, so the world is left as it was.

   `restored` receives the created handles in payload order; `remap` pairs the
   handle the save named with the one this world issued, for a domain whose
   blob contents carry handles the field descriptors cannot see. A component
   the payload names but this build does not register is an error once an
   entity carries it. An older component version runs the registry's migration
   for that version; a version with no migration, or a newer one, is refused.
   The payload records which components are opaque, and those alone are copied
   verbatim when their byte size matches the registered type; a non-opaque
   component with no descriptor in this build is an error, because its bytes
   would restore handles this world never issued. */
[[nodiscard]] bool decodeWorld(ecs::World& world, const dom::ComponentRegistry& schema,
                               const MigrationRegistry& migrations,
                               Span<const std::byte> payload, std::string& error,
                               std::vector<ecs::Entity>* restored = nullptr,
                               std::vector<std::pair<ecs::Entity, ecs::Entity>>* remap = nullptr);

} // namespace resonate::save

#endif /* RESONATE_APPLICATION_SNAPSHOT_H */
