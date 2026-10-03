set_project("ResonateEngine")
set_version("0.0.1")
set_xmakever("3.1.1")

add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".compile"})

set_languages("cxxlatest", "clatest")
set_fpmodels("precise")

if is_mode("debug") then
    set_optimize("none")
    -- The ABI promises debug-only checks (capability thread mask, live allocation
    -- reports), so the build has to tell the code which mode it is.
    add_defines("RESONATE_DEBUG")
else
    set_optimize("fastest")
    add_defines("RESONATE_RELEASE")
end

-- A module exports exactly two entry points and nothing else, so on ELF and
-- Mach-O the compiler is told to hide the rest. Windows decides exports by
-- __declspec(dllexport) and has no equivalent switch.
if not is_plat("windows") then
    add_cxflags("-fvisibility=hidden", "-fvisibility-inlines-hidden")
end

if is_plat("windows") then
    set_toolchains("clang-cl[vs=2026]")
    add_defines("RESONATE_PLATFORM_WINDOWS")
    add_defines("_CRT_SECURE_NO_WARNINGS", "NOMINMAX", "WIN32_LEAN_AND_MEAN")
    if is_mode("debug") then
        set_runtimes("MDd")
    else
        set_runtimes("MD")
    end
end

if is_plat("linux") then
    add_defines("RESONATE_PLATFORM_LINUX")
    add_syslinks("dl", "pthread")
end

if is_plat("macosx") then
    add_defines("RESONATE_PLATFORM_MACOS")
    add_frameworks("Foundation", "CoreFoundation", "Metal")
end

if is_plat("android") then
    add_defines("RESONATE_PLATFORM_ANDROID")
    set_config("ndk_cxxstl", "c++_shared")
    set_config("ndk_sdkver", 30)
    add_syslinks("dl", "pthread")
end

if is_plat("iphoneos") then
    add_defines("RESONATE_PLATFORM_IOS")
    add_frameworks("Foundation", "CoreFoundation", "Metal", "UIKit")
    if is_config("appledev", "simulator") then
        add_defines("RESONATE_PLATFORM_SIMULATOR")
    end
end

if is_plat("ohos") then
    add_defines("RESONATE_PLATFORM_HARMONYOS")
    add_syslinks("dl", "pthread")
end

add_requires("catch2 v3.16.0", {verify = false})
add_requires("emhash 1.2.0")
add_requires("libhv 1.3.4")
add_requires("libsdl3 3.4.16", {verify = false})
add_requires("mimalloc v3.5.3", {verify = false})
add_requires("toml++ v3.4.0")
add_requires("xxhash v0.8.4", {verify = false, configs = {cmake = false}})
add_requires("yyjson 0.13.0", {verify = false})

local function tidy_check_output(output)
    return (output:gsub("%\27%[[%d;]*m", ""):gsub("%s*%{%s*%}%s*$", ""):gsub("\n+$", ""))
end

add_rules("resonate.module-check")

local check_done = false

rule("resonate.module-check")
    before_build(function (target)
        if check_done then
            return
        end
        check_done = true

        local script = path.join(os.projectdir(), "scripts", "module_check.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print(tidy_check_output(output))
        end
    end)

task("module-check")
    set_category("plugin")
    on_run(function ()
        local script = path.join(os.projectdir(), "scripts", "module_check.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print(tidy_check_output(output))
        end
    end)
    set_menu {
        usage = "xmake module-check",
        description = "Validate module manifests against descriptors, headers and link dependencies.",
    }

-- Consumed by the targets that include generated schema types. A rule's hook
-- fires only for the target that was requested, not for a dependency built on
-- its way, so a library that is usually reached as a dependency (hierarchy,
-- startup) carries the same call in a target-level before_build of its own.
local schema_done = false

rule("resonate.schema")
    before_build(function (target)
        if schema_done then
            return
        end
        schema_done = true

        local script = path.join(os.projectdir(), "scripts", "schema.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print(tidy_check_output(output))
        end
    end)

task("schema")
    set_category("plugin")
    on_run(function ()
        local script = path.join(os.projectdir(), "scripts", "schema.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print(tidy_check_output(output))
        end
    end)
    set_menu {
        usage = "xmake schema",
        description = "Compile content modules' .rschema declarations into generated C++ and metadata.",
    }

-- The deliberate act that accepts a change to a versioned type: rewrites each
-- module's schemas.lock.json from what the schemas say now. A build only checks
-- the lock, so a version bump or a removed type fails until it is recorded here.
task("schema-lock")
    set_category("plugin")
    on_run(function ()
        local script = path.join(os.projectdir(), "scripts", "schema.lua")
        local code = os.execv(os.programfile(), {"lua", script},
                              {try = true, envs = {RESONATE_SCHEMA_MODE = "update"}})
        if code ~= 0 then
            raise("schema lock update failed (exit %s)", tostring(code))
        end
    end)
    set_menu {
        usage = "xmake schema-lock",
        description = "Record the accepted shape of every versioned schema type (rewrites the locks).",
    }

task("format")
    set_category("plugin")
    on_run(function ()
        local script = path.join(os.projectdir(), "scripts", "format.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print(tidy_check_output(output))
        end
    end)
    set_menu {
        usage = "xmake format",
        description = "Run clang-format over the source tree.",
    }

includes("packages/pal")
includes("packages/core")
includes("packages/ecs")
includes("packages/module")
includes("packages/render")
includes("packages/window")
includes("packages/physics")
includes("packages/audio")
includes("packages/ui")
includes("packages/hierarchy")
includes("packages/runtime")
includes("packages/startup")
includes("packages/editor")
includes("packages/launcher")
includes("packages/modules")
includes("tests")
