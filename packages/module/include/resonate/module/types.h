#ifndef RESONATE_MODULE_TYPES_H
#define RESONATE_MODULE_TYPES_H

/*
 * Wire types shared by capability interfaces; the C++ side converts at the edge.
 * Every type is trivially copyable and its layout is fixed.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateVec2
{
    float x;
    float y;
} ResonateVec2;

typedef struct ResonateVec3
{
    float x;
    float y;
    float z;
} ResonateVec3;

typedef struct ResonateVec4
{
    float x;
    float y;
    float z;
    float w;
} ResonateVec4;

/* Row-major, matching the layout a GPU expects. */
typedef struct ResonateMat4
{
    float m[4][4];
} ResonateMat4;

typedef struct ResonateQuat
{
    float x;
    float y;
    float z;
    float w;
} ResonateQuat;

typedef struct ResonateRect
{
    float x;
    float y;
    float width;
    float height;
} ResonateRect;

typedef struct ResonateRay
{
    ResonateVec3 origin;
    ResonateVec3 direction; /* normalised */
} ResonateRay;

typedef struct ResonateRayHit
{
    ResonateVec3 position;
    ResonateVec3 normal;
    float distance;
    uint64_t body; /* opaque provider-side handle */
} ResonateRayHit;

/* A handle, not a pointer; a module cannot dereference it. */
typedef struct ResonateEntity
{
    uint32_t index;
    uint32_t generation;
} ResonateEntity;

#define RESONATE_ENTITY_NULL {0, 0}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RESONATE_MODULE_TYPES_H */
