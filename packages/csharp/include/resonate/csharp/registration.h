#ifndef RESONATE_CSHARP_REGISTRATION_H
#define RESONATE_CSHARP_REGISTRATION_H

#include <stdint.h>

/*
 * What the engine hands a managed assembly when it loads: a pointer to the
 * dispatch table (opaque to the managed side) and the one call it needs to
 * place an entry. The managed assembly mirrors this struct field for field
 * with blittable types; the binding generator will emit both sides from the
 * same declaration.
 */

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateCSharpRegistration
{
    void* context;
    /* Returns 0 when the entry took the slot, non-zero otherwise. `entry` is
       an [UnmanagedCallersOnly] function pointer. */
    int32_t (*register_entry)(void* context, uint32_t id, void* entry);
} ResonateCSharpRegistration;

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_CSHARP_REGISTRATION_H */
