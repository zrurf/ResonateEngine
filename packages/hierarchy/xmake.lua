-- The hierarchy domain: the Parent/Children tree, its edit commands and the
-- transform propagation. It owns the content module content/hierarchy (the
-- schema-declared components) and depends on the ECS and core math only — the
-- frame loop reaches it through an engine-system registration, not a link.

target("Resonate.Hierarchy")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.ECS", "Resonate.Core.Math")

    -- The components are schema-declared: build/gen has to hold their generated
    -- header before this target compiles. The resonate.schema rule's hook fires
    -- only for the target that was requested, and this library is usually built
    -- as somebody's dependency, so it runs the compiler itself — the script is
    -- idempotent, so a second run in one build is just a freshness check.
    add_includedirs(path.join(os.projectdir(), "build", "gen"), {public = true})
    on_load(function (target)
        os.mkdir(path.join(os.projectdir(), "build", "gen"))
    end)

    before_build(function (target)
        import("builder")
        builder.run({"schema"})
    end)
