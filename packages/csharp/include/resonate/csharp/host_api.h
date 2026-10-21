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
    /* The frame's command buffer: structural changes are recorded here and
       played at the frame's sync point, never applied during the tick. */
    void* commands;
    /* Reserves an entity handle; it becomes alive at playback. */
    int32_t (*spawn)(void* host_api, uint64_t* out_entity);
    /* Records an add of the named component with `data` (the component's
       bytes, in the layout the schema generated). */
    int32_t (*add_component)(void* host_api, uint64_t entity, const char* component,
                             const void* data);
} ResonateCSharpHostApi;

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_CSHARP_HOST_API_H */
