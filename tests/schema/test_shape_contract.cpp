#include <catch2/catch_all.hpp>

#include <string>

#include "cli.h"

namespace
{

using resonate::test::ToolRun;

/* The shape contract the build passes to the compiler; it lives in the source
   tree, beside the manifest schemas, so the tests read it from there. */
std::string shippedContract()
{
    return std::string(RESONATE_SOURCE_DIR) + "/schema/formats/schema/rschema.xsd";
}

/* Everything the language can express today, so "the contract accepts what the
   compiler accepts" is checked against the whole surface rather than one kind
   of declaration. */
const char* const kEverything =
    R"xml(<schema module="resonate.test">
  <enum name="Flags" width="u8">
    <value name="Idle"/>
    <value name="Busy" value="4"/>
  </enum>

  <bitmask name="Mask" width="u64">
    <flag name="Fire"/>
    <flag name="Ice" bit="63"/>
  </bitmask>

  <struct name="Inner">
    <field name="amount" type="i32" min="0" default="1"/>
  </struct>

  <component name="Health" version="2">
    <field name="current" type="i32" min="0" default="100"/>
    <field name="flags" type="Flags" default="Busy"/>
    <field name="mask" type="Mask" default="Ice"/>
    <field name="inner" type="Inner"/>
    <field name="owner" type="entity"/>
    <field name="ratio" type="f64" min="0" max="1" default="0.5"/>
    <field name="ready" type="bool" default="true"/>
    <migrate from="1" to="2"/>
  </component>

  <node name="Record" version="1">
    <field name="health" type="Health"/>
  </node>
</schema>
)xml";

const char* const kOneComponent = R"xml(<schema module="resonate.test">
  <component name="Health" version="1">
    <field name="current" type="i32"/>
  </component>
</schema>
)xml";

/* A contract that knows only the root element: everything inside is a shape it
   has never heard of. */
const char* const kNarrowContract =
    R"xml(<?xml version="1.0"?>
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="schema">
    <xs:complexType>
      <xs:sequence/>
      <xs:attribute name="module" type="xs:string" use="required"/>
    </xs:complexType>
  </xs:element>
</xs:schema>
)xml";

const char* const kPermissiveContract =
    R"xml(<?xml version="1.0"?>
<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xs:element name="schema">
    <xs:complexType>
      <xs:sequence>
        <xs:any processContents="skip" minOccurs="0" maxOccurs="unbounded"/>
      </xs:sequence>
      <xs:attribute name="module" type="xs:string" use="required"/>
    </xs:complexType>
  </xs:element>
</xs:schema>
)xml";

} // namespace

TEST_CASE("the shipped shape contract accepts everything the compiler accepts", "[schema][xsd]")
{
    const std::string root = resonate::test::scratchDirectory("shape-shipped");
    resonate::test::writeTextFile(root + "/case.rschema", kEverything);

    const ToolRun run = resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root +
                                                   "/gen\" --xsd \"" + shippedContract() + "\"");
    INFO(run.output);
    REQUIRE(run.exitCode == 0);
}

TEST_CASE("a contract that does not know a declaration fails the run", "[schema][xsd]")
{
    const std::string root = resonate::test::scratchDirectory("shape-narrow");
    resonate::test::writeTextFile(root + "/case.rschema", kOneComponent);
    resonate::test::writeTextFile(root + "/narrow.xsd", kNarrowContract);

    /* The compiler alone accepts the file: whatever fails below is the contract
       being stricter, which is the drift the build has to catch. */
    const ToolRun alone =
        resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root + "/gen\"");
    INFO(alone.output);
    REQUIRE(alone.exitCode == 0);

    const ToolRun checked = resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root +
                                                       "/gen\" --xsd \"" + root + "/narrow.xsd\"");
    INFO(checked.output);
    REQUIRE(checked.exitCode == 1);
    REQUIRE(checked.output.find("the shape contract rejects this declaration") !=
            std::string::npos);
}

TEST_CASE("a contract that accepts the file lets the run pass", "[schema][xsd]")
{
    const std::string root = resonate::test::scratchDirectory("shape-permissive");
    resonate::test::writeTextFile(root + "/case.rschema", kOneComponent);
    resonate::test::writeTextFile(root + "/permissive.xsd", kPermissiveContract);

    const ToolRun run = resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root +
                                                   "/gen\" --xsd \"" + root + "/permissive.xsd\"");
    INFO(run.output);
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.output.find("1 module(s), 1 type(s)") != std::string::npos);
}

TEST_CASE("a contract that cannot be used fails the run", "[schema][xsd]")
{
    const std::string root = resonate::test::scratchDirectory("shape-broken");
    resonate::test::writeTextFile(root + "/case.rschema", kOneComponent);

    const ToolRun missing = resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root +
                                                       "/gen\" --xsd \"" + root + "/missing.xsd\"");
    INFO(missing.output);
    REQUIRE(missing.exitCode == 1);
    REQUIRE(missing.output.find("cannot read the shape contract") != std::string::npos);

    resonate::test::writeTextFile(root + "/broken.xsd", "<xs:schema xmlns:xs=\"wrong\"/>");
    const ToolRun broken = resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root +
                                                      "/gen\" --xsd \"" + root + "/broken.xsd\"");
    INFO(broken.output);
    REQUIRE(broken.exitCode == 1);
    REQUIRE(broken.output.find("does not compile") != std::string::npos);
}
