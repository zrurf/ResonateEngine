#include <catch2/catch_all.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <resonate/application/settings.h>
#include <resonate/startup/cvar.h>

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

/* The bound target: one float the applier writes and the reader formats. */
struct FakeSetting
{
    float value = 1.0F;

    static bool apply(void* context, const std::string& text, std::string&)
    {
        static_cast<FakeSetting*>(context)->value = std::strtof(text.c_str(), nullptr);
        return true;
    }

    static std::string read(void* context)
    {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "%.2f",
                      static_cast<double>(static_cast<FakeSetting*>(context)->value));
        return text;
    }
};

} // namespace

TEST_CASE("cvar host: a set validates against the schema and reaches the system",
          "[runtime][cvar]")
{
    TempConfig schema_file;
    schema_file.write("[[setting]]\n"
                      "key = \"resonate.render.scale\"\n"
                      "type = \"float\"\n"
                      "range = [0.25, 1.0]\n"
                      "default = 1.0\n");
    resonate::SettingsSchema schema;
    std::string error;
    REQUIRE(schema.load(schema_file.path(), error));

    FakeSetting setting;
    resonate::startup::CVarHost host;
    host.setSchema(&schema);
    host.bind({"resonate.render.scale", &setting, &FakeSetting::apply, &FakeSetting::read});

    REQUIRE(host.apply("resonate.render.scale", "0.5", error));
    CHECK(error.empty());
    CHECK(setting.value == 0.5F);
    std::string value;
    REQUIRE(host.read("resonate.render.scale", value));
    CHECK(value == "0.50");

    /* Out of range: refused before the applier runs. */
    CHECK_FALSE(host.apply("resonate.render.scale", "4.0", error));
    CHECK(error.find("outside the declared range") != std::string::npos);
    CHECK(setting.value == 0.5F);

    /* An unknown key names itself before the schema is consulted. */
    CHECK_FALSE(host.apply("resonate.render.unknown", "1", error));
    CHECK(error.find("not a bound cvar") != std::string::npos);

    /* A set without a schema trusts the applier. */
    resonate::startup::CVarHost unvalidated;
    unvalidated.bind({"resonate.render.scale", &setting, &FakeSetting::apply,
                      &FakeSetting::read});
    REQUIRE(unvalidated.apply("resonate.render.scale", "0.75", error));
    CHECK(setting.value == 0.75F);
}

TEST_CASE("cvar host: find lists the bound keys and rebind replaces", "[runtime][cvar]")
{
    FakeSetting setting;
    resonate::startup::CVarHost host;
    host.bind({"resonate.render.scale", &setting, &FakeSetting::apply, &FakeSetting::read});
    host.bind({"resonate.window.width", &setting, &FakeSetting::apply, &FakeSetting::read});
    /* The last binding for a key wins. */
    host.bind({"resonate.render.scale", &setting, &FakeSetting::apply, &FakeSetting::read});

    const std::vector<std::string> all = host.find("");
    REQUIRE(all.size() == 2U);
    /* A rebind replaces in place, so the first registration keeps its slot. */
    CHECK(all[0] == "resonate.render.scale");
    CHECK(all[1] == "resonate.window.width");

    const std::vector<std::string> render = host.find("render");
    REQUIRE(render.size() == 1U);
    CHECK(render[0] == "resonate.render.scale");
}
