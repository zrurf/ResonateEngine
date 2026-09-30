#include <resonate/pal/library.h>

#include <dlfcn.h>

#include <resonate/pal/io.h>

#include "status.h"

extern "C"
{

ResonatePalStatus resonate_pal_library_load(ResonateLibrary* out, const char* path)
{
    if (out == nullptr || path == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    out->handle = nullptr;

    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        /* dlerror reports its own vocabulary, so the errno mapping does not
           apply; a missing file is the case a caller branches on. */
        return resonate_pal_io_exists(path) != 0U ? RESONATE_PAL_IO : RESONATE_PAL_NOT_FOUND;
    }

    out->handle = handle;
    return RESONATE_PAL_OK;
}

void resonate_pal_library_unload(ResonateLibrary* library)
{
    if (library == nullptr || library->handle == nullptr)
    {
        return;
    }
    dlclose(library->handle);
    library->handle = nullptr;
}

void* resonate_pal_library_symbol(const ResonateLibrary* library, const char* name)
{
    if (library == nullptr || library->handle == nullptr || name == nullptr)
    {
        return nullptr;
    }
    dlerror();
    return dlsym(library->handle, name);
}

const char* resonate_pal_library_extension(void)
{
#if defined(__APPLE__)
    return "dylib";
#else
    return "so";
#endif
}

} // extern "C"
