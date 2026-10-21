-- The Android app: the NativeActivity shell around the engine's startup. The
-- root includes this file on Android only.
--
-- Packaging is the `android-apk` task rather than an after_build hook: xmake
-- does not order an order-only dependency's build before the dependent's
-- after_build, so the hook could copy a plugin while its link was still
-- writing. The task runs against a completed build and checks what it needs.

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

target("Resonate.Host.Android")
    set_kind("shared")
    set_filename("libmain.so")
    add_files("src/main.cpp")

    add_deps("Resonate.Startup")

    -- Packaged beside libmain.so, never linked into it: the module host
    -- discovers and loads them at run time.
    add_deps("Resonate.Module.resonate.audio", {links = false, inherit = false})
    add_deps("Resonate.Module.resonate.physics", {links = false, inherit = false})
    add_deps("Resonate.Module.resonate.render-vulkan", {links = false, inherit = false})
    add_deps("Resonate.Module.resonate.ui", {links = false, inherit = false})

    add_syslinks("android", "log")

    on_load(function (target)
        local glue = path.join(ndk_directory(target), "sources", "android", "native_app_glue")
        target:add("files", path.join(glue, "android_native_app_glue.c"))
        target:add("includedirs", glue)
    end)
