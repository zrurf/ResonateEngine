#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <functional>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
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

TEST_CASE("config store: later layers override earlier ones and CVars win", "[config]")
{
    resonate::ConfigStore store;
    TempConfig base;
    TempConfig project;
    TempConfig user;
    base.write("[resonate.render]\n"
               "vsync = false\n"
               "name = \"base\"\n"
               "[resonate.window]\n"
               "width = 800\n");
    project.write("[resonate.render]\nvsync = true\n");
    user.write("[resonate.window]\nwidth = 1920\n");

    store.load(base.path());
    store.addFile(project.path());
    /* A layer that is not there is normal and leaves the stack alone. */
    store.addFile("no_such_user_layer.toml");
    store.addFile(user.path());

    CHECK(store.readBool("resonate.render", "vsync", false) == true);
    CHECK(store.value("resonate.render", "name") == std::optional<std::string>("base"));
    CHECK(store.readUInt("resonate.window", "width", 0U) == 1920U);

    store.setOverride("resonate.window", "width", "2560");
    store.setOverride("resonate.render", "vsync", "false");
    CHECK(store.readUInt("resonate.window", "width", 0U) == 2560U);
    CHECK(store.readBool("resonate.render", "vsync", true) == false);

    store.clearOverrides();
    CHECK(store.readUInt("resonate.window", "width", 0U) == 1920U);
    CHECK(store.readBool("resonate.render", "vsync", false) == true);
}

TEST_CASE("config store: a broken layer keeps the stack it had", "[config]")
{
    resonate::ConfigStore store;
    TempConfig good;
    TempConfig broken;
    good.write("[resonate.render]\nvsync = true\n");
    broken.write("[resonate.render\nvsync =\n");

    store.load(good.path());
    store.addFile(broken.path());
    CHECK(store.readBool("resonate.render", "vsync", false) == true);
}

namespace
{

struct Collected
{
    std::vector<std::pair<std::string, std::string>> entries;
};

void collectScalar(void* user, const char* section, const char* key, const char* value)
{
    auto* collected = static_cast<Collected*>(user);
    collected->entries.emplace_back(std::string(section) + "." + key, value);
}

} // namespace

TEST_CASE("config store: the merged view reports each key once, with the winner", "[config]")
{
    resonate::ConfigStore store;
    TempConfig base;
    TempConfig over;
    base.write("[resonate.render]\n"
               "vsync = false\n"
               "name = \"base\"\n");
    over.write("[resonate.render]\nvsync = true\n");
    store.load(base.path());
    store.addFile(over.path());
    store.setOverride("resonate.render", "name", "override");

    Collected collected;
    store.visitScalars(&collected, &collectScalar);
    std::map<std::string, std::string> merged;
    for (const auto& entry : collected.entries)
    {
        CHECK(merged.emplace(entry.first, entry.second).second);
    }
    REQUIRE(merged.size() == 2U);
    CHECK(merged["resonate.render.vsync"] == "true");
    CHECK(merged["resonate.render.name"] == "override");
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
                      "default = 1280\n"
                      "[[setting]]\n"
                      "key = \"render.scale\"\n"
                      "type = \"float\"\n"
                      "range = [0.25, 1.0]\n"
                      "default = 1.0\n");
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

    /* NaN compares false against both bounds; the finite check refuses it
       before the range can. */
    config_file.write("[render]\nscale = nan\n");
    store.load(config_file.path());
    CHECK_FALSE(schema.validate(store, warnings, error));
    CHECK(error.find("expected a number") != std::string::npos);

    /* The single-value check a console uses shares the same contract. */
    REQUIRE(schema.checkValue("render.width", "800", error));
    CHECK_FALSE(schema.checkValue("render.width", "wide", error));
    CHECK_FALSE(schema.checkValue("render.width", "9000", error));
    CHECK_FALSE(schema.checkValue("render.undeclared", "1", error));
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

namespace
{

struct RecordedChange
{
    std::string path;
    std::string value;
};

struct ChangeLog
{
    std::vector<RecordedChange> changes;
    std::vector<std::string> messages;

    static void onChange(void* user, const char* section, const char* key, const char* value)
    {
        auto* log = static_cast<ChangeLog*>(user);
        std::string path = section != nullptr ? section : "";
        if (key != nullptr)
        {
            if (!path.empty())
            {
                path.push_back('.');
            }
            path.append(key);
        }
        log->changes.push_back(RecordedChange{std::move(path), value != nullptr ? value : ""});
    }

    static void onMessage(void* user, const char* message)
    {
        static_cast<ChangeLog*>(user)->messages.emplace_back(message != nullptr ? message : "");
    }

    static bool onValidate(void* user, const char* section, const char* key, const char* value,
                           std::string& error)
    {
        auto* log = static_cast<ChangeLog*>(user);
        std::string path = section != nullptr ? section : "";
        if (key != nullptr)
        {
            if (!path.empty())
            {
                path.push_back('.');
            }
            path.append(key);
        }
        /* The fixture refuses one specific value to exercise the refusal. */
        if (value != nullptr && std::string(value) == "poison")
        {
            error = path + ": refused by the test";
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::string* find(const std::string& path) const
    {
        for (const RecordedChange& change : changes)
        {
            if (change.path == path)
            {
                return &change.value;
            }
        }
        return nullptr;
    }
};

/* Pumps the store until `done` holds or the budget runs out: the OS delivers
   the notification asynchronously. */
bool pollUntil(resonate::ConfigStore& store, const std::function<bool()>& done,
               int budget_millis = 3000)
{
    for (int waited = 0; waited < budget_millis; waited += 20)
    {
        store.poll();
        if (done())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

TEST_CASE("config store: a watched file reloads and reports each changed key", "[config]")
{
    TempConfig file;
    file.write("[resonate.render]\nvsync = false\nname = \"first\"\n"
               "[resonate.window]\nwidth = 800\n");

    resonate::ConfigStore store;
    ChangeLog log;
    store.load(file.path());
    store.setChangeSink(&log, &ChangeLog::onChange);
    store.setMessageSink(&log, &ChangeLog::onMessage);
    REQUIRE(store.watchFiles());

    /* Let the filesystem settle the first write before the second one, so the
       timestamps differ. */
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    file.write("[resonate.render]\nvsync = true\nname = \"second\"\n"
               "[resonate.window]\nwidth = 800\n"
               "[resonate.render.detail]\nlevel = 2\n");

    REQUIRE(pollUntil(store, [&log] { return log.changes.size() >= 2U; }));
    CHECK(store.readBool("resonate.render", "vsync", false) == true);
    CHECK(store.value("resonate.render", "name") == std::optional<std::string>("second"));
    CHECK(store.value("resonate.render.detail", "level") == std::optional<std::string>("2"));

    const std::string* vsync = log.find("resonate.render.vsync");
    REQUIRE(vsync != nullptr);
    CHECK(*vsync == "true");
    const std::string* level = log.find("resonate.render.detail.level");
    REQUIRE(level != nullptr);
    CHECK(*level == "2");
    /* The unchanged key reports nothing. */
    CHECK(log.find("resonate.window.width") == nullptr);
}

TEST_CASE("config store: a removed key reports an empty value", "[config]")
{
    TempConfig file;
    file.write("[resonate.render]\nname = \"kept\"\nvsync = false\n");

    resonate::ConfigStore store;
    ChangeLog log;
    store.load(file.path());
    store.setChangeSink(&log, &ChangeLog::onChange);
    REQUIRE(store.watchFiles());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    file.write("[resonate.render]\nname = \"kept\"\n");

    REQUIRE(pollUntil(store, [&log] { return log.find("resonate.render.vsync") != nullptr; }));
    CHECK(*log.find("resonate.render.vsync") == "");
    CHECK(store.value("resonate.render", "vsync") == std::nullopt);
}

TEST_CASE("config store: a bad reload keeps the previous values", "[config]")
{
    TempConfig file;
    file.write("[resonate.render]\nvsync = true\n");

    resonate::ConfigStore store;
    ChangeLog log;
    store.load(file.path());
    store.setChangeSink(&log, &ChangeLog::onChange);
    store.setMessageSink(&log, &ChangeLog::onMessage);
    REQUIRE(store.watchFiles());

    SECTION("a file that does not parse")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        file.write("not toml [\n");
        REQUIRE(pollUntil(store, [&log] { return !log.messages.empty(); }));
        CHECK(log.changes.empty());
        CHECK(store.readBool("resonate.render", "vsync", false) == true);
        CHECK(log.messages.front().find("kept the previous configuration") != std::string::npos);
    }
    SECTION("a value the sink refuses")
    {
        store.setValidation(&log, &ChangeLog::onValidate);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        file.write("[resonate.render]\nvsync = true\nname = \"poison\"\n");
        REQUIRE(pollUntil(store, [&log] { return !log.messages.empty(); }));
        CHECK(log.changes.empty());
        CHECK(store.value("resonate.render", "name") == std::nullopt);
        CHECK(store.readBool("resonate.render", "vsync", false) == true);
        CHECK(log.messages.front().find("refused by the test") != std::string::npos);
    }
}

TEST_CASE("config store: a change shadowed by a CVar reports nothing", "[config]")
{
    TempConfig file;
    file.write("[resonate.window]\nwidth = 800\n");

    resonate::ConfigStore store;
    ChangeLog log;
    store.load(file.path());
    store.setOverride("resonate.window", "width", "2560");
    store.setChangeSink(&log, &ChangeLog::onChange);
    REQUIRE(store.watchFiles());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    file.write("[resonate.window]\nwidth = 1920\n");

    /* The reload happens (the file changed), but the effective value stays
       the override's, so no event fires. Give it a moment, then check. */
    pollUntil(store, [] { return false; }, 600);
    CHECK(log.changes.empty());
    CHECK(store.readUInt("resonate.window", "width", 0U) == 2560U);
}
