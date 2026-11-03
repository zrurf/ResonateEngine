set_project("ResonateEngine")
set_version("0.0.1")
set_xmakever("3.1.1")

-- Top level on purpose: a platform-guarded set_config does not reach a fresh
-- configure, and NDK r28's default API (21) predates libvulkan (24). The
-- reference device is Android 10 (API 29).
set_config("ndk_sdkver", 29)

add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".compile"})

add_moduledirs("xmake/modules")

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
    -- The app is one shared library around static archives, so every object
    -- has to be position independent.
    add_cxflags("-fPIC")
    -- Crash reports walk the frame chain; without frame pointers a device run
    -- can only name the faulting function, not its callers.
    add_cxflags("-fno-omit-frame-pointer")
    -- Bionic keeps pthread and dl in libc; there are no separate libs.
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

-- Project-local package: the upstream source downloads from GitHub, which is
-- unreachable here; the Gitee mirror is used instead.
includes("packages/thirdparty/harfbuzz")
includes("packages/thirdparty/basisu")

-- The engine libraries and modules alone: no window, editor, launcher or
-- tests. A cross-compiled build (Android) uses this to skip the host-only
-- tools and the display stack.
option("engine_only")
    set_default(false)
    set_showmenu(true)
    set_description("Build the engine libraries and modules only (no window, tools, editor or tests).")
option_end()

add_requires("basisu 1.16.4")
add_requires("harfbuzz 13.1.1", {configs = {freetype = false, icu = false, glib = false}})
add_requires("toml++ v3.4.0")
add_requires("xxhash v0.8.4", {verify = false, configs = {cmake = false}})
add_requires("blake3 1.8.7", {verify = false})
if not get_config("engine_only") then
    add_requires("catch2 v3.16.0", {verify = false})
    add_requires("libsdl3 3.4.16", {verify = false})
    add_requires("mimalloc v3.5.3", {verify = false})
    add_requires("yyjson 0.13.0", {verify = false})
end

-- Vulkan comes from the installed SDK rather than vendored sources: the SDK
-- carries the headers, the loader import library, the validation layers and the
-- shader toolchain (slangc). The environment variable the installer sets is the
-- default; --vulkan_sdk=<dir> overrides it.
option("vulkan_sdk")
    set_showmenu(true)
    set_default("")
    set_description("Vulkan SDK root directory; defaults to $VULKAN_SDK")

local vulkan_sdk = get_config("vulkan_sdk")
if vulkan_sdk == nil or vulkan_sdk == "" then
    vulkan_sdk = os.getenv("VULKAN_SDK")
end
if vulkan_sdk == nil or vulkan_sdk == "" then
    raise("no Vulkan SDK: set the VULKAN_SDK environment variable or pass --vulkan_sdk=<dir>")
end

target("Resonate.Vulkan")
    set_kind("headeronly")
    -- VMA calls Vulkan 1.3 entry points directly when the SDK headers are newer
    -- than the API the app targets; an older loader then refuses the library at
    -- load time. Pin it to the 1.1 baseline this renderer asks for.
    add_defines("VMA_VULKAN_VERSION=1001000", {public = true})
    if is_plat("android") then
        -- libvulkan.so comes from the NDK sysroot; the SDK include carries the
        -- same Khronos headers and VMA beside them.
        add_includedirs(path.join(vulkan_sdk, "Include"), {public = true})
        add_defines("VK_USE_PLATFORM_ANDROID_KHR", {public = true})
        add_syslinks("vulkan", {public = true})
    elseif is_plat("windows") then
        add_includedirs(path.join(vulkan_sdk, "Include"), {public = true})
        add_defines("VK_USE_PLATFORM_WIN32_KHR", {public = true})
        add_linkdirs(path.join(vulkan_sdk, "Lib"), {public = true})
        add_links("vulkan-1", {public = true})
    else
        add_syslinks("vulkan", {public = true})
    end

add_rules("resonate.module-check")

local check_done = false

rule("resonate.module-check")
    before_build(function (target)
        if check_done then
            return
        end
        check_done = true
        import("builder")
        builder.run({"module-check"})
    end)

task("module-check")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"module-check"})
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
        import("builder")
        builder.run({"schema"})
    end)

task("schema")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"schema"})
    end)
    set_menu {
        usage = "xmake schema",
        description = "Compile content modules' .rschema declarations into generated C++ and metadata.",
    }

task("csharp-demo")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"csharp-demo"})
    end)
    set_menu {
        usage = "xmake csharp-demo",
        description = "Publish the sample managed game into build/content/csharp/demo.",
    }

task("analyzers")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"analyzers"})
    end)
    set_menu {
        usage = "xmake analyzers",
        description = "Build and run the behaviour analyzers' self-checks.",
    }

task("slang")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"shaders"}, {RESONATE_VULKAN_SDK = vulkan_sdk})
    end)
    set_menu {
        usage = "xmake slang",
        description = "Compile the modules' Slang shaders to SPIR-V and reflection JSON.",
    }

task("tgfx")
    set_category("plugin")
    on_run(function ()
        import("core.project.config")
        config.load()
        local plat = config.get("plat") or os.host()
        local mode = os.getenv("RESONATE_TGFX_MODE") or config.get("mode") or "debug"
        local ndk = config.get("ndk") or os.getenv("ANDROID_NDK_HOME")
            or os.getenv("ANDROID_NDK_ROOT")
        local args = {"tgfx", "--platform", plat, "--mode", mode,
                      "--sdk-version", tostring(config.get("ndk_sdkver") or 30)}
        if ndk ~= nil and ndk ~= "" then
            table.insert(args, "--ndk")
            table.insert(args, ndk)
        end
        if config.get("arch") ~= nil and config.get("arch") ~= "" then
            table.insert(args, "--arch")
            table.insert(args, config.get("arch"))
        end
        import("builder")
        builder.run(args)
    end)
    set_menu {
        usage = "xmake tgfx",
        description = "Fetch and build tgfx with its Vulkan backend into build/tgfx.",
    }

-- The deliberate act that accepts a change to a versioned type: rewrites each
-- module's schemas.lock.json from what the schemas say now. A build only checks
-- the lock, so a version bump or a removed type fails until it is recorded here.
task("schema-lock")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"schema", "--update-lock"})
    end)
    set_menu {
        usage = "xmake schema-lock",
        description = "Record the accepted shape of every versioned schema type (rewrites the locks).",
    }

task("format")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"format"})
    end)
    set_menu {
        usage = "xmake format",
        description = "Run clang-format over the source tree.",
    }

task("ecs-bench")
    set_category("plugin")
    on_run(function ()
        os.execv(os.programfile(), {"build", "-y", "Resonate.Bench.ECS"})
        os.execv(os.programfile(), {"run", "Resonate.Bench.ECS"})
    end)
    set_menu {
        usage = "xmake ecs-bench",
        description = "Run the ECS acceptance bench.",
    }

task("csharp-bench")
    set_category("plugin")
    on_run(function ()
        os.execv(os.programfile(), {"build", "-y", "Resonate.Bench.CSharp"})
        os.execv(os.programfile(), {"run", "Resonate.Bench.CSharp"})
    end)
    set_menu {
        usage = "xmake csharp-bench",
        description = "Run the C# FFI-budget bench.",
    }

task("csharp-reload")
    set_category("plugin")
    on_run(function ()
        os.execv(os.programfile(), {"build", "-y", "Resonate.Bench.Reload"})
        os.execv(os.programfile(), {"run", "Resonate.Bench.Reload"})
    end)
    set_menu {
        usage = "xmake csharp-reload",
        description = "Run the managed hot-reload acceptance bench.",
    }

task("assets")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"assets"})
    end)
    set_menu {
        usage = "xmake assets",
        description = "Index the content tree's identity sidecars into the products manifest.",
    }

task("texcook")
    set_category("plugin")
    on_run(function ()
        os.execv(os.programfile(), {"build", "-y", "Resonate.Tools.TexCook"})
    end)
    set_menu {
        usage = "xmake texcook",
        description = "Cook the source textures into the runtime KTX2 container.",
    }

task("builder-check")
    set_category("plugin")
    on_run(function ()
        import("builder")
        builder.run({"selfcheck"})
    end)
    set_menu {
        usage = "xmake builder-check",
        description = "Run the C# builder's own checks (BLAKE3 vectors, identity rules).",
    }

task("cook")
    set_category("plugin")
    on_run(function ()
        os.execv(os.programfile(), {"build", "-y", "Resonate.Tools.Cook"})
    end)
    set_menu {
        usage = "xmake cook",
        description = "Cook the content tree into the tree a packaged build ships.",
    }

task("icons")
    set_category("plugin")
    on_run(function ()
        os.execv(os.programfile(), {"build", "-y", "Resonate.Tools.IconCook"})
        local project = os.projectdir()
        os.execv(os.programfile(),
                 {"run", "Resonate.Tools.IconCook",
                  path.join(project, "content", "ui", "icons"),
                  path.join(project, "content", "ui", "images", "icons.png"),
                  path.join(project, "content", "ui", "icons.icons")})
    end)
    set_menu {
        usage = "xmake icons",
        description = "Cook content/ui/icons into the shipped icon atlas and its UV map.",
    }

-- The suite task refuses a configuration with no test targets: `xmake test`
-- under the Android configuration prints "nothing to test" and exits 0, which
-- reads as green in a scripted loop. The platform is read from the config
-- because is_plat() is not populated in a task scope.
task("suite")
    set_category("plugin")
    on_run(function ()
        import("core.project.config")
        config.load()
        local plat = config.get("plat") or os.host()
        if not (plat == "windows" or plat == "linux" or plat == "macosx") then
            raise("switch to a host platform first (xmake f -p windows -a x64 -m debug -y)")
        end
        import("builder")
        builder.run({"selfcheck"})
        os.execv(os.programfile(), {"test", "-j1"})
    end)
    set_menu {
        usage = "xmake suite",
        description = "Run the full test suite on the host platform, refusing a configuration that has none.",
    }

task("android-apk")
    set_category("plugin")
    on_run(function ()
        import("core.project.config")
        import("core.project.project")
        config.load()


        local host = project.target("Resonate.Host.Android")
        local module = project.target("Resonate.Module.resonate.audio")
        if host == nil or module == nil then
            raise("configure for Android first: xmake f -p android ...")
        end
        local host_file = path.join(host:targetdir(), host:filename())
        if not os.isfile(host_file) then
            raise("build the host first: xmake build Resonate.Host.Android")
        end
        local plugins = path.absolute(module:targetdir())
        local ndk = config.get("ndk")
        if ndk == nil or ndk == "" then
            ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK_ROOT")
        end
        if ndk == nil or ndk == "" then
            raise("no NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME")
        end
        local sdk = os.getenv("ANDROID_HOME") or os.getenv("ANDROID_SDK_ROOT")
        if sdk == nil or sdk == "" then
            sdk = path.directory(path.directory(ndk))
        end
        import("builder")
        builder.run({"--build", path.directory(plugins), "package", "android",
                     "--host", host_file, "--plugins", plugins, "--abi", host:arch(),
                     "--ndk", ndk, "--sdk", sdk,
                     "--sdk-version", tostring(config.get("ndk_sdkver") or "29")})
    end)
    set_menu {
        usage = "xmake android-apk",
        description = "Package the built Android host and modules into resonate.apk.",
    }

task("desktop-package")
    set_category("plugin")
    on_run(function ()
        import("core.project.config")
        import("core.project.project")
        config.load()


        local launcher = project.target("Resonate.Launcher")
        local module = project.target("Resonate.Module.resonate.audio")
        if launcher == nil or module == nil then
            raise("no launcher or module targets; configure a desktop platform first")
        end
        local exe = launcher:targetfile()
        if not os.isfile(exe) then
            raise("build the launcher first: xmake build Resonate.Launcher")
        end
        import("builder")
        builder.run({"package", "desktop", "--exe", exe, "--plugins",
                     path.absolute(module:targetdir())})
    end)
    set_menu {
        usage = "xmake desktop-package",
        description = "Assemble the shipped desktop tree under build/package.",
    }

includes("packages/pal")
includes("packages/window")
includes("packages/core")
includes("packages/document")
includes("packages/document-bind")
includes("packages/ecs")
includes("packages/gameplay")
includes("packages/module")
includes("packages/render")
includes("packages/physics")
includes("packages/audio")
includes("packages/text")
includes("packages/ui")
if is_plat("windows", "android") then
    includes("packages/ui-tgfx")
end
includes("packages/hierarchy")
includes("packages/scene")
includes("packages/animation")
includes("packages/renderer")
includes("packages/runtime")
includes("packages/modules")

-- Startup composes the display stack and the frame loop; on Android the display
-- comes from the NativeActivity shell, so the engine-only build still carries it.
if not get_config("engine_only") or is_plat("android") then
    includes("packages/startup")
end
if is_plat("android") then
    includes("packages/host/android")
end
if not get_config("engine_only") then
    includes("packages/csharp")
    includes("packages/tools/texcook")
    includes("packages/tools/manifestbake")
    includes("packages/tools/iconcook")
    includes("packages/tools/cook")
    includes("packages/editor")
    includes("packages/launcher")
    includes("tests")
end
