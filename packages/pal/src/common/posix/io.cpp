#include <resonate/pal/io.h>

#include <cerrno>
#include <cstring>
#include <new>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#    include <mach-o/dyld.h>
#endif

#include "file_descriptor.h"
#include "status.h"

namespace
{

constexpr size_t PATH_CAPACITY = 4096;

struct DirectoryEnumeration
{
    DIR* directory = nullptr;
    char name[PATH_CAPACITY] = {};
};

const std::string& executablePath()
{
    static const std::string path = []
    {
        char buffer[PATH_CAPACITY] = {};

#if defined(__APPLE__)
        uint32_t size = static_cast<uint32_t>(sizeof(buffer));
        if (_NSGetExecutablePath(buffer, &size) != 0)
        {
            return std::string();
        }
#else
        const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1U);
        if (length <= 0)
        {
            return std::string();
        }
        buffer[length] = '\0';
#endif

        return std::string(buffer);
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

    const bool reading = (mode & RESONATE_FILE_READ) != 0U;
    const bool writing = (mode & (RESONATE_FILE_WRITE | RESONATE_FILE_APPEND)) != 0U;
    if (!reading && !writing)
    {
        return RESONATE_PAL_INVALID;
    }

    int flags = O_RDONLY;
    if (reading && writing)
    {
        flags = O_RDWR;
    }
    else if (writing)
    {
        flags = O_WRONLY;
    }
    if ((mode & RESONATE_FILE_CREATE) != 0U)
    {
        flags |= O_CREAT;
    }
    if ((mode & RESONATE_FILE_TRUNCATE) != 0U)
    {
        flags |= O_TRUNC;
    }

    const int descriptor = open(path, flags, 0644);
    if (descriptor < 0)
    {
        return resonate::pal::posix::statusFromLastErrno();
    }

    /* Append positions the file at its end instead of setting O_APPEND: Linux
       appends even a write that names an offset, other kernels write where they
       are told, and the event loop always names one. Opening at the end leaves
       every write taking its own offset, which is what the Windows backend does
       and therefore what the layer as a whole promises. */
    if ((mode & RESONATE_FILE_APPEND) != 0U && lseek(descriptor, 0, SEEK_END) < 0)
    {
        const ResonatePalStatus status = resonate::pal::posix::statusFromLastErrno();
        close(descriptor);
        return status;
    }

    out->handle = resonate::pal::posix::handleFromDescriptor(descriptor);
    return RESONATE_PAL_OK;
}

void resonate_pal_io_close_file(ResonateFileHandle* file)
{
    if (file == nullptr || file->handle == nullptr)
    {
        return;
    }
    close(resonate::pal::posix::descriptorFromHandle(file));
    file->handle = nullptr;
}

/* Driven to completion: read and write transfer less than asked on occasion, and
   EINTR is retried. A failure partway through still reports what it moved. */
ResonatePalStatus resonate_pal_io_read(ResonateFileHandle* file, void* buffer, uint64_t size,
                                       uint64_t* out_read)
{
    if (file == nullptr || file->handle == nullptr || buffer == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    const int descriptor = resonate::pal::posix::descriptorFromHandle(file);
    auto* cursor = static_cast<unsigned char*>(buffer);
    uint64_t total = 0;
    ResonatePalStatus status = RESONATE_PAL_OK;

    while (total < size)
    {
        const ssize_t read_now = read(descriptor, cursor + total, size - total);
        if (read_now < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            status = resonate::pal::posix::statusFromLastErrno();
            break;
        }
        if (read_now == 0)
        {
            break;
        }
        total += static_cast<uint64_t>(read_now);
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

    const int descriptor = resonate::pal::posix::descriptorFromHandle(file);
    const auto* cursor = static_cast<const unsigned char*>(buffer);
    uint64_t total = 0;
    ResonatePalStatus status = RESONATE_PAL_OK;

    while (total < size)
    {
        const ssize_t written_now = write(descriptor, cursor + total, size - total);
        if (written_now < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            status = resonate::pal::posix::statusFromLastErrno();
            break;
        }
        if (written_now == 0)
        {
            /* No progress is possible; looping again would not change that. */
            break;
        }
        total += static_cast<uint64_t>(written_now);
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

    int whence = SEEK_SET;
    if (origin == RESONATE_SEEK_CURRENT)
    {
        whence = SEEK_CUR;
    }
    else if (origin == RESONATE_SEEK_END)
    {
        whence = SEEK_END;
    }

    const off_t position =
        lseek(resonate::pal::posix::descriptorFromHandle(file), static_cast<off_t>(offset), whence);
    if (position < 0)
    {
        return resonate::pal::posix::statusFromLastErrno();
    }

    if (out_offset != nullptr)
    {
        *out_offset = static_cast<uint64_t>(position);
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
    if (fsync(resonate::pal::posix::descriptorFromHandle(file)) != 0)
    {
        return resonate::pal::posix::statusFromLastErrno();
    }
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_size(ResonateFileHandle* file, uint64_t* out_size)
{
    if (file == nullptr || file->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    struct stat information = {};
    if (fstat(resonate::pal::posix::descriptorFromHandle(file), &information) != 0)
    {
        return resonate::pal::posix::statusFromLastErrno();
    }

    if (out_size != nullptr)
    {
        *out_size = static_cast<uint64_t>(information.st_size);
    }
    return RESONATE_PAL_OK;
}

uint32_t resonate_pal_io_exists(const char* path)
{
    return path != nullptr && access(path, F_OK) == 0 ? 1U : 0U;
}

uint32_t resonate_pal_io_is_directory(const char* path)
{
    if (path == nullptr)
    {
        return 0;
    }

    struct stat information = {};
    if (stat(path, &information) != 0)
    {
        return 0;
    }
    return S_ISDIR(information.st_mode) ? 1U : 0U;
}

ResonatePalStatus resonate_pal_io_remove(const char* path)
{
    if (path == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    const int removed = resonate_pal_io_is_directory(path) != 0U ? rmdir(path) : unlink(path);
    return removed == 0 ? RESONATE_PAL_OK : resonate::pal::posix::statusFromLastErrno();
}

ResonatePalStatus resonate_pal_io_rename(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    if (rename(from, to) != 0)
    {
        return resonate::pal::posix::statusFromLastErrno();
    }
    return RESONATE_PAL_OK;
}

ResonatePalStatus resonate_pal_io_create_directory(const char* path)
{
    if (path == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    if (mkdir(path, 0755) != 0)
    {
        return resonate::pal::posix::statusFromLastErrno();
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

    auto* enumeration = new (std::nothrow) DirectoryEnumeration();
    if (enumeration == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    enumeration->directory = opendir(path);
    if (enumeration->directory == nullptr)
    {
        const ResonatePalStatus status = resonate::pal::posix::statusFromLastErrno();
        delete enumeration;
        return status;
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

    while (true)
    {
        errno = 0;
        const struct dirent* entry = readdir(enumeration->directory);
        if (entry == nullptr)
        {
            return errno == 0 ? RESONATE_PAL_EXHAUSTED
                              : resonate::pal::posix::statusFromLastErrno();
        }
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
        {
            continue;
        }

        std::strncpy(enumeration->name, entry->d_name, sizeof(enumeration->name) - 1U);
        enumeration->name[sizeof(enumeration->name) - 1U] = '\0';
        if (out_name != nullptr)
        {
            *out_name = enumeration->name;
        }
        return RESONATE_PAL_OK;
    }
}

void resonate_pal_io_close_directory(ResonateDirHandle* dir)
{
    if (dir == nullptr || dir->handle == nullptr)
    {
        return;
    }

    auto* enumeration = static_cast<DirectoryEnumeration*>(dir->handle);
    closedir(enumeration->directory);
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
        const std::string path = executablePath();
        const size_t separator = path.find_last_of('/');
        return separator == std::string::npos ? std::string() : path.substr(0, separator);
    }();
    return directory.c_str();
}

} // extern "C"
