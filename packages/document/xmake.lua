-- The generic document kernel: node tree, identity, components and the XML
-- text codec. It knows no domain vocabulary; scene/UI adapters instantiate it.
-- It deliberately depends on core containers only, so it can be used from the
-- cook tool, the editor and the runtime loader alike.

target("Resonate.Document")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.Core.Container")
