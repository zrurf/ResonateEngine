-- The window, input and window-backed canvas capabilities. Statically linked
-- rather than a plugin because it is not substitutable and nothing needs to
-- replace it at run time.
--
-- Two targets, mirroring packages/module: `.Abi` carries the capability structs a
-- module may require, and this one carries the platform implementation plus the
-- host-side session. A plugin therefore gets the `Resonate.Window` declaration
-- without linking SDL, and it is not a premise of any package above it any more —
-- a run without a window is headless, so nothing but the startup layer links this.
--
-- The session is shared; the backend below it is SDL on desktop platforms and the
-- NDK's NativeActivity glue on Android.

target("Resonate.Window.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("Resonate.Module.Abi")

-- The NDK is a platform setting rather than a project option. The toolchain
-- carries the configured path; the environment is the fallback.
local function ndk_directory(target)
    local toolchain = target:toolchain("ndk")
    local ndk = toolchain and toolchain:config("ndk")
    if ndk == nil or ndk == "" then
        ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK_ROOT")
    end
    if ndk == nil or ndk == "" then
        raise("no NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME")
    end
    return ndk
end

if not get_config("engine_only") or is_plat("android") then
    target("Resonate.Window")
        set_kind("static")
        add_includedirs("host", {public = true})
        add_files("src/session.cpp")

        -- Render.Abi and not Render: the window publishes a canvas, which is an
        -- interface, so it needs no part of the render package's host side.
        add_deps("Resonate.Window.Abi", "Resonate.PAL", "Resonate.Render.Abi")

        if is_plat("android") then
            add_files("src/android/**.cpp")
            add_syslinks("android", "log")
            on_load(function (target)
                local glue = path.join(ndk_directory(target), "sources", "android", "native_app_glue")
                target:add("includedirs", glue)
            end)
        else
            add_files("src/sdl/**.cpp")
            add_packages("libsdl3")
        end
end
