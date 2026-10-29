-- The scene adapter: a .rscene document instantiated into entities. Nesting is
-- parenting, an Object's key is its stable id, its name the display name, and
-- its components apply through the schema descriptors of whatever registry the
-- host passes -- the scene schema names no component layout of its own.

target("Resonate.Scene")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.Document", "Resonate.Document.Bind", "Resonate.ECS",
             "Resonate.Hierarchy", "Resonate.Core.Math")
