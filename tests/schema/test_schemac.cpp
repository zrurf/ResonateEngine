#include <catch2/catch_all.hpp>

#include <string>

#include "cli.h"

namespace
{

using resonate::test::runSchemac;
using resonate::test::schemacPath;
using resonate::test::scratchDirectory;
using resonate::test::writeTextFile;

/* Runs the CLI and fails with the whole invocation in the message when the
   exit code is not the expected one. */
resonate::test::ToolRun requireSchemac(const std::string& arguments, int expectedExit)
{
    const resonate::test::ToolRun run = runSchemac(arguments);
    INFO("schemac " << arguments << "\nexit " << run.exitCode << "\n" << run.output);
    REQUIRE(run.exitCode == expectedExit);
    return run;
}

} // namespace

TEST_CASE("the published compiler runs and reports its version", "[schema]")
{
    REQUIRE_FALSE(schemacPath().empty());

    const resonate::test::ToolRun run = requireSchemac("--version", 0);
    REQUIRE(run.output.rfind("schemac ", 0) == 0);
}

TEST_CASE("an invocation without a root is a usage error", "[schema]")
{
    const resonate::test::ToolRun run = requireSchemac("--out somewhere", 2);
    REQUIRE(run.output.find("usage: schemac") != std::string::npos);
}

TEST_CASE("every schema file under a root is discovered", "[schema]")
{
    const std::string root = scratchDirectory("discovery");
    writeTextFile(root + "/one.rschema", "<schema module=\"resonate.test.one\"/>\n");
    writeTextFile(root + "/nested/two.rschema", "<schema module=\"resonate.test.two\"/>\n");

    const resonate::test::ToolRun run =
        requireSchemac("--root \"" + root + "\" --out \"" + root + "/out\"", 0);
    REQUIRE(run.output.find("2 schema file(s)") != std::string::npos);
}

TEST_CASE("a root that is not a directory fails", "[schema]")
{
    const resonate::test::ToolRun run =
        requireSchemac("--root \"" + scratchDirectory("missing") + "/nope\" --out out", 1);
    REQUIRE(run.output.find("is not a directory") != std::string::npos);
}

TEST_CASE("a root without schema files fails", "[schema]")
{
    const std::string root = scratchDirectory("empty");

    const resonate::test::ToolRun run = requireSchemac("--root \"" + root + "\" --out out", 1);
    REQUIRE(run.output.find("no .rschema files found") != std::string::npos);
}
