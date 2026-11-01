#include <catch2/catch_all.hpp>

#include <string>

#include <resonate/scene/machine_model.h>

namespace
{

constexpr const char* kFragment = R"xml(<Machine entry="spin" parameters="input_x jump">
  <State name="bob" clip="#bob"/>
  <State name="spin" clip="@0704c7df-7013-4c31-818d-b515207bb7df" looping="false"/>
  <Transition from="bob" to="spin" fade="0.4">
    <Condition parameter="input_x" greater="0.5"/>
  </Transition>
  <Transition from="spin" to="bob" at_end="true">
    <Condition parameter="jump" trigger="true"/>
  </Transition>
</Machine>)xml";

} // namespace

TEST_CASE("machine model: a fragment parses into the authored strings", "[scene][machine]")
{
    resonate::scene::MachineModel model;
    std::string error;
    const bool ok = resonate::scene::parseMachineFragment(kFragment, model, error);
    INFO("parse: " << error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(model.entry == "spin");
    CHECK(model.parameters == "input_x jump");
    REQUIRE(model.states.size() == 2U);
    CHECK(model.states[0].name == "bob");
    CHECK(model.states[0].clip == "#bob");
    CHECK(model.states[0].looping.empty());
    CHECK(model.states[1].looping == "false");
    REQUIRE(model.transitions.size() == 2U);
    CHECK(model.transitions[0].from == "bob");
    CHECK(model.transitions[0].fade == "0.4");
    REQUIRE(model.transitions[0].conditions.size() == 1U);
    CHECK(model.transitions[0].conditions[0].parameter == "input_x");
    CHECK(model.transitions[0].conditions[0].test == "greater");
    CHECK(model.transitions[0].conditions[0].value == "0.5");
    CHECK(model.transitions[1].at_end == "true");
    CHECK(model.transitions[1].conditions[0].test == "trigger");
}

TEST_CASE("machine model: serialize and parse agree", "[scene][machine]")
{
    resonate::scene::MachineModel model;
    std::string error;
    REQUIRE(resonate::scene::parseMachineFragment(kFragment, model, error));

    const std::string first = resonate::scene::serializeMachineFragment(model);
    resonate::scene::MachineModel reparsed;
    REQUIRE(resonate::scene::parseMachineFragment(first, reparsed, error));
    const std::string second = resonate::scene::serializeMachineFragment(reparsed);
    /* Two serializations of one model are byte-identical, so an editor edit
       never churns the stored fragment. */
    CHECK(first == second);
    CHECK(first.find("entry=\"spin\"") != std::string::npos);
    CHECK(first.find("parameters=\"input_x jump\"") != std::string::npos);
    CHECK(first.find("looping=\"false\"") != std::string::npos);
    CHECK(first.find("<Condition parameter=\"input_x\" greater=\"0.5\"/>") !=
          std::string::npos);

    /* The serialized form still builds through the scene adapter's own parser:
       feed it back through parse with every attribute the adapter reads. */
    resonate::scene::MachineModel edited = model;
    edited.entry = "bob";
    const std::string edited_text = resonate::scene::serializeMachineFragment(edited);
    resonate::scene::MachineModel edited_back;
    REQUIRE(resonate::scene::parseMachineFragment(edited_text, edited_back, error));
    CHECK(edited_back.entry == "bob");
}

TEST_CASE("machine model: the grammar's refusals carry the reason", "[scene][machine]")
{
    resonate::scene::MachineModel model;
    std::string error;

    CHECK_FALSE(resonate::scene::parseMachineFragment("<clip/>", model, error));
    CHECK(error.find("root is not a Machine") != std::string::npos);

    CHECK_FALSE(resonate::scene::parseMachineFragment(
        R"xml(<Machine><State clip="#a"/></Machine>)xml", model, error));
    CHECK(error.find("name and a clip") != std::string::npos);

    CHECK_FALSE(resonate::scene::parseMachineFragment(
        R"xml(<Machine><State name="a" clip="#a"/><State name="a" clip="#a"/></Machine>)xml",
        model, error));
    CHECK(error.find("twice") != std::string::npos);

    CHECK_FALSE(resonate::scene::parseMachineFragment(
        R"xml(<Machine><State name="a" clip="#a"/><Transition from="a" to="gone"/></Machine>)xml",
        model, error));
    CHECK(error.find("unknown state") != std::string::npos);

    CHECK_FALSE(resonate::scene::parseMachineFragment(
        R"xml(<Machine><State name="a" clip="#a"/><Transition from="a" to="a">
        <Condition parameter="p" greater="fast"/></Transition></Machine>)xml", model, error));
    CHECK(error.find("one number") != std::string::npos);

    CHECK_FALSE(resonate::scene::parseMachineFragment(
        R"xml(<Machine><State name="a" clip="#a"/><Transition from="a" to="a">
        <Condition parameter="p"/></Transition></Machine>)xml", model, error));
    CHECK(error.find("no test") != std::string::npos);

    CHECK_FALSE(resonate::scene::parseMachineFragment(
        R"xml(<Machine entry="gone"><State name="a" clip="#a"/></Machine>)xml", model,
        error));
    CHECK(error.find("names no state") != std::string::npos);
}
