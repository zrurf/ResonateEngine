#ifndef RESONATE_PAL_STATUS_H
#define RESONATE_PAL_STATUS_H

#include <stdint.h>

/*
 * PAL's own result codes.
 *
 * Every fallible PAL call returns one of these. Backends translate errno,
 * GetLastError and the like into this table at their boundary, so no platform
 * error code reaches a caller.
 *
 * Calls that cannot fail return their value directly, and predicates return
 * 1 or 0.
 */
typedef enum ResonatePalStatus : uint8_t
{
    RESONATE_PAL_OK = 0,
    RESONATE_PAL_UNSUPPORTED = 1,    /* this platform cannot do it */
    RESONATE_PAL_INVALID = 2,        /* a caller passed something malformed */
    RESONATE_PAL_NOT_FOUND = 3,      /* path or device does not exist */
    RESONATE_PAL_ALREADY_EXISTS = 4, /* create hit an existing name */
    RESONATE_PAL_ACCESS = 5,         /* permission denied */
    RESONATE_PAL_AGAIN = 6,          /* would block; retry later */
    RESONATE_PAL_TIMEOUT = 7,        /* the wait expired */
    RESONATE_PAL_EXHAUSTED = 8,      /* an enumeration reached its end */
    RESONATE_PAL_IO = 9,             /* the device reported a failure */
    RESONATE_PAL_OUT_OF_MEMORY = 10,
    RESONATE_PAL_UNKNOWN = 11 /* the backend could not classify it */
} ResonatePalStatus;

#endif /* RESONATE_PAL_STATUS_H */
