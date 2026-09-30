#ifndef RESONATE_PAL_LIBRARY_H
#define RESONATE_PAL_LIBRARY_H

/*
 * Dynamic library loading. The module host is the intended caller: it loads each
 * plugin library, reads the two symbols a module exports, and unloads it again.
 */

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateLibrary
{
    void* handle;
} ResonateLibrary;

/* Loads a shared library and whatever it depends on. A library that is already
   loaded is returned as a second handle, not loaded twice. */
ResonatePalStatus resonate_pal_library_load(ResonateLibrary* out, const char* path);

/* Releases the handle. The library leaves the process when its last handle
   does. */
void resonate_pal_library_unload(ResonateLibrary* library);

/* NULL when the symbol is absent, which is how a probe is meant to fail: the
   caller decides whether the absence is fatal. The name is a C symbol. */
void* resonate_pal_library_symbol(const ResonateLibrary* library, const char* name);

/* The extension shared libraries carry here, without the dot: "dll", "so" or
   "dylib". Plugin discovery scans a directory for it. */
const char* resonate_pal_library_extension(void);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_LIBRARY_H */
