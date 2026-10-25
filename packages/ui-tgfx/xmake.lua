-- The tgfx UI layer behind a small C ABI. On Windows tgfx's debug static
-- library is over a gigabyte once its vendors are merged and linking it into
-- every executable exhausts the linker's memory, so one DLL owns it and hosts
-- link the import library instead. On Android tgfx builds as libtgfx.so (the
-- static archive would exceed the Windows command line), so the wrapper is a
-- shared object beside the host and the APK packages both.

local plugin_dir = "$(builddir)/plugins"

target("Resonate.UiTgfx")
    set_default(false)
    set_kind("shared")
    add_files("src/**.cpp")
    add_includedirs("include", {public = true})
    add_includedirs(path.join(os.projectdir(), "build", "third_party", "tgfx", "include"))

    -- Always the release tgfx: the boundary is a C ABI, so the library can use
    -- the release runtime while the host stays debug. The release archive
    -- links within memory; the debug one does not.
    local tgfx_mode = "release"
    local tgfx_root = path.join(os.projectdir(), "build", "tgfx")
    local tgfx_dir = nil
    local tgfx_lib = nil

    if is_plat("android") then
        tgfx_dir = path.join(tgfx_root, "android", tgfx_mode)
        tgfx_lib = path.join(tgfx_dir, "libtgfx.so")
        -- tgfx's Vulkan backend reaches the loader through the engine's
        -- libvulkan link; log and android are its own platform deps. Public so
        -- the final host link inherits them.
        add_syslinks("log", "android", {public = true})
        -- Beside the plugin libraries, so the APK's flat lib/ layout carries
        -- libtgfx.so and this wrapper next to the host.
        set_targetdir(plugin_dir, {bindir = ".", libdir = "."})
    else
        set_runtimes("MD")
        set_optimize("fastest")
        tgfx_dir = path.join(tgfx_root, tgfx_mode)
        tgfx_lib = path.join(tgfx_dir, "tgfx.lib")
        add_syslinks("bcrypt", "ws2_32", "dwrite", "windowscodecs", "ole32")
    end
    add_linkdirs(tgfx_dir)
    add_links("tgfx")

    before_build(function (target)
        if not os.isfile(tgfx_lib) then
            local script = path.join(os.projectdir(), "scripts", "tgfx.lua")
            local output = os.iorunv(os.programfile(), {"lua", script},
                                     {envs = {RESONATE_TGFX_MODE = tgfx_mode}})
            if output ~= nil then
                print(output)
            end
        end
    end)
