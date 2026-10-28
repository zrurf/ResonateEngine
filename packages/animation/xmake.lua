-- The animation domain: clip sampling and the player that drives
-- LocalTransform. Clips are plain data the player owns until the asset slice
-- brings a cooked format; the state is a runtime component code creates.

target("Resonate.Animation")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.ECS", "Resonate.Core.Math", "Resonate.Hierarchy")
