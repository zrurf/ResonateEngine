#ifndef RESONATE_CSHARP_HOST_API_H
#define RESONATE_CSHARP_HOST_API_H

#include <stdint.h>

/*
 * The calls a managed system may make while it runs. The frame context points
 * at one of these per chunk; the managed assembly mirrors the struct field for
 * field with blittable types. Direct writes to the chunk are stamped through
 * mark_chunk_changed, which is how a change observer still sees them.
 */

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateCSharpHostApi
{
    void* world;
    uint32_t component;
    void* chunk_data;
    uint64_t chunk_count;
    void* entity_data;
    /* Stamps every row of the current chunk; returns 0 when it did. */
    int32_t (*mark_chunk_changed)(void* host_api);
} ResonateCSharpHostApi;

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_CSHARP_HOST_API_H */
