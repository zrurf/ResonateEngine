-- Owns the frame loop, the module host and the world the run's systems record
-- into. It knows nothing about displays: a run wires in what the host provides
-- through hooks, which is what lets a server, an offscreen renderer and a
-- windowed game share this loop. It therefore links no window and no canvas.

target("Resonate.Runtime")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.Module", "Resonate.Core.Job", "Resonate.ECS")
