#include <resonate/pal/watch.h>

#include <cerrno>
#include <cstddef>
#include <new>
#include <string>

#if defined(__linux__)
#    include <sys/inotify.h>
#    include <unistd.h>
#endif

namespace
{

#if defined(__linux__)
struct WatchState
{
    int descriptor = -1;
    int watch = -1;
};
#endif

} // namespace

ResonatePalStatus resonate_pal_watch_create(ResonateWatchHandle* out, const char* file_path)
{
    if (out == nullptr || file_path == nullptr || *file_path == '\0')
    {
        return RESONATE_PAL_INVALID;
    }
    out->handle = nullptr;

#if defined(__linux__)
    /* The directory, not the file: the notification covers create, delete,
       rename and write, and survives an editor's replace-by-rename. */
    const std::string path(file_path);
    const std::size_t separator = path.find_last_of('/');
    const std::string directory =
        separator == std::string::npos ? std::string(".")
                                      : (separator == 0U ? std::string("/")
                                                         : path.substr(0U, separator));

    const int descriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (descriptor < 0)
    {
        return errno == EMFILE ? RESONATE_PAL_EXHAUSTED : RESONATE_PAL_IO;
    }
    const int watch = inotify_add_watch(
        descriptor, directory.c_str(),
        IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
            IN_DELETE);
    if (watch < 0)
    {
        const int error = errno;
        close(descriptor);
        return error == ENOENT ? RESONATE_PAL_NOT_FOUND
                               : (error == EACCES ? RESONATE_PAL_ACCESS : RESONATE_PAL_IO);
    }

    auto* state = new (std::nothrow) WatchState();
    if (state == nullptr)
    {
        inotify_rm_watch(descriptor, watch);
        close(descriptor);
        return RESONATE_PAL_OUT_OF_MEMORY;
    }
    state->descriptor = descriptor;
    state->watch = watch;
    out->handle = state;
    return RESONATE_PAL_OK;
#else
    /* macOS and the BSDs have no inotify; a kqueue backend is the later
       slice, and until then hot reload simply never fires there. */
    (void)file_path;
    return RESONATE_PAL_UNSUPPORTED;
#endif
}

int32_t resonate_pal_watch_poll(ResonateWatchHandle* watch)
{
    if (watch == nullptr || watch->handle == nullptr)
    {
        return -1;
    }
#if defined(__linux__)
    auto* state = static_cast<WatchState*>(watch->handle);
    /* Drain everything pending: the events themselves do not matter (the
       caller compares timestamps), only whether anything happened. */
    alignas(struct inotify_event) char buffer[4096];
    bool changed = false;
    for (;;)
    {
        const ssize_t read_size = read(state->descriptor, buffer, sizeof(buffer));
        if (read_size > 0)
        {
            changed = true;
            continue;
        }
        if (read_size < 0 && errno == EAGAIN)
        {
            break;
        }
        if (read_size < 0 && errno == EINTR)
        {
            continue;
        }
        /* A failed read (the watch went away) reports itself once. */
        return changed ? 1 : -1;
    }
    return changed ? 1 : 0;
#else
    return 0;
#endif
}

void resonate_pal_watch_close(ResonateWatchHandle* watch)
{
    if (watch == nullptr)
    {
        return;
    }
#if defined(__linux__)
    if (watch->handle != nullptr)
    {
        auto* state = static_cast<WatchState*>(watch->handle);
        if (state->watch >= 0)
        {
            inotify_rm_watch(state->descriptor, state->watch);
        }
        if (state->descriptor >= 0)
        {
            close(state->descriptor);
        }
        delete state;
        watch->handle = nullptr;
    }
#else
    watch->handle = nullptr;
#endif
}
