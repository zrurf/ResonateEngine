#ifndef RESONATE_TESTS_PATHS_H
#define RESONATE_TESTS_PATHS_H

#include <string>

#include <resonate/pal/io.h>

namespace resonate::test
{

/* Build output is laid out under build/<platform>/<arch>/<mode>/ while the
   plugin directories sit directly under build/, so they are found by walking up
   rather than by a fixed relative path. */
inline std::string findBuildDirectory(const char* name)
{
    std::string directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 5; ++level)
    {
        const std::string candidate = directory + "/" + name;
        if (resonate_pal_io_is_directory(candidate.c_str()) != 0U)
        {
            return candidate;
        }

        const std::size_t separator = directory.find_last_of("/\\");
        if (separator == std::string::npos)
        {
            break;
        }
        directory.erase(separator);
    }
    return {};
}

} // namespace resonate::test

#endif /* RESONATE_TESTS_PATHS_H */
