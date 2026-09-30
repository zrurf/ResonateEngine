#include <resonate/startup/startup.hpp>

#ifdef RESONATE_PLATFORM_WINDOWS
#    include <Windows.h>
#endif

#ifdef RESONATE_PLATFORM_WINDOWS
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return resonate::startup::run(resonate::startup::commandLine(), "Resonate Editor");
}
#else
int main(int argc, char** argv)
{
    return resonate::startup::run(resonate::startup::commandLine(argc, argv), "Resonate Editor");
}
#endif
