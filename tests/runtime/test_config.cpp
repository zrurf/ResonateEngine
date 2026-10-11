#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

#include <resonate/application/config_store.h>

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
