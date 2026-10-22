-- Project-local override of the xmake-repo HarfBuzz package: the upstream
-- download is on GitHub, which is unreachable here, so the source comes from
-- the Gitee mirror. FreeType/ICU/GLib integration is off: the engine feeds
-- HarfBuzz raw font bytes and shapes independently of those libraries.
package("harfbuzz")
    set_homepage("https://harfbuzz.github.io/")
    set_description("HarfBuzz is a text shaping library.")
    set_license("MIT")

    add_urls("https://gitee.com/mirrors/harfbuzz.git")
    add_versions("13.1.1", "8497968914f415d633c7ff885f58862667c40b79d2f04031951a5d04e91d1090")

    add_configs("icu", {description = "Enable ICU library unicode functions.", default = false, type = "boolean"})
    add_configs("freetype", {description = "Enable freetype interop helpers.", default = false, type = "boolean"})
    add_configs("glib", {description = "Enable glib unicode functions.", default = false, type = "boolean"})

    add_deps("meson", "ninja")

    add_includedirs("include", "include/harfbuzz")

    if is_plat("linux", "bsd") then
        add_syslinks("m")
    elseif is_plat("macosx") then
        add_frameworks("CoreText", "CoreFoundation", "CoreGraphics")
    end

    on_load(function (package)
        if package:config("icu") then
            package:add("deps", "icu4c")
        end
        if package:config("freetype") then
            package:add("deps", "freetype")
        end
        if package:config("glib") then
            package:add("deps", "glib")
        end
    end)

    on_install(function (package)
        local configs = {"-Dtests=disabled", "-Ddocs=disabled", "-Dbenchmark=disabled",
                         "-Dcairo=disabled", "-Dfreetype=disabled", "-Dicu=disabled",
                         "-Dglib=disabled", "-Dgobject=disabled", "-Dcpp_std=c++17"}
        table.insert(configs, "-Ddefault_library=" .. (package:config("shared") and "shared" or "static"))
        import("package.tools.meson").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("hb_buffer_add_utf8", {includes = "hb.h"}))
    end)
