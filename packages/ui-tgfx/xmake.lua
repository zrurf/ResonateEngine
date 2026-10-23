-- The tgfx UI layer behind a small C ABI. tgfx's debug static library is over
-- a gigabyte once its vendors are merged; linking it into every executable
-- exhausts the linker's memory. One DLL owns it and hosts link the import
-- library instead. Windows only until the NDK build exists.

target("Resonate.UiTgfx")
    set_kind("shared")
    set_default(false)
    add_files("src/**.cpp")
    add_includedirs("include", {public = true})

    -- Always the release tgfx: the boundary is a C ABI, so the DLL can use the
    -- release CRT while the host stays debug. The release archive links within
    -- memory; the debug one does not.
    local tgfx_mode = "release"
    set_runtimes("MD")
    set_optimize("fastest")
    add_includedirs(path.join(os.projectdir(), "build", "third_party", "tgfx", "include"))
    add_linkdirs(path.join(os.projectdir(), "build", "tgfx", tgfx_mode))
    add_links("tgfx")
    add_syslinks("bcrypt", "ws2_32", "dwrite", "windowscodecs", "ole32")

    before_build(function (target)
        local library = path.join(os.projectdir(), "build", "tgfx", tgfx_mode, "tgfx.lib")
        if not os.isfile(library) then
            local script = path.join(os.projectdir(), "scripts", "tgfx.lua")
            local output = os.iorunv(os.programfile(), {"lua", script},
                                     {envs = {RESONATE_TGFX_MODE = tgfx_mode}})
            if output ~= nil then
                print(output)
            end
        end
    end)
