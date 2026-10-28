#include "resonate/startup/startup.hpp"

#ifdef RESONATE_PLATFORM_WINDOWS

#include <cstdio>
#include <ctime>

#include <Windows.h>
#include <dbghelp.h>

namespace resonate::startup
{
namespace
{

/* The dump lands beside the executable: a dev run finds it next to the
   binaries it just built, and the installed tree writes next to itself. */
void dumpPath(wchar_t* out, std::size_t capacity)
{
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::time_t now = {};
    std::time(&now);
    std::tm local = {};
    localtime_s(&local, &now);
    wchar_t drive[_MAX_DRIVE] = {};
    wchar_t directory[_MAX_DIR] = {};
    _wsplitpath_s(executable, drive, _MAX_DRIVE, directory, _MAX_DIR, nullptr, 0, nullptr, 0);
    _snwprintf_s(out, capacity, _TRUNCATE, L"%s%sresonate-crash-%04d%02d%02d-%02d%02d%02d.dmp",
                 drive, directory, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
}

LONG WINAPI writeMinidump(EXCEPTION_POINTERS* information)
{
    wchar_t path[MAX_PATH] = {};
    dumpPath(path, MAX_PATH);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), information, FALSE};
        if (!MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                               MiniDumpNormal, &exception, nullptr, nullptr))
        {
            std::fprintf(stderr, "resonate: the crash minidump failed (%lu)\n",
                         static_cast<unsigned long>(GetLastError()));
        }
        CloseHandle(file);
    }
    else
    {
        std::fprintf(stderr, "resonate: the crash minidump could not be created at %ls (%lu)\n",
                     path, static_cast<unsigned long>(GetLastError()));
    }
    std::fflush(stderr);
    /* The filter's answer ends the process with the fault; the dump is on
       disk before it does. */
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void installCrashHandler()
{
    SetUnhandledExceptionFilter(&writeMinidump);
    /* The dump path is the part of the handler a test can exercise without a
       real fault: raise after the install and look for the file. */
    if (std::getenv("RESONATE_CRASH_TEST") != nullptr)
    {
        RaiseException(0xE0000001U, EXCEPTION_NONCONTINUABLE, 0U, nullptr);
    }
}

} // namespace resonate::startup

#endif /* RESONATE_PLATFORM_WINDOWS */
