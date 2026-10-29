-- The document-to-ECS bridge: one component element's text fields written into
-- the schema's packed layout and attached to an entity. Both adapters (UI and
-- scene) instantiate documents through it; it knows no domain vocabulary.

target("Resonate.Document.Bind")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.Document", "Resonate.ECS", "Resonate.Core.Math")
