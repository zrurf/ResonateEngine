-- Owns the frame loop, the module host and the world the run's systems record
-- into. It knows nothing about displays: a run wires in what the host provides
-- through hooks, which is what lets a server, an offscreen renderer and a
-- windowed game share this loop. It therefore links no window and no canvas.

-- The asset identity slice: sidecars and the products manifest. Host tools
-- (texcook) link this without the whole runtime.
target("Resonate.Asset.Meta")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/asset_identity.cpp", "src/meta.cpp")
    add_packages("toml++")

target("Resonate.Runtime")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")
    remove_files("src/asset_identity.cpp", "src/meta.cpp")

    add_packages("toml++")

    add_deps("Resonate.Module", "Resonate.Core.Job", "Resonate.ECS", "Resonate.Core.Hash",
             "Resonate.Document", "Resonate.Asset.Meta")
