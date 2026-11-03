#include <resonate/pal/watch.h>

#include <new>
#include <string>

#include <windows.h>

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

struct WatchState
{
    HANDLE notification = nullptr;
};

} // namespace

ResonatePalStatus resonate_pal_watch_create(ResonateWatchHandle* out, const char* file_path)
{
    if (out == nullptr || file_path == nullptr || *file_path == '\0')
    {
        return RESONATE_PAL_INVALID;
    }
    out->handle = nullptr;

    /* The directory, not the file: the notification covers create, delete,
       rename and write, and survives an editor's replace-by-rename. */
    const std::string path(file_path);
    const std::size_t separator = path.find_last_of("/\\");
    std::string directory = separator == std::string::npos ? std::string(".") : path.substr(0U, separator);
    if (directory.empty())
    {
        directory = "/";
    }
    wchar_t wide[PATH_CAPACITY] = {};
    if (!toWide(directory.c_str(), wide, PATH_CAPACITY))
    {
        return RESONATE_PAL_INVALID;
    }

    const HANDLE notification =
        FindFirstChangeNotificationW(wide, FALSE,
                                     FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                         FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_CREATION);
    if (notification == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? RESONATE_PAL_NOT_FOUND
                                                      : RESONATE_PAL_IO;
    }

    auto* state = new (std::nothrow) WatchState();
    if (state == nullptr)
    {
        FindCloseChangeNotification(notification);
        return RESONATE_PAL_OUT_OF_MEMORY;
    }
    state->notification = notification;
    out->handle = state;
    return RESONATE_PAL_OK;
}

int32_t resonate_pal_watch_poll(ResonateWatchHandle* watch)
{
    if (watch == nullptr || watch->handle == nullptr)
    {
        return -1;
    }
    auto* state = static_cast<WatchState*>(watch->handle);
    const DWORD status = WaitForSingleObject(state->notification, 0U);
    if (status == WAIT_TIMEOUT)
    {
        return 0;
    }
    if (status != WAIT_OBJECT_0)
    {
        return -1;
    }
    /* Re-arm before answering: a change between the wait and the re-arm is
       still reported, because the state stays signaled until the next event
       is armed. */
    if (FindNextChangeNotification(state->notification) == FALSE)
    {
        return -1;
    }
    return 1;
}

void resonate_pal_watch_close(ResonateWatchHandle* watch)
{
    if (watch == nullptr)
    {
        return;
    }
    if (watch->handle != nullptr)
    {
        auto* state = static_cast<WatchState*>(watch->handle);
        FindCloseChangeNotification(state->notification);
        delete state;
        watch->handle = nullptr;
    }
}
