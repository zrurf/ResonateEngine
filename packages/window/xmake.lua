-- The window, input and window-backed canvas capabilities. Statically linked
-- rather than a plugin because it is not substitutable and nothing needs to
-- replace it at run time.
--
-- Two targets, mirroring packages/module: `.Abi` carries the capability structs a
-- module may require, and this one carries the SDL implementation plus the
-- host-side session. A plugin therefore gets the `Resonate.Window` declaration
-- without linking SDL, and it is not a premise of any package above it any more —
-- a run without a window is headless, so nothing but the startup layer links this.

target("Resonate.Window.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("Resonate.Module.Abi")

target("Resonate.Window")
    set_kind("static")
    add_includedirs("host", {public = true})
    add_files("src/**.cpp")

    -- Render.Abi and not Render: the window publishes a canvas, which is an
    -- interface, so it needs no part of the render package's host side.
    add_deps("Resonate.Window.Abi", "Resonate.PAL", "Resonate.Render.Abi")
    add_packages("libsdl3")
