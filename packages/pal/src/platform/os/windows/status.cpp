#include "status.h"

#include <windows.h>

namespace resonate::pal::windows
{

ResonatePalStatus statusFromWin32(unsigned long error)
{
    switch (error)
    {
        case ERROR_SUCCESS:
            return RESONATE_PAL_OK;

        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_NOT_READY:
        case ERROR_SEEK_ON_DEVICE:
            return RESONATE_PAL_NOT_FOUND;

        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return RESONATE_PAL_ALREADY_EXISTS;

        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_WRITE_PROTECT:
        case ERROR_SHARING_VIOLATION:
            return RESONATE_PAL_ACCESS;

        case ERROR_INVALID_HANDLE:
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_NEGATIVE_SEEK:
            return RESONATE_PAL_INVALID;

        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return RESONATE_PAL_OUT_OF_MEMORY;

        case ERROR_IO_PENDING:
        case ERROR_IO_INCOMPLETE:
            return RESONATE_PAL_AGAIN;

        case ERROR_TIMEOUT:
        case WAIT_TIMEOUT:
            return RESONATE_PAL_TIMEOUT;

        case ERROR_NO_MORE_FILES:
            return RESONATE_PAL_EXHAUSTED;

        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
        case ERROR_INVALID_FUNCTION:
            return RESONATE_PAL_UNSUPPORTED;

        case ERROR_OPERATION_ABORTED:
        case ERROR_CRC:
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_SECTOR_NOT_FOUND:
            return RESONATE_PAL_IO;

        default:
            return RESONATE_PAL_UNKNOWN;
    }
}

ResonatePalStatus statusFromLastError()
{
    return statusFromWin32(GetLastError());
}

} // namespace resonate::pal::windows
