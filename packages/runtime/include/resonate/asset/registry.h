#ifndef RESONATE_ASSET_REGISTRY_H
#define RESONATE_ASSET_REGISTRY_H

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Opaque handle to one loaded asset; only meaningful to the registry that issued it. */
typedef struct ResonateAssetId
{
    uint64_t value;
} ResonateAssetId;

/* Loader discriminator, not a type system. */
typedef uint32_t ResonateAssetType;

#define RESONATE_ASSET_TYPE_INVALID 0u
#define RESONATE_ASSET_TYPE_TEXTURE 1u
#define RESONATE_ASSET_TYPE_MESH 2u
#define RESONATE_ASSET_TYPE_SHADER 3u
#define RESONATE_ASSET_TYPE_AUDIO 4u
#define RESONATE_ASSET_TYPE_SCENE 5u

/* Asset lookup. Loads are reference counted and released through this
   interface, never freed directly. */
typedef struct ResonateAssetRegistry
{
    ResonateCapabilityHeader header;

    ResonateStatus (*load)(void* self, const char* path, ResonateAssetType type,
                           ResonateAssetId* out_id);

    /* Returns RESONATE_E_MISSING for an id already released to zero, which is
       how a caller learns its handle is stale. */
    ResonateStatus (*retain)(void* self, ResonateAssetId id);

    ResonateStatus (*release)(void* self, ResonateAssetId id);

    /* Pointer to the loaded data, owned by the registry; valid while a reference is held. */
    ResonateStatus (*resolve)(void* self, ResonateAssetId id, const void** out_data,
                              uint32_t* out_size);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonateAssetRegistry;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateAssetRegistry>
{
    static constexpr const char* name = "Resonate.Asset.Registry";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_ASSET_REGISTRY_H */
