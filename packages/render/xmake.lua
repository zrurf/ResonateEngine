-- The render domain: the interfaces a renderer is written against, and the
-- offscreen canvas a host without a window publishes.
--
-- Three targets, mirroring packages/module: `.Abi` carries the capability
-- structs a plugin links, `.Graph` carries the render graph and its executor —
-- pure logic over the device ABI, no host dependency, which is what lets a
-- render backend plugin link them — and this package's host-side half is what
-- a host publishes from. The implementation is a plugin under packages/modules,
-- which is what makes the interface the engine's rather than the default
-- implementation's.

target("Resonate.Render.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("Resonate.Module.Abi")

target("Resonate.Render.Graph")
    set_kind("static")
    add_files("src/graph.cpp", "src/executor.cpp")

    add_deps("Resonate.Render.Abi")

target("Resonate.Render")
    set_kind("static")
    add_includedirs("host", {public = true})
    -- The graph and the executor compile once, into Resonate.Render.Graph.
    add_files("src/offscreen.cpp")

    add_deps("Resonate.Render.Abi", "Resonate.Render.Graph", "Resonate.Module")
