#include <catch2/catch_all.hpp>

#include <string>

#include "cli.h"

namespace
{

using resonate::test::ToolRun;

/* One schema's worth of input: a scratch directory, the files written into it,
   and the compile that reads them back. */
struct SchemaCase
{
    std::string root;

    explicit SchemaCase(const std::string& name) : root(resonate::test::scratchDirectory(name))
    {
    }

    void write(const std::string& relative, const std::string& content) const
    {
        resonate::test::writeTextFile(root + "/" + relative, content);
    }

    ToolRun compile() const
    {
        return resonate::test::runSchemac("--root \"" + root + "\" --out \"" + root + "/gen\"");
    }
};

/* A single-file case that must be refused, with the message fragment naming the
   reason. */
void expectError(const std::string& name, const std::string& content, const std::string& fragment)
{
    SchemaCase testCase(name);
    testCase.write("case.rschema", content);

    const ToolRun run = testCase.compile();
    INFO(run.output);
    REQUIRE(run.exitCode == 1);
    REQUIRE(run.output.find(fragment) != std::string::npos);
}

void expectAccepted(const std::string& name, const std::string& content)
{
    SchemaCase testCase(name);
    testCase.write("case.rschema", content);

    const ToolRun run = testCase.compile();
    INFO(run.output);
    REQUIRE(run.exitCode == 0);
}

} // namespace

TEST_CASE("a module declaring every kind of type is accepted", "[schema]")
{
    expectAccepted("kinds", R"xml(<?xml version="1.0" encoding="utf-8"?>
<schema module="resonate.test">
  <enum name="HealthFlags" width="u8">
    <value name="Poisoned"/>
    <value name="Regenerating"/>
    <value name="Invulnerable"/>
  </enum>

  <bitmask name="HitMask" width="u32">
    <flag name="Fire"/>
    <flag name="Ice" bit="3"/>
  </bitmask>

  <struct name="Damage">
    <field name="amount" type="i32" min="0"/>
    <field name="stagger" type="f32" min="0.0" max="1.0"/>
  </struct>

  <component name="Health" version="1">
    <field name="current" type="i32" min="0" default="100"/>
    <field name="flags" type="HealthFlags" default="Regenerating"/>
    <field name="mask" type="HitMask" default="Ice"/>
    <field name="regen" type="f32" default="0.5"/>
    <field name="source" type="entity"/>
    <field name="last" type="Damage"/>
    <field name="ready" type="bool" default="true"/>
    <field name="bag" type="blob"/>
    <field name="offset" type="vec2" default="1 2"/>
    <field name="position" type="vec3" default="1 2 3"/>
    <field name="color" type="vec4" default="0 0 0 1"/>
    <field name="facing" type="quat" default="0 0 0 1"/>
    <field name="basis" type="mat4"/>
  </component>

  <node name="SceneEntity" version="1">
    <field name="health" type="Health"/>
  </node>
</schema>
)xml");
}

TEST_CASE("the root element is the document kind", "[schema]")
{
    expectError("root-name", R"xml(<document module="resonate.test"/>)xml",
                "the root element is 'document'");
    expectError("root-attr", R"xml(<schema/>)xml", "the schema root needs a 'module' attribute");
}

TEST_CASE("the module id is a reverse-DNS identity", "[schema]")
{
    expectError("module-shape", R"xml(<schema module="Gameplay"/>)xml", "is not a module id");
    expectError("module-dots", R"xml(<schema module="resonate"/>)xml", "is not a module id");
}

TEST_CASE("a declaration outside the vocabulary is refused", "[schema]")
{
    expectError("unknown-element", R"xml(<schema module="resonate.test">
  <widget name="x"/>
</schema>
)xml",
                "'widget' is not a type declaration");

    expectError("unknown-attribute", R"xml(<schema module="resonate.test">
  <component name="C" version="1" color="red">
    <field name="f" type="i32"/>
  </component>
</schema>
)xml",
                "takes name, version; 'color' is not one of them");
}

TEST_CASE("a type has one owner module", "[schema]")
{
    SchemaCase duplicates("type-twice");
    duplicates.write("one.rschema", R"xml(<schema module="resonate.test">
  <component name="Health" version="1"><field name="current" type="i32"/></component>
</schema>
)xml");
    duplicates.write("two.rschema", R"xml(<schema module="resonate.test">
  <component name="Health" version="1"><field name="current" type="i32"/></component>
</schema>
)xml");

    const ToolRun run = duplicates.compile();
    INFO(run.output);
    REQUIRE(run.exitCode == 1);
    REQUIRE(run.output.find("'Health' is declared twice in module 'resonate.test'") !=
            std::string::npos);

    /* Two modules may own a type of the same name: the qualified name is what
       identifies a component, so they are different types. */
    SchemaCase modules("type-two-modules");
    modules.write("one.rschema", R"xml(<schema module="resonate.one">
  <component name="Health" version="1"><field name="current" type="i32"/></component>
</schema>
)xml");
    modules.write("two.rschema", R"xml(<schema module="resonate.two">
  <component name="Health" version="1"><field name="current" type="i32"/></component>
</schema>
)xml");

    const ToolRun shared = modules.compile();
    INFO(shared.output);
    REQUIRE(shared.exitCode == 0);
}

TEST_CASE("an enum needs an explicit integer width", "[schema]")
{
    expectError("enum-width", R"xml(<schema module="resonate.test">
  <enum name="E" width="f32"><value name="A"/></enum>
</schema>
)xml",
                "an enum's width is an integer type");

    expectError("enum-range", R"xml(<schema module="resonate.test">
  <enum name="E" width="u8"><value name="A" value="256"/></enum>
</schema>
)xml",
                "does not fit the enum's width");

    expectError("enum-twice", R"xml(<schema module="resonate.test">
  <enum name="E" width="u8"><value name="A"/><value name="A"/></enum>
</schema>
)xml",
                "the enum value 'A' is declared twice");

    expectError("enum-empty", R"xml(<schema module="resonate.test">
  <enum name="E" width="u8"/>
</schema>
)xml",
                "declares no values");
}

TEST_CASE("a bitmask allocates bits in order", "[schema]")
{
    expectError("mask-signed", R"xml(<schema module="resonate.test">
  <bitmask name="M" width="i32"><flag name="A"/></bitmask>
</schema>
)xml",
                "a bitmask's width is an unsigned type");

    expectError("mask-taken", R"xml(<schema module="resonate.test">
  <bitmask name="M" width="u32"><flag name="A"/><flag name="B" bit="0"/></bitmask>
</schema>
)xml",
                "bit 0 is already taken");

    expectError("mask-range", R"xml(<schema module="resonate.test">
  <bitmask name="M" width="u8"><flag name="A" bit="8"/></bitmask>
</schema>
)xml",
                "bit 8 is outside the bitmask's width");
}

TEST_CASE("a field names a type the module declares", "[schema]")
{
    expectError("field-twice", R"xml(<schema module="resonate.test">
  <component name="C" version="1">
    <field name="f" type="i32"/>
    <field name="f" type="i32"/>
  </component>
</schema>
)xml",
                "the field 'f' is declared twice");

    expectError("field-keyword", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="class" type="i32"/></component>
</schema>
)xml",
                "'class' is a C++ keyword");

    expectError("field-unknown", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="Nope"/></component>
</schema>
)xml",
                "'Nope' is not a declared type in module 'resonate.test'");

    expectError("field-component", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="D"/></component>
  <component name="D" version="1"><field name="g" type="i32"/></component>
</schema>
)xml",
                "'D' is a component; a storage field inlines a struct, an enum or a bitmask");
}

TEST_CASE("types the design names but v0 does not have are refused as such", "[schema]")
{
    expectError("field-asset", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="asset:texture"/></component>
</schema>
)xml",
                "asset references arrive with the asset registry's GUID");

    expectError("field-array", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="f32[4]"/></component>
</schema>
)xml",
                "fixed arrays are not implemented yet");
}

TEST_CASE("a blob field takes no scalar constraint", "[schema]")
{
    expectError("blob-min", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="blob" min="0"/></component>
</schema>
)xml",
                "a blob field takes no min, max or default");

    expectError("blob-default", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="blob" default="0"/></component>
</schema>
)xml",
                "the blob is the variable part");
}

TEST_CASE("a math field's default is one number per component", "[schema]")
{
    expectError("vec-min", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="vec3" min="0"/></component>
</schema>
)xml",
                "a vec3 field takes no min or max");

    expectError("vec-count", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="vec3" default="1 2"/></component>
</schema>
)xml",
                "a vec3 default is one number per component (3), got 2");

    expectError("quat-value", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="quat" default="0 0 0 x"/></component>
</schema>
)xml",
                "'x' is not a number for the quat default");

    expectError("mat4-default", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="mat4" default="0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1"/></component>
</schema>
)xml",
                "a mat4 field takes no default");
}

TEST_CASE("a struct cannot be inlined into itself", "[schema]")
{
    expectError("struct-cycle", R"xml(<schema module="resonate.test">
  <struct name="A"><field name="b" type="B"/></struct>
  <struct name="B"><field name="a" type="A"/></struct>
</schema>
)xml",
                "is inlined into itself");
}

TEST_CASE("constraints have to fit the field's type", "[schema]")
{
    expectError("min-bool", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="bool" min="0"/></component>
</schema>
)xml",
                "a bool field takes no min or max");

    expectError("default-entity", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="entity" default="0"/></component>
</schema>
)xml",
                "an entity field takes no min, max or default");

    expectError("min-above-max", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="i32" min="5" max="3"/></component>
</schema>
)xml",
                "is above the max");

    expectError("default-outside", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="i32" min="3" default="1"/></component>
</schema>
)xml",
                "is outside [3, 2147483647]");

    expectError("default-type", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="u8" default="300"/></component>
</schema>
)xml",
                "does not fit the field's type");

    expectError("default-enum", R"xml(<schema module="resonate.test">
  <enum name="E" width="u8"><value name="A"/></enum>
  <component name="C" version="1"><field name="f" type="E" default="B"/></component>
</schema>
)xml",
                "'B' is not a value of the enum 'E'");

    expectError("enum-constraints", R"xml(<schema module="resonate.test">
  <enum name="E" width="u8"><value name="A"/></enum>
  <component name="C" version="1"><field name="f" type="E" min="0"/></component>
</schema>
)xml",
                "is not numeric; min and max do not apply");

    expectError("f32-range", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="f32" default="1e300"/></component>
</schema>
)xml",
                "is beyond the range of f32");

    expectError("not-a-number", R"xml(<schema module="resonate.test">
  <component name="C" version="1"><field name="f" type="i32" min="abc"/></component>
</schema>
)xml",
                "'abc' is not an integer min");
}

TEST_CASE("a version above one needs a migration path to it", "[schema]")
{
    expectError("version-zero", R"xml(<schema module="resonate.test">
  <component name="C" version="0"><field name="f" type="i32"/></component>
</schema>
)xml",
                "is not an integer of at least 1");

    expectError("version-no-migrations", R"xml(<schema module="resonate.test">
  <component name="C" version="2"><field name="f" type="i32"/></component>
</schema>
)xml",
                "declares no migrations");

    expectError("migration-gap", R"xml(<schema module="resonate.test">
  <component name="C" version="3">
    <field name="f" type="i32"/>
    <migrate from="1" to="2"/>
    <migrate from="3" to="4"/>
  </component>
</schema>
)xml",
                "the chain has a gap");

    expectError("migration-end", R"xml(<schema module="resonate.test">
  <component name="C" version="3">
    <field name="f" type="i32"/>
    <migrate from="1" to="2"/>
  </component>
</schema>
)xml",
                "the migration chain ends at version 2 but 'C' is at version 3");

    expectError("migration-order", R"xml(<schema module="resonate.test">
  <component name="C" version="2">
    <field name="f" type="i32"/>
    <migrate from="2" to="1"/>
  </component>
</schema>
)xml",
                "'to' is the later version");

    expectError("migration-flat", R"xml(<schema module="resonate.test">
  <component name="C" version="1">
    <field name="f" type="i32"/>
    <migrate from="1" to="2"/>
  </component>
</schema>
)xml",
                "there is nothing below it to migrate from");

    expectAccepted("migration-chain", R"xml(<schema module="resonate.test">
  <component name="C" version="3">
    <field name="f" type="i32"/>
    <migrate from="1" to="2"/>
    <migrate from="2" to="3"/>
  </component>
</schema>
)xml");
}

TEST_CASE("the document family is modelled but not generated", "[schema]")
{
    expectAccepted("node-family", R"xml(<schema module="resonate.test">
  <node name="SceneRecord" version="1">
    <field name="health" type="i32"/>
  </node>
</schema>
)xml");

    expectError("node-migration", R"xml(<schema module="resonate.test">
  <node name="SceneRecord" version="2">
    <field name="health" type="i32"/>
    <migrate from="1" to="2"/>
  </node>
</schema>
)xml",
                "v0 declares migrations on components only");
}
