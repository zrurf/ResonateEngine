#ifndef RESONATE_PAL_IO_H
#define RESONATE_PAL_IO_H

/*
 * File and directory access. Paths are UTF-8 on every platform; conversion to
 * the native encoding happens in the backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateFileHandle
{
    void* handle;
} ResonateFileHandle;

typedef struct ResonateDirHandle
{
    void* handle;
} ResonateDirHandle;

typedef enum ResonateSeekOrigin : uint8_t
{
    RESONATE_SEEK_BEGIN = 0,
    RESONATE_SEEK_CURRENT = 1,
    RESONATE_SEEK_END = 2
} ResonateSeekOrigin;

/* Opens the file for the event loop instead of for resonate_pal_io_read; on
   Windows that is FILE_FLAG_OVERLAPPED. Ignored where there is no such
   distinction. */
typedef enum ResonateFileMode : uint32_t
{
    RESONATE_FILE_READ = 1 << 0,
    RESONATE_FILE_WRITE = 1 << 1,

    /* Opens the file positioned at its end, so a write that names no offset of its
       own continues there. It does not make a write that names one append: every
       write lands where it is told to, which is what keeps an event-loop
       submission meaning the same thing on every backend. */
    RESONATE_FILE_APPEND = 1 << 2,

    RESONATE_FILE_TRUNCATE = 1 << 3,
    RESONATE_FILE_CREATE = 1 << 4,
    RESONATE_FILE_OVERLAPPED = 1 << 5
} ResonateFileMode;

/* A file opened with RESONATE_FILE_OVERLAPPED must not be read or written
   through this layer; its reads and writes go through the event loop. */
ResonatePalStatus resonate_pal_io_open_file(ResonateFileHandle* out, const char* path,
                                            uint32_t mode);
void resonate_pal_io_close_file(ResonateFileHandle* file);

/* A short transfer at end of file is not an error. out_read and out_written are
   written on every path that reaches the transfer, a failing one included, so
   what a partial transfer moved is visible beside the status. */
ResonatePalStatus resonate_pal_io_read(ResonateFileHandle* file, void* buffer, uint64_t size,
                                       uint64_t* out_read);
ResonatePalStatus resonate_pal_io_write(ResonateFileHandle* file, const void* buffer, uint64_t size,
                                        uint64_t* out_written);
ResonatePalStatus resonate_pal_io_seek(ResonateFileHandle* file, int64_t offset,
                                       ResonateSeekOrigin origin, uint64_t* out_offset);
ResonatePalStatus resonate_pal_io_tell(ResonateFileHandle* file, uint64_t* out_offset);
ResonatePalStatus resonate_pal_io_flush(ResonateFileHandle* file);
ResonatePalStatus resonate_pal_io_size(ResonateFileHandle* file, uint64_t* out_size);

/* Predicates: 1 or 0, never an error code. */
uint32_t resonate_pal_io_exists(const char* path);
uint32_t resonate_pal_io_is_directory(const char* path);

ResonatePalStatus resonate_pal_io_remove(const char* path);
ResonatePalStatus resonate_pal_io_rename(const char* from, const char* to);
ResonatePalStatus resonate_pal_io_create_directory(const char* path);

/* enumerate_next returns RESONATE_PAL_EXHAUSTED at the end, which is how a
   caller ends the loop. The returned name is borrowed from the handle and
   stays valid until the next call on it. */
ResonatePalStatus resonate_pal_io_enumerate(ResonateDirHandle* out, const char* path);
ResonatePalStatus resonate_pal_io_enumerate_next(ResonateDirHandle* dir, const char** out_name);
void resonate_pal_io_close_directory(ResonateDirHandle* dir);

/* Resolved once at startup and owned by the platform layer. */
const char* resonate_pal_io_executable_path(void);
const char* resonate_pal_io_executable_directory(void);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_IO_H */
