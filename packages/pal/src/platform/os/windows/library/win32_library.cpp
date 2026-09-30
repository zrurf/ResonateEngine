#include <resonate/pal/library.h>

#include <new>
#include <string>

#include <windows.h>

#include "../status.h"

namespace
{

constexpr size_t PATH_CAPACITY = 1024;

bool toWide(const char* utf8, wchar_t* out, size_t capacity)
{
    if (utf8 == nullptr)
    {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, static_cast<int>(capacity)) > 0;
}

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_library_load(ResonateLibrary* out, const char* path)
{
    if (out == nullptr || path == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    out->handle = nullptr;

    wchar_t wide[PATH_CAPACITY] = {};
    if (!toWide(path, wide, PATH_CAPACITY))
    {
        return RESONATE_PAL_INVALID;
    }

    const HMODULE module = LoadLibraryW(wide);
    if (module == nullptr)
    {
        return resonate::pal::windows::statusFromLastError();
    }

    out->handle = module;
    return RESONATE_PAL_OK;
}

void resonate_pal_library_unload(ResonateLibrary* library)
{
    if (library == nullptr || library->handle == nullptr)
    {
        return;
    }
    FreeLibrary(static_cast<HMODULE>(library->handle));
    library->handle = nullptr;
}

void* resonate_pal_library_symbol(const ResonateLibrary* library, const char* name)
{
    if (library == nullptr || library->handle == nullptr || name == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library->handle), name));
}

const char* resonate_pal_library_extension(void)
{
    return "dll";
}

} // extern "C"
