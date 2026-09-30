#include "status.h"

#include <cerrno>

namespace resonate::pal::posix
{

ResonatePalStatus statusFromErrno(int error)
{
    switch (error)
    {
        case 0:
            return RESONATE_PAL_OK;

        case ENOENT:
        case ENODEV:
        case ENXIO:
            return RESONATE_PAL_NOT_FOUND;

        case EEXIST:
            return RESONATE_PAL_ALREADY_EXISTS;

        case EACCES:
        case EPERM:
        case EROFS:
        case ETXTBSY:
            return RESONATE_PAL_ACCESS;

        case EBADF:
        case EINVAL:
        case ENAMETOOLONG:
        case EISDIR:
        case ENOTDIR:
        case ELOOP:
        case EFAULT:
            return RESONATE_PAL_INVALID;

        case ENOMEM:
            return RESONATE_PAL_OUT_OF_MEMORY;

        case EAGAIN:
            return RESONATE_PAL_AGAIN;

#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
            return RESONATE_PAL_AGAIN;
#endif

        case EINPROGRESS:
        case EBUSY:
            return RESONATE_PAL_AGAIN;

        case ETIMEDOUT:
            return RESONATE_PAL_TIMEOUT;

        case ENOSPC:
        case EDQUOT:
            return RESONATE_PAL_IO;

        case ENOTSUP:
            return RESONATE_PAL_UNSUPPORTED;

#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP:
            return RESONATE_PAL_UNSUPPORTED;
#endif

        case ENOSYS:
            return RESONATE_PAL_UNSUPPORTED;

        case EIO:
            return RESONATE_PAL_IO;

        default:
            return RESONATE_PAL_UNKNOWN;
    }
}

ResonatePalStatus statusFromLastErrno()
{
    return statusFromErrno(errno);
}

} // namespace resonate::pal::posix
