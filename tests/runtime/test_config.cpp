#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <resonate/application/config_store.h>
#include <resonate/application/settings.h>

namespace
{

/* One config file per section, rewritten in place; removed when the test
   binary exits. */
class TempConfig
{
  public:
    TempConfig() : path_("resonate_test_config_" + std::to_string(++counter) + ".toml")
    {
    }

    ~TempConfig()
    {
        std::remove(path_.c_str());
    }

    TempConfig(const TempConfig&) = delete;
    TempConfig& operator=(const TempConfig&) = delete;

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

} // namespace

TEST_CASE("config store: scalars, nested sections and fallbacks", "[config]")
{
    resonate::ConfigStore store;
    TempConfig file;
    file.write("[resonate.render]\n"
               "vsync = false\n"
               "name = \"backend\"\n"
               "lights = 128\n"
               "[resonate.window]\n"
               "width = 1280\n"
               "[resonate.window.nested]\n"
               "height = 720\n");
    store.load(file.path());

    CHECK(store.value("resonate.render", "vsync") == std::optional<std::string>("false"));
    CHECK(store.readBool("resonate.render", "vsync", true) == false);
    CHECK(store.value("resonate.render", "name") == std::optional<std::string>("backend"));
    CHECK(store.readUInt("resonate.render", "lights", 0U) == 128U);
    CHECK(store.readUInt("resonate.window", "width", 0U) == 1280U);

    /* A table at the key reads as unset, so a mistyped section cannot be
       mistaken for a value. */
    CHECK(store.value("resonate.window", "nested") == std::nullopt);
    CHECK(store.value("resonate.window", "height") == std::nullopt);

    /* Missing keys — and the whole store missing — answer the fallback. */
    CHECK(store.readBool("resonate.render", "absent", true) == true);
    CHECK(store.readUInt("resonate.loop", "max-fps", 7U) == 7U);

    resonate::ConfigStore empty;
    CHECK(empty.value("resonate.render", "vsync") == std::nullopt);
    CHECK(empty.readBool("resonate.render", "vsync", true) == true);
}

TEST_CASE("config store: an unsigned read beyond 32 bits answers the fallback", "[config]")
{
    resonate::ConfigStore store;
    TempConfig file;
    file.write("[resonate.test]\nbig = \"99999999999999999999\"\n");
    store.load(file.path());
    CHECK(store.readUInt("resonate.test", "big", 7U) == 7U);

    file.write("[resonate.test]\nbig = \"4294967295\"\n");
    store.load(file.path());
    CHECK(store.readUInt("resonate.test", "big", 7U) == 4294967295U);
}

TEST_CASE("config store: a file that cannot be parsed is reported and left empty", "[config]")
{
    resonate::ConfigStore store;
    TempConfig file;
    file.write("[resonate.render\nvsync = \n");

    store.load(file.path());
    CHECK(store.value("resonate.render", "vsync") == std::nullopt);
}

TEST_CASE("config store: a missing file is not an error", "[config]")
{
    resonate::ConfigStore store;
    store.load("no_such_resonate_config_file.toml");
    CHECK(store.readBool("resonate.render", "vsync", true) == true);
}

TEST_CASE("settings schema: values validate and defaults fill the gaps", "[settings]")
{
    TempConfig schema_file;
    schema_file.write("[[setting]]\n"
                      "key = \"render.width\"\n"
                      "type = \"int\"\n"
                      "range = [1, 4096]\n"
                      "default = 1280\n"
                      "[[setting]]\n"
                      "key = \"render.vsync\"\n"
                      "type = \"bool\"\n"
                      "default = true\n"
                      "[[setting]]\n"
                      "key = \"render.mode\"\n"
                      "type = \"enum\"\n"
                      "values = [\"fast\", \"pretty\"]\n"
                      "default = \"fast\"\n");

    resonate::SettingsSchema schema;
    std::string error;
    REQUIRE(schema.load(schema_file.path(), error));
    CHECK(schema.size() == 3);

    TempConfig config_file;
    config_file.write("[render]\n"
                      "width = 800\n"
                      "vsync = false\n"
                      "mode = \"pretty\"\n");
    resonate::ConfigStore store;
    store.load(config_file.path());
    std::vector<std::string> warnings;
    REQUIRE(schema.validate(store, warnings, error));
    CHECK(warnings.empty());
    CHECK(schema.readUInt(store, "render.width", 1U) == 800U);
    CHECK(schema.readBool(store, "render.vsync", true) == false);
    CHECK(schema.readString(store, "render.mode", "fast") == "pretty");

    /* An omitted key takes the declared default, not the caller's fallback. */
    resonate::ConfigStore empty;
    REQUIRE(schema.validate(empty, warnings, error));
    CHECK(schema.readUInt(empty, "render.width", 1U) == 1280U);
    CHECK(schema.readBool(empty, "render.vsync", false) == true);
    CHECK(schema.readUInt(empty, "render.undeclared", 7U) == 7U);
}

TEST_CASE("settings schema: an unknown key warns", "[settings]")
{
    TempConfig schema_file;
    schema_file.write("[[setting]]\n"
                      "key = \"render.width\"\n"
                      "type = \"int\"\n"
                      "default = 1280\n");
    resonate::SettingsSchema schema;
    std::string error;
    REQUIRE(schema.load(schema_file.path(), error));

    TempConfig config_file;
    config_file.write("[render]\n"
                      "width = 800\n"
                      "mystery = 3\n");
    resonate::ConfigStore store;
    store.load(config_file.path());
    std::vector<std::string> warnings;
    REQUIRE(schema.validate(store, warnings, error));
    REQUIRE(warnings.size() == 1U);
    CHECK(warnings[0].find("render.mystery") != std::string::npos);
}

TEST_CASE("settings schema: a wrong type or range is refused", "[settings]")
{
    TempConfig schema_file;
    schema_file.write("[[setting]]\n"
                      "key = \"render.width\"\n"
                      "type = \"int\"\n"
                      "range = [1, 4096]\n"
                      "default = 1280\n");
    resonate::SettingsSchema schema;
    std::string error;
    REQUIRE(schema.load(schema_file.path(), error));

    TempConfig config_file;
    config_file.write("[render]\nwidth = \"wide\"\n");
    resonate::ConfigStore store;
    store.load(config_file.path());
    std::vector<std::string> warnings;
    CHECK_FALSE(schema.validate(store, warnings, error));
    CHECK(error.find("expected an integer") != std::string::npos);

    config_file.write("[render]\nwidth = 9000\n");
    store.load(config_file.path());
    CHECK_FALSE(schema.validate(store, warnings, error));
    CHECK(error.find("outside the declared range") != std::string::npos);
}

TEST_CASE("settings schema: bad declarations fail the load", "[settings]")
{
    TempConfig duplicate;    duplicate.write("[[setting]]\n"
                    "key = \"render.width\"\n"
                    "type = \"int\"\n"
                    "default = 1\n"
                    "[[setting]]\n"
                    "key = \"render.width\"\n"
                    "type = \"int\"\n"
                    "default = 2\n");
    resonate::SettingsSchema schema;
    std::string error;
    CHECK_FALSE(schema.load(duplicate.path(), error));
    CHECK(error.find("declared twice") != std::string::npos);

    TempConfig bad_default;
    bad_default.write("[[setting]]\n"
                      "key = \"render.mode\"\n"
                      "type = \"enum\"\n"
                      "values = [\"fast\", \"pretty\"]\n"
                      "default = \"slow\"\n");
    CHECK_FALSE(schema.load(bad_default.path(), error));
    CHECK(error.find("the default is invalid") != std::string::npos);

    TempConfig unknown_field;
    unknown_field.write("[[setting]]\n"
                        "key = \"render.width\"\n"
                        "type = \"int\"\n"
                        "default = 1\n"
                        "colour = \"red\"\n");
    CHECK_FALSE(schema.load(unknown_field.path(), error));
    CHECK(error.find("unknown field") != std::string::npos);
}

TEST_CASE("the project settings contract declares the engine keys", "[settings]")
{
    resonate::SettingsSchema schema;
    std::string error;
    REQUIRE(schema.load(std::string(RESONATE_SOURCE_DIR) + "/resonate.settings.toml", error));

    const resonate::SettingDecl* width = schema.find("resonate.window.width");
    REQUIRE(width != nullptr);
    CHECK(width->type == resonate::SettingType::Int);
    CHECK(width->default_value == "1600");
    CHECK(width->bounded);

    const resonate::SettingDecl* antialiasing =
        schema.find("resonate.render.antialiasing");
    REQUIRE(antialiasing != nullptr);
    CHECK(antialiasing->type == resonate::SettingType::Enum);
    CHECK(antialiasing->values.size() == 2U);
}
