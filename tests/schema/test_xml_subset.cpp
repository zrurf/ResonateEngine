#include <catch2/catch_all.hpp>

#include <string>

#include "cli.h"

namespace
{

using resonate::test::ToolRun;

/* Writes one file, verbatim bytes included, and compiles it: the subset rules
   are decided before any declaration is looked at. */
void expectRejected(const std::string& name, const std::string& bytes, const std::string& fragment)
{
    const std::string root = resonate::test::scratchDirectory(name);
    resonate::test::writeTextFile(root + "/case.rschema", bytes);

    const ToolRun run =
        resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root + "/gen\"");
    INFO(run.output);
    REQUIRE(run.exitCode == 1);
    REQUIRE(run.output.find(fragment) != std::string::npos);
}

void expectAccepted(const std::string& name, const std::string& bytes)
{
    const std::string root = resonate::test::scratchDirectory(name);
    resonate::test::writeTextFile(root + "/case.rschema", bytes);

    const ToolRun run =
        resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root + "/gen\"");
    INFO(run.output);
    REQUIRE(run.exitCode == 0);
}

} // namespace

TEST_CASE("a declaration inside the subset is accepted", "[schema][xml]")
{
    expectAccepted("xml-accepted", R"xml(<?xml version="1.0" encoding="utf-8"?>
<schema module="resonate.test">
  <!-- comments carry no content and are allowed -->
  <component name="Health" version="1">
    <field name="current" type="i32" min="0" default="10"/>
  </component>
</schema>
)xml");
}

TEST_CASE("a UTF-8 byte order mark is refused", "[schema][xml]")
{
    expectRejected("xml-bom", std::string("\xEF\xBB\xBF") + "<schema module=\"resonate.test\"/>\n",
                   "byte order mark");
}

TEST_CASE("bytes that are not UTF-8 are refused", "[schema][xml]")
{
    expectRejected("xml-latin1", std::string("<schema module=\"resonate.t") + "\xE9" + "st\"/>\n",
                   "not valid UTF-8");
}

TEST_CASE("a declaration must not claim another encoding", "[schema][xml]")
{
    expectRejected("xml-encoding",
                   R"xml(<?xml version="1.0" encoding="utf-16"?>
<schema module="resonate.test"/>
)xml",
                   "the declaration says encoding 'utf-16'");
}

TEST_CASE("a DTD is refused", "[schema][xml]")
{
    expectRejected("xml-dtd", R"xml(<?xml version="1.0"?>
<!DOCTYPE schema [<!ENTITY extra "value">]>
<schema module="resonate.test"/>
)xml",
                   "a DTD is not part of the XML subset");
}

TEST_CASE("namespaces are refused", "[schema][xml]")
{
    expectRejected("xml-prefix", R"xml(<x:schema module="resonate.test"/>)xml",
                   "namespaces are not part of the XML subset");

    expectRejected("xml-xmlns", R"xml(<schema xmlns="urn:resonate" module="resonate.test"/>)xml",
                   "namespaces are not part of the XML subset");
}

TEST_CASE("CDATA is refused", "[schema][xml]")
{
    expectRejected("xml-cdata", R"xml(<schema module="resonate.test"><![CDATA[x]]></schema>)xml",
                   "CDATA is not part of the XML subset");
}

TEST_CASE("text content is refused", "[schema][xml]")
{
    expectRejected("xml-text", R"xml(<schema module="resonate.test">stray text</schema>)xml",
                   "content text is not part of the XML subset");
}

TEST_CASE("processing instructions are refused", "[schema][xml]")
{
    expectRejected("xml-pi", R"xml(<schema module="resonate.test"><?target data?></schema>)xml",
                   "processing instructions are not part of the XML subset");
}

TEST_CASE("a broken document is reported where it breaks", "[schema][xml]")
{
    expectRejected("xml-unclosed", R"xml(<schema module="resonate.test">
  <component name="C" version="1">
    <field name="f" type="i32"/>
  </component>
)xml",
                   "The following elements are not closed");

    expectRejected("xml-tag-mismatch", R"xml(<schema module="resonate.test">
  <component name="C" version="1">
    <field name="f" type="i32"/>
  </struct>
</schema>
)xml",
                   "mismatch");

    expectRejected(
        "xml-duplicate-attribute",
        R"xml(<schema module="resonate.test"><component name="C" version="1" name="D"><field name="f" type="i32"/></component></schema>)xml",
        "is a duplicate attribute name");
}
