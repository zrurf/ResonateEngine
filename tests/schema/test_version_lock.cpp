#include <catch2/catch_all.hpp>

#include <string>

#include "cli.h"

namespace
{

using resonate::test::ToolRun;

/* One module's worth of declarations plus the two lock operations. */
struct LockCase
{
    std::string root;

    explicit LockCase(const std::string& name) : root(resonate::test::scratchDirectory(name))
    {
    }

    void write(const std::string& content) const
    {
        resonate::test::writeTextFile(root + "/case.rschema", content);
    }

    ToolRun check() const
    {
        return run("--check-lock");
    }
    ToolRun update() const
    {
        return run("--update-lock");
    }

    std::string lock() const
    {
        return resonate::test::readTextFile(root + "/schemas.lock.json");
    }

  private:
    ToolRun run(const std::string& mode) const
    {
        return resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root + "/gen\" " +
                                          mode);
    }
};

const char* const kHealthV1 = R"xml(<schema module="resonate.test">
  <enum name="Flags" width="u8">
    <value name="Idle"/>
    <value name="Busy"/>
  </enum>

  <component name="Health" version="1">
    <field name="current" type="i32" min="0" default="100"/>
    <field name="flags" type="Flags"/>
  </component>
</schema>
)xml";

const char* const kHealthV2 = R"xml(<schema module="resonate.test">
  <enum name="Flags" width="u8">
    <value name="Idle"/>
    <value name="Busy"/>
  </enum>

  <component name="Health" version="2">
    <field name="current" type="i32" min="0" default="100"/>
    <field name="flags" type="Flags"/>
    <migrate from="1" to="2"/>
  </component>
</schema>
)xml";

/* The same component with one field retyped: what a saved payload would be read
   as changes while the version says nothing did. */
const char* const kHealthV1Wide = R"xml(<schema module="resonate.test">
  <enum name="Flags" width="u8">
    <value name="Idle"/>
    <value name="Busy"/>
  </enum>

  <component name="Health" version="1">
    <field name="current" type="i64" min="0" default="100"/>
    <field name="flags" type="Flags"/>
  </component>
</schema>
)xml";

/* The same component with one more enum value: its own layout does not move,
   but what the stored numbers mean does. */
const char* const kHealthV1MoreFlags = R"xml(<schema module="resonate.test">
  <enum name="Flags" width="u8">
    <value name="Idle"/>
    <value name="Busy"/>
    <value name="Frozen"/>
  </enum>

  <component name="Health" version="1">
    <field name="current" type="i32" min="0" default="100"/>
    <field name="flags" type="Flags"/>
  </component>
</schema>
)xml";

void requireProblem(const ToolRun& run, const std::string& fragment)
{
    INFO(run.output);
    REQUIRE(run.exitCode == 1);
    REQUIRE(run.output.find(fragment) != std::string::npos);
}

} // namespace

TEST_CASE("the lock records the accepted shape and then checks clean", "[schema][lock]")
{
    LockCase testCase("lock-roundtrip");

    /* Before anything is recorded, the check says how to record it. */
    testCase.write(kHealthV1);
    requireProblem(testCase.check(), "has no schemas.lock.json");
    requireProblem(testCase.check(), "xmake schema-lock");

    const ToolRun update = testCase.update();
    INFO(update.output);
    REQUIRE(update.exitCode == 0);
    REQUIRE(update.output.find("1 lock(s) updated") != std::string::npos);

    const std::string lock = testCase.lock();
    REQUIRE(lock.find("\"module\": \"resonate.test\"") != std::string::npos);
    REQUIRE(lock.find("\"version\": 1") != std::string::npos);
    REQUIRE(lock.find("field current i32 @0 size 4 min=0 default=100") != std::string::npos);
    /* The shape carries what is inlined into the component, not just its own
       fields: the enum is part of what version 1 means. */
    REQUIRE(lock.find("enum Flags u8 values Idle=0, Busy=1") != std::string::npos);

    const ToolRun check = testCase.check();
    INFO(check.output);
    REQUIRE(check.exitCode == 0);

    /* Nothing changed, so a second update writes nothing. */
    const ToolRun again = testCase.update();
    INFO(again.output);
    REQUIRE(again.exitCode == 0);
    REQUIRE(again.output.find("0 lock(s) updated") != std::string::npos);
}

TEST_CASE("a version that goes backwards fails the check", "[schema][lock]")
{
    LockCase testCase("lock-rollback");
    testCase.write(kHealthV2);
    REQUIRE(testCase.update().exitCode == 0);

    testCase.write(kHealthV1);
    requireProblem(testCase.check(), "a version never goes backwards");
    requireProblem(testCase.check(), "records version 2, the schema declares 1");
}

TEST_CASE("a shape change at the same version fails the check", "[schema][lock]")
{
    LockCase testCase("lock-shape");
    testCase.write(kHealthV1);
    REQUIRE(testCase.update().exitCode == 0);

    testCase.write(kHealthV1Wide);
    requireProblem(testCase.check(), "is recorded with a different shape");
    /* The report names the first line that differs, which for a retyped field
       is the size it moved. */
    requireProblem(testCase.check(), "size 8 align 4");
    requireProblem(testCase.check(), "size 16 align 8");

    /* What a field names is part of the shape too, even when the component's
       own layout does not move. */
    LockCase values("lock-shape-enum");
    values.write(kHealthV1);
    REQUIRE(values.update().exitCode == 0);
    values.write(kHealthV1MoreFlags);
    requireProblem(values.check(), "enum Flags u8 values Idle=0, Busy=1");
    requireProblem(values.check(), "Idle=0, Busy=1, Frozen=2");
}

TEST_CASE("a version bump needs the lock recorded", "[schema][lock]")
{
    LockCase testCase("lock-bump");
    testCase.write(kHealthV1);
    REQUIRE(testCase.update().exitCode == 0);

    testCase.write(kHealthV2);
    requireProblem(testCase.check(), "run 'xmake schema-lock' to record the new version");

    REQUIRE(testCase.update().exitCode == 0);
    const ToolRun check = testCase.check();
    INFO(check.output);
    REQUIRE(check.exitCode == 0);
}

TEST_CASE("types appearing and disappearing need the lock recorded", "[schema][lock]")
{
    LockCase testCase("lock-types");
    testCase.write(kHealthV1);
    REQUIRE(testCase.update().exitCode == 0);

    testCase.write(R"xml(<schema module="resonate.test">
  <component name="Health" version="1">
    <field name="current" type="i32"/>
  </component>
  <component name="Mana" version="1">
    <field name="current" type="i32"/>
  </component>
</schema>
)xml");
    requireProblem(testCase.check(), "'Mana' is declared but not in schemas.lock.json");
    REQUIRE(testCase.update().exitCode == 0);

    testCase.write(R"xml(<schema module="resonate.test">
  <component name="Mana" version="1">
    <field name="current" type="i32"/>
  </component>
</schema>
)xml");
    requireProblem(testCase.check(), "'Health' is recorded in");
    requireProblem(testCase.check(), "but no longer declared");
}

TEST_CASE("two modules cannot share a schema directory", "[schema][lock]")
{
    LockCase testCase("lock-shared");
    resonate::test::writeTextFile(testCase.root + "/one.rschema",
                                  "<schema module=\"resonate.one\"><component name=\"A\" "
                                  "version=\"1\"><field name=\"f\" type=\"i32\"/></component>"
                                  "</schema>\n");
    resonate::test::writeTextFile(testCase.root + "/two.rschema",
                                  "<schema module=\"resonate.two\"><component name=\"B\" "
                                  "version=\"1\"><field name=\"f\" type=\"i32\"/></component>"
                                  "</schema>\n");

    requireProblem(testCase.update(), "a module owns its directory");
}
