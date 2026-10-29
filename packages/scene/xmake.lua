-- The scene adapter: a .rscene document instantiated into entities. Nesting is
-- parenting, an Object's key is its stable id, its name the display name, and
-- its components apply through the schema descriptors of whatever registry the
-- host passes -- the scene schema names no component layout of its own.

target("Resonate.Scene")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.Document", "Resonate.Document.Bind", "Resonate.ECS",
             "Resonate.Hierarchy", "Resonate.Render", "Resonate.Runtime",
             "Resonate.Core.Math")

    -- The reader attaches the scene module's SourceKey; the generated header
    -- must exist before this target compiles.
    add_includedirs(path.join(os.projectdir(), "build", "gen"))
    on_load(function (target)
        os.mkdir(path.join(os.projectdir(), "build", "gen"))
    end)
    add_rules("resonate.schema")
