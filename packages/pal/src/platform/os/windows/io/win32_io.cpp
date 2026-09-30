#include <resonate/pal/io.h>

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
    const int written = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, static_cast<int>(capacity));
    return written > 0;
}

void toUtf8(const wchar_t* wide, char* out, size_t capacity)
{
    if (wide == nullptr || capacity == 0U)
    {
        return;
    }
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(capacity),
                                            nullptr, nullptr);
    if (written <= 0)
    {
        out[0] = '\0';
    }
}

struct DirectoryEnumeration
{
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW entry = {};
    char name[PATH_CAPACITY] = {};
    bool first = true;
};

bool isDotEntry(const wchar_t* name)
{
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

/* Cached: the executable cannot move while it runs. */
const std::string& executablePath()
{
    static const std::string path = []
    {
        wchar_t wide[PATH_CAPACITY] = {};
        const DWORD length = GetModuleFileNameW(nullptr, wide, PATH_CAPACITY);
        if (length == 0U || length >= PATH_CAPACITY)
        {
            return std::string();
        }

        char utf8[PATH_CAPACITY * 2] = {};
        toUtf8(wide, utf8, sizeof(utf8));
        return std::string(utf8);
    }();
    return path;
}

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_io_open_file(ResonateFileHandle* out, const char* path,
                                            uint32_t mode)
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

    DWORD access = 0;
    if ((mode & RESONATE_FILE_READ) != 0U)
    {
        access |= GENERIC_READ;
    }
    if ((mode & (RESONATE_FILE_WRITE | RESONATE_FILE_APPEND)) != 0U)
    {
        access |= GENERIC_WRITE;
    }
    if (access == 0U)
    {
        return RESONATE_PAL_INVALID;
    }

    DWORD disposition = OPEN_EXISTING;
    if ((mode & RESONATE_FILE_CREATE) != 0U)
    {
        disposition = (mode & RESONATE_FILE_TRUNCATE) != 0U ? CREATE_ALWAYS : OPEN_ALWAYS;
    }
    else if ((mode & RESONATE_FILE_TRUNCATE) != 0U)
    {
        disposition = TRUNCATE_EXISTING;
    }

    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    if ((mode & RESONATE_FILE_OVERLAPPED) != 0U)
    {
        attributes |= FILE_FLAG_OVERLAPPED;
    }

    const HANDLE handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      disposition, attributes, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return resonate::pal::windows::statusFromLastError();
    }

    /* Append means the file opens at its end, not that later writes ignore the
       offset they name — FILE_APPEND_DATA would do that, and the event loop names
       an offset on every write. The POSIX backend opens at the end for the same
       reason. */
    if ((mode & RESONATE_FILE_APPEND) != 0U)
    {
        LARGE_INTEGER end = {};
        if (SetFilePointerEx(handle, LARGE_INTEGER{}, &end, FILE_END) == 0)
        {
            const ResonatePalStatus status = resonate::pal::windows::statusFromLastError();
            CloseHandle(handle);
            return status;
        }
    }

    out->handle = handle;
    return RESONATE_PAL_OK;
}

void resonate_pal_io_close_file(ResonateFileHandle* file)
{
    if (file == nullptr || file->handle == nullptr)
    {
        return;
    }
    CloseHandle(static_cast<HANDLE>(file->handle));
    file->handle = nullptr;
}

/* Split because one ReadFile transfers at most MAXDWORD bytes. A failure
   partway through still reports what it moved. */
ResonatePalStatus resonate_pal_io_read(ResonateFileHandle* file, void* buffer, uint64_t size,
                                       uint64_t* out_read)
{
    if (file == nullptr || file->handle == nullptr || buffer == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* cursor = static_cast<unsigned char*>(buffer);
    uint64_t total = 0;
    ResonatePalStatus status = RESONATE_PAL_OK;

    while (total < size)
    {
        const uint64_t remaining = size - total;
        const DWORD requested = remaining > MAXDWORD ? MAXDWORD : static_cast<DWORD>(remaining);

        DWORD read = 0;
        if (ReadFile(static_cast<HANDLE>(file->handle), cursor + total, requested, &read,
                     nullptr) == 0)
        {
            status = resonate::pal::windows::statusFromLastError();
            break;
        }

        total += read;
        if (read < requested)
        {
            break;
        }
    }

    if (out_read != nullptr)
    {
        *out_read = total;
    }
    return status;
}

ResonatePalStatus resonate_pal_io_write(ResonateFileHandle* file, const void* buffer, uint64_t size,
                                        uint64_t* out_written)
{
    if (file == nullptr || file->handle == nullptr || buffer == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    const auto* cursor = static_cast<const unsigned char*>(buffer);
    uint64_t total = 0;
    ResonatePalStatus status = RESONATE_PAL_OK;

    while (total < size)
    {
        const uint64_t remaining = size - total;
        const DWORD requested = remaining > MAXDWORD ? MAXDWORD : static_cast<DWORD>(remaining);

        DWORD written = 0;
        if (WriteFile(static_cast<HANDLE>(file->handle), cursor + total, requested, &written,
                      nullptr) == 0)
        {
            status = resonate::pal::windows::statusFromLastError();
            break;
        }

        total += written;
        if (written < requested)
        {
            break;
        }
    }

    if (out_written != nullptr)
    {
        *out_written = total;
    }
    return status;
}

ResonatePalStatus resonate_pal_io_seek(ResonateFileHandle* file, int64_t offset,
                                       ResonateSeekOrigin origin, uint64_t* out_offset)
{
    if (file == nullptr || file->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    DWORD method = FILE_BEGIN;
    if (origin == RESONATE_SEEK_CURRENT)
    {
        method = FILE_CURRENT;
    }
    else if (origin == RESONATE_SEEK_END)
    {
        method = FILE_END;
    }

    LARGE_INTEGER distance = {};
    distance.QuadPart = offset;
    LARGE_INTEGER position = {};
    if (SetFilePointerEx(static_cast<HANDLE>(file->handle), distance, &position, method) == 0)
    {
        return resonate::pal::windows::statusFromLastError();
    }

    if (out_offset != nullptr)
    {
        *out_offset = static_cast<uint64_t>(position.QuadPart);
    }
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_tell(ResonateFileHandle* file, uint64_t* out_offset)
{
    return resonate_pal_io_seek(file, 0, RESONATE_SEEK_CURRENT, out_offset);
}

ResonatePalStatus resonate_pal_io_flush(ResonateFileHandle* file)
{
    if (file == nullptr || file->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    if (FlushFileBuffers(static_cast<HANDLE>(file->handle)) == 0)
    {
        return resonate::pal::windows::statusFromLastError();
    }
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_size(ResonateFileHandle* file, uint64_t* out_size)
{
    if (file == nullptr || file->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    LARGE_INTEGER size = {};
    if (GetFileSizeEx(static_cast<HANDLE>(file->handle), &size) == 0)
    {
        return resonate::pal::windows::statusFromLastError();
    }

    if (out_size != nullptr)
    {
        *out_size = static_cast<uint64_t>(size.QuadPart);
    }
    return RESONATE_PAL_OK;
}

uint32_t resonate_pal_io_exists(const char* path)
{
    if (path == nullptr)
    {
        return 0;
    }
    wchar_t wide[PATH_CAPACITY] = {};
    if (!toWide(path, wide, PATH_CAPACITY))
    {
        return 0;
    }
    return GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES ? 1U : 0U;
}

uint32_t resonate_pal_io_is_directory(const char* path)
{
    if (path == nullptr)
    {
        return 0;
    }
    wchar_t wide[PATH_CAPACITY] = {};
    if (!toWide(path, wide, PATH_CAPACITY))
    {
        return 0;
    }
    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return 0;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ? 1U : 0U;
}

ResonatePalStatus resonate_pal_io_remove(const char* path)
{
    if (path == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    wchar_t wide[PATH_CAPACITY] = {};
    if (!toWide(path, wide, PATH_CAPACITY))
    {
        return RESONATE_PAL_INVALID;
    }

    const DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return resonate::pal::windows::statusFromLastError();
    }

    const BOOL removed =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ? RemoveDirectoryW(wide) : DeleteFileW(wide);
    return removed != 0 ? RESONATE_PAL_OK : resonate::pal::windows::statusFromLastError();
}

ResonatePalStatus resonate_pal_io_rename(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    wchar_t wide_from[PATH_CAPACITY] = {};
    wchar_t wide_to[PATH_CAPACITY] = {};
    if (!toWide(from, wide_from, PATH_CAPACITY) || !toWide(to, wide_to, PATH_CAPACITY))
    {
        return RESONATE_PAL_INVALID;
    }

    if (MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING) == 0)
    {
        return resonate::pal::windows::statusFromLastError();
    }
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_create_directory(const char* path)
{
    if (path == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    wchar_t wide[PATH_CAPACITY] = {};
    if (!toWide(path, wide, PATH_CAPACITY))
    {
        return RESONATE_PAL_INVALID;
    }

    if (CreateDirectoryW(wide, nullptr) == 0)
    {
        return resonate::pal::windows::statusFromLastError();
    }
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_enumerate(ResonateDirHandle* out, const char* path)
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

    std::wstring pattern(wide);
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
    {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    auto* enumeration = new (std::nothrow) DirectoryEnumeration();
    if (enumeration == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    enumeration->find = FindFirstFileW(pattern.c_str(), &enumeration->entry);
    if (enumeration->find == INVALID_HANDLE_VALUE)
    {
        delete enumeration;
        return resonate::pal::windows::statusFromLastError();
    }

    out->handle = enumeration;
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_enumerate_next(ResonateDirHandle* dir, const char** out_name)
{
    if (dir == nullptr || dir->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* enumeration = static_cast<DirectoryEnumeration*>(dir->handle);

    /* FindFirstFile already holds the first entry, so the first call must not
       advance past it. */
    if (!enumeration->first)
    {
        if (FindNextFileW(enumeration->find, &enumeration->entry) == 0)
        {
            return resonate::pal::windows::statusFromLastError();
        }
    }
    enumeration->first = false;

    if (isDotEntry(enumeration->entry.cFileName))
    {
        return resonate_pal_io_enumerate_next(dir, out_name);
    }

    toUtf8(enumeration->entry.cFileName, enumeration->name, sizeof(enumeration->name));
    if (out_name != nullptr)
    {
        *out_name = enumeration->name;
    }
    return RESONATE_PAL_OK;
}

void resonate_pal_io_close_directory(ResonateDirHandle* dir)
{
    if (dir == nullptr || dir->handle == nullptr)
    {
        return;
    }

    auto* enumeration = static_cast<DirectoryEnumeration*>(dir->handle);
    FindClose(enumeration->find);
    delete enumeration;
    dir->handle = nullptr;
}

const char* resonate_pal_io_executable_path(void)
{
    return executablePath().c_str();
}

const char* resonate_pal_io_executable_directory(void)
{
    static const std::string directory = []
    {
        std::string path = executablePath();
        const size_t separator = path.find_last_of("\\/");
        return separator == std::string::npos ? std::string() : path.substr(0, separator);
    }();
    return directory.c_str();
}

} // extern "C"
