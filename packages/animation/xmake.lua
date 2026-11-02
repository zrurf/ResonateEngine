-- The animation domain: clip sampling and the player that drives
-- LocalTransform. Clips are plain data the player owns; authored clips are
-- .rclip documents resolved through the products manifest (design note 26).

target("Resonate.Animation")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_includedirs(path.join(os.projectdir(), "build", "gen"))
    add_files("src/**.cpp")

    add_deps("Resonate.ECS", "Resonate.Core.Math", "Resonate.Hierarchy", "Resonate.Runtime")

    -- The Animation component is schema-declared in content/animation. The
    -- hook is idempotent; a second run in one build is a freshness check.
    before_build(function (target)
        import("builder")
        builder.run({"schema"})
    end)
