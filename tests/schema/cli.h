#ifndef RESONATE_TESTS_SCHEMA_CLI_H
#define RESONATE_TESTS_SCHEMA_CLI_H

#include <string>

namespace resonate::test
{

/* What one schemac invocation reported: its exit code and everything it wrote
   to stdout and stderr. */
struct ToolRun
{
    int exitCode = -1;
    std::string output;
};

/* Runs the published compiler, so the tests exercise the shipped CLI rather
   than a linked-in copy. `arguments` is the command line after the executable;
   the caller quotes paths that may contain spaces. The build's schema rule
   publishes the tool before the tests run. */
ToolRun runSchemac(const std::string& arguments);

/* The published CLI; empty when nothing published it. */
std::string schemacPath();

/* A writable directory under the build tree, emptied on each call, for the
   inputs a case writes and the outputs the tool leaves. */
std::string scratchDirectory(const std::string& name);

/* Creates the file and any missing parent directories, then writes the text. */
void writeTextFile(const std::string& path, const std::string& content);

/* Reads a file the tool wrote; empty when it does not exist. */
std::string readTextFile(const std::string& path);

/* Whether the file was written and contains `fragment`. */
bool fileContains(const std::string& path, const std::string& fragment);

} // namespace resonate::test

#endif /* RESONATE_TESTS_SCHEMA_CLI_H */
