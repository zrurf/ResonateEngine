#include "cli.h"

#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <resonate/pal/io.h>

#include "paths.h"

#if !defined(_WIN32)
#    include <sys/wait.h>
#endif

namespace resonate::test
{
namespace
{

#if defined(_WIN32)
constexpr const char* kToolName = "schemac.exe";
#else
constexpr const char* kToolName = "schemac";
#endif

/* system() hands back the wait status on POSIX and the exit code on Windows. */
int exitCodeOf(int status)
{
#if defined(_WIN32)
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

std::string schemacPath()
{
    const std::string directory = findBuildDirectory("tools");
    if (directory.empty())
    {
        return {};
    }
    return directory + "/schemac/publish/" + kToolName;
}

std::string scratchDirectory(const std::string& name)
{
    const std::filesystem::path path =
        std::filesystem::path(resonate_pal_io_executable_directory()) / "schemac-scratch" / name;

    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    INFO("scratch directory " << path.string() << ": " << error.message());
    REQUIRE_FALSE(error);
    return path.string();
}

void writeTextFile(const std::string& path, const std::string& content)
{
    const std::filesystem::path file(path);
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);

    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    INFO("writing " << path);
    REQUIRE(stream.good());
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(stream.good());
}

std::string readTextFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {};
    }

    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

bool fileContains(const std::string& path, const std::string& fragment)
{
    return readTextFile(path).find(fragment) != std::string::npos;
}

ToolRun runSchemac(const std::string& arguments)
{
    ToolRun run;

    const std::string tool = schemacPath();
    if (tool.empty() || !std::filesystem::exists(tool))
    {
        run.output = "the schema compiler is not published under build/tools";
        return run;
    }

    const std::string log = scratchDirectory("logs") + "/run.log";

    /* cmd.exe strips the first and the last quote of a /c command line that
       starts with a quote, which would corrupt the program path and the
       redirect; wrapping the whole line in its own pair of quotes is the
       documented way to preserve it. A POSIX shell needs no such dance. */
#if defined(_WIN32)
    const std::string command = "\"\"" + tool + "\" " + arguments + " > \"" + log + "\" 2>&1\"";
#else
    const std::string command = "\"" + tool + "\" " + arguments + " > \"" + log + "\" 2>&1";
#endif

    run.exitCode = exitCodeOf(std::system(command.c_str()));
    run.output = readTextFile(log);
    return run;
}

} // namespace resonate::test
