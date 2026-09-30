-- The render domain: the interfaces a renderer is written against, and the
-- offscreen canvas a host without a window publishes.
--
-- Two targets, mirroring packages/module: `.Abi` carries the capability structs a
-- plugin links, so a renderer or a UI reaches them without linking an
-- implementation, and this package's host-side half is what a host publishes from.
-- The implementation is a plugin under packages/modules, which is what makes the
-- interface the engine's rather than the default implementation's.

target("ResonateEngine.Render.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("ResonateEngine.Module.Abi")

target("ResonateEngine.Render")
    set_kind("static")
    add_includedirs("host", {public = true})
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Render.Abi", "ResonateEngine.Module")
