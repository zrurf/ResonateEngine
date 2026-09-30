-- Owns the frame loop and the module host. It knows nothing about displays: a
-- run wires in what the host provides through two hooks, which is what lets a
-- server, an offscreen renderer and a windowed game share this loop. It therefore
-- links no window and no canvas.

target("ResonateEngine.Runtime")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Module", "ResonateEngine.Core.Job")
