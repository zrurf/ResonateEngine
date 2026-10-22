#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include <resonate/gameplay/action_map.h>
#include <resonate/window/input.h>

namespace
{

/* One action map file per case, rewritten in place and removed on exit. */
class TempMap
{
  public:
    TempMap() : path_("resonate_test_actions_" + std::to_string(++counter) + ".ractionmap")
    {
    }

    ~TempMap()
    {
        std::remove(path_.c_str());
    }

    TempMap(const TempMap&) = delete;
    TempMap& operator=(const TempMap&) = delete;

    void write(const std::string& text)
    {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << text;
    }

    [[nodiscard]] const std::string& path() const
    {
        return path_;
    }

  private:
    inline static int counter = 0;
    std::string path_;
};

/* The raw input the map samples: a test controls which controls are down. */
struct FakeInput
{
    bool space = false;
    bool left_mouse = false;
    bool j = false;
    bool k = false;
    bool south = false;
    bool trigger = false;
    float left_x = 0.0F;
    ResonateInput input{};

    FakeInput()
    {
        input.header.struct_size = sizeof(ResonateInput);
        input.self = this;
        input.key_down = &FakeInput::keyDown;
        input.mouse_down = &FakeInput::mouseDown;
        input.gamepad_count = &FakeInput::gamepadCount;
        input.gamepad_button_down = &FakeInput::gamepadButtonDown;
        input.gamepad_axis = &FakeInput::gamepadAxis;
    }

    static std::int32_t keyDown(void* self, ResonateKey key)
    {
        const auto* fake = static_cast<const FakeInput*>(self);
        if (key == RESONATE_KEY_SPACE)
        {
            return fake->space ? 1 : 0;
        }
        if (key == RESONATE_KEY_J)
        {
            return fake->j ? 1 : 0;
        }
        if (key == RESONATE_KEY_K)
        {
            return fake->k ? 1 : 0;
        }
        return 0;
    }

    static std::int32_t mouseDown(void* self, ResonateMouseButton button)
    {
        const auto* fake = static_cast<const FakeInput*>(self);
        return button == RESONATE_MOUSE_LEFT && fake->left_mouse ? 1 : 0;
    }

    static std::int32_t gamepadCount(void*)
    {
        return 1;
    }

    static std::int32_t gamepadButtonDown(void* self, std::int32_t pad,
                                          ResonateGamepadButton button)
    {
        const auto* fake = static_cast<const FakeInput*>(self);
        return pad == 0 && button == RESONATE_GAMEPAD_SOUTH && fake->south ? 1 : 0;
    }

    static float gamepadAxis(void* self, std::int32_t pad, ResonateGamepadAxis axis)
    {
        const auto* fake = static_cast<const FakeInput*>(self);
        if (pad == 0 && axis == RESONATE_GAMEPAD_AXIS_RIGHT_TRIGGER && fake->trigger)
        {
            return 0.8F;
        }
        if (pad == 0 && axis == RESONATE_GAMEPAD_AXIS_LEFT_X)
        {
            return fake->left_x;
        }
        return 0.0F;
    }
};

const char* kDocument =
    "<?xml version=\"1.0\"?>\n"
    "<!-- three actions; each is bound on more than one device where it fits -->\n"
    "<action-map>\n"
    "  <action name=\"fire\" value=\"button\">\n"
    "    <phase name=\"pressed\"/>\n"
    "    <phase name=\"released\"/>\n"
    "  </action>\n"
    "  <action name=\"aim\"/>\n"
    "  <action name=\"move\" value=\"axis\"/>\n"
    "  <binding action=\"fire\" device=\"keyboard\" control=\"space\"/>\n"
    "  <binding action=\"fire\" device=\"mouse\" control=\"left-button\"/>\n"
    "  <binding action=\"fire\" device=\"gamepad\" control=\"south\"/>\n"
    "  <binding action=\"aim\" device=\"keyboard\" control=\"j\"/>\n"
    "  <binding action=\"aim\" device=\"gamepad\" control=\"right-trigger\"/>\n"
    "  <binding action=\"move\" device=\"gamepad\" control=\"left-stick-x\"/>\n"
    "</action-map>\n";

} // namespace

TEST_CASE("action map: bindings drive pressed, held and released", "[action-map]")
{
    TempMap file;
    file.write(kDocument);

    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(file.path(), error));
    CHECK(map.actionCount() == 3U);

    FakeInput fake;
    map.update(&fake.input);
    const auto* fire = map.state("fire");
    REQUIRE(fire != nullptr);
    CHECK_FALSE(fire->pressed);
    CHECK_FALSE(fire->held);
    CHECK_FALSE(fire->released);
    CHECK(map.state("missing") == nullptr);

    /* Space goes down: pressed on the first update, held afterwards. */
    fake.space = true;
    map.update(&fake.input);
    CHECK(fire->pressed);
    CHECK(fire->held);
    CHECK_FALSE(fire->released);
    map.update(&fake.input);
    CHECK_FALSE(fire->pressed);
    CHECK(fire->held);

    /* Releasing the key while the mouse button is down keeps it held. */
    fake.space = false;
    fake.left_mouse = true;
    map.update(&fake.input);
    CHECK_FALSE(fire->pressed);
    CHECK(fire->held);
    CHECK_FALSE(fire->released);

    fake.left_mouse = false;
    map.update(&fake.input);
    CHECK_FALSE(fire->held);
    CHECK(fire->released);

    /* A frame without an input provider releases everything. */
    fake.space = true;
    map.update(&fake.input);
    CHECK(fire->held);
    map.update(nullptr);
    CHECK(fire->released);

    const auto* aim = map.state("aim");
    REQUIRE(aim != nullptr);
    fake.j = true;
    map.update(&fake.input);
    CHECK(aim->pressed);

    /* The gamepad drives the same actions: a face button holds fire, and a
       trigger past halfway drives the digital aim state. Space was still held
       from the null-input check, so release first to see a fresh press. */
    fake.j = false;
    fake.space = false;
    map.update(&fake.input);
    fake.south = true;
    map.update(&fake.input);
    CHECK(fire->pressed);
    CHECK(fire->held);
    fake.south = false;
    map.update(&fake.input);
    CHECK(fire->released);

    fake.trigger = true;
    map.update(&fake.input);
    CHECK(aim->pressed);
    CHECK(aim->held);
    fake.trigger = false;
    map.update(&fake.input);
    CHECK(aim->released);
}

TEST_CASE("action map: an axis action reports its binding's value", "[action-map]")
{
    TempMap file;
    file.write(kDocument);
    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(file.path(), error));

    const auto* move = map.state("move");
    REQUIRE(move != nullptr);
    FakeInput fake;

    /* A stick past halfway is down; the value is the raw axis, sign included. */
    fake.left_x = 0.75F;
    map.update(&fake.input);
    CHECK(move->pressed);
    CHECK(move->held);
    CHECK(move->value > 0.74F);
    CHECK(move->value < 0.76F);

    fake.left_x = -0.8F;
    map.update(&fake.input);
    CHECK(move->held);
    CHECK(move->value > -0.81F);
    CHECK(move->value < -0.79F);

    /* Below the threshold it is not held, but the axis still reports. */
    fake.left_x = 0.3F;
    map.update(&fake.input);
    CHECK_FALSE(move->held);
    CHECK(move->released);
    CHECK(move->value > 0.29F);
    CHECK(move->value < 0.31F);

    /* A button action reports 1 while held, 0 otherwise. */
    fake.left_x = 0.0F;
    const auto* fire = map.state("fire");
    REQUIRE(fire != nullptr);
    fake.space = true;
    map.update(&fake.input);
    CHECK(fire->value > 0.99F);
    fake.space = false;
    map.update(&fake.input);
    CHECK(fire->value < 0.01F);
}

TEST_CASE("action map: a capability without the gamepad tail still drives keyboards",
          "[action-map]")
{
    TempMap file;
    file.write(kDocument);
    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(file.path(), error));

    FakeInput fake;
    fake.j = true;
    fake.south = true;
    /* The writer stops before the tail: the gamepad callbacks are unreadable. */
    fake.input.header.struct_size =
        static_cast<std::uint32_t>(offsetof(ResonateInput, gamepad_count));

    map.update(&fake.input);
    CHECK(map.state("aim")->held);
    CHECK_FALSE(map.state("fire")->held);

    /* The same input with a complete capability carries the pad. */
    fake.input.header.struct_size = sizeof(ResonateInput);
    map.update(&fake.input);
    CHECK(map.state("fire")->held);
}

TEST_CASE("action map: invalid documents fail and leave the map unchanged", "[action-map]")
{
    TempMap file;
    file.write(kDocument);
    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(file.path(), error));

    const char* const invalid[] = {
        /* unknown device */
        "<action-map><action name=\"a\"/><binding action=\"a\" device=\"wheel\" "
        "control=\"x\"/></action-map>",
        /* unknown keyboard control */
        "<action-map><action name=\"a\"/><binding action=\"a\" device=\"keyboard\" "
        "control=\"hyper-key\"/></action-map>",
        /* binding to an undeclared action */
        "<action-map><action name=\"a\"/><binding action=\"b\" device=\"keyboard\" "
        "control=\"space\"/></action-map>",
        /* duplicate action */
        "<action-map><action name=\"a\"/><action name=\"a\"/></action-map>",
        /* unknown element */
        "<action-map><action name=\"a\"/><gesture/></action-map>",
        /* malformed tag */
        "<action-map><action name=\"a\"></action-map>",
    };
    for (const char* document : invalid)
    {
        file.write(document);
        CHECK_FALSE(map.load(file.path(), error));
        CHECK_FALSE(error.empty());
        CHECK(map.actionCount() == 3U);
        CHECK(map.state("fire") != nullptr);
    }
}

TEST_CASE("action map: missing files and empty maps are errors", "[action-map]")
{
    resonate::gameplay::ActionMap map;
    std::string error;
    CHECK_FALSE(map.load("no_such_action_map.ractionmap", error));
    CHECK(error.find("cannot read") != std::string::npos);

    TempMap file;
    file.write("<action-map/>\n");
    CHECK_FALSE(map.load(file.path(), error));
    CHECK(error.find("no actions") != std::string::npos);
}

TEST_CASE("action map: rebinding persists as a user layer", "[action-map]")
{
    TempMap base;
    base.write(kDocument);
    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(base.path(), error));

    FakeInput fake;
    fake.space = true;
    map.update(&fake.input);
    CHECK(map.state("fire")->held);

    /* Rebind fire's keyboard binding from space to k: the old key stops
       holding it and the new one holds it. */
    REQUIRE(map.bind("fire", "keyboard", "k", error));
    fake.space = false;
    map.update(&fake.input);
    CHECK_FALSE(map.state("fire")->held);
    fake.k = true;
    map.update(&fake.input);
    CHECK(map.state("fire")->held);

    /* The mouse binding can be removed independently. */
    REQUIRE(map.unbind("fire", "mouse", error));

    TempMap saved;
    REQUIRE(map.saveOverrides(saved.path(), error));

    /* A fresh base map plus the saved layer behaves like the rebound one. */
    resonate::gameplay::ActionMap restored;
    REQUIRE(restored.load(base.path(), error));
    REQUIRE(restored.loadOverrides(saved.path(), error));
    FakeInput check;
    check.k = true;
    restored.update(&check.input);
    CHECK(restored.state("fire")->held);
    check.k = false;
    check.space = true;
    restored.update(&check.input);
    CHECK_FALSE(restored.state("fire")->held);
    check.space = false;
    check.left_mouse = true;
    restored.update(&check.input);
    CHECK_FALSE(restored.state("fire")->held);
}

TEST_CASE("action map: reloading the base map drops the old user layer", "[action-map]")
{
    TempMap base;
    base.write(kDocument);
    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(base.path(), error));
    REQUIRE(map.bind("fire", "keyboard", "k", error));

    /* A different base map: fire is now on escape. The old user layer named
       space, which must not survive into the new map's overrides. */
    base.write("<action-map>"
               "<action name=\"fire\"/>"
               "<binding action=\"fire\" device=\"keyboard\" control=\"escape\"/>"
               "</action-map>");
    REQUIRE(map.load(base.path(), error));

    TempMap saved;
    REQUIRE(map.saveOverrides(saved.path(), error));
    std::ifstream in(saved.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(text.find("<binding") == std::string::npos);

    FakeInput fake;
    fake.k = true;
    map.update(&fake.input);
    CHECK_FALSE(map.state("fire")->held);
}

TEST_CASE("action map: invalid overrides change nothing", "[action-map]")
{
    TempMap base;
    base.write(kDocument);
    resonate::gameplay::ActionMap map;
    std::string error;
    REQUIRE(map.load(base.path(), error));

    CHECK_FALSE(map.bind("missing", "keyboard", "k", error));
    CHECK_FALSE(map.bind("fire", "keyboard", "hyper-key", error));
    CHECK_FALSE(map.unbind("missing", "mouse", error));

    TempMap bad;
    bad.write("<action-overrides>"
              "<binding action=\"missing\" device=\"keyboard\" control=\"k\"/>"
              "</action-overrides>");
    CHECK_FALSE(map.loadOverrides(bad.path(), error));
    CHECK(error.find("undeclared action") != std::string::npos);

    TempMap malformed;
    malformed.write("<action-overrides><binding action=\"fire\" device=\"keyboard\"/></action-overrides>");
    CHECK_FALSE(map.loadOverrides(malformed.path(), error));

    /* The original bindings are still in force after every failure. */
    FakeInput fake;
    fake.space = true;
    map.update(&fake.input);
    CHECK(map.state("fire")->held);
}
