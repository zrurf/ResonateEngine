-- Cooks the P6 sources under content/render/textures into KTX2 files with
-- UASTC Basis payloads. The after_build hook runs the freshly linked tool, so
-- building the renderer (which depends on this target) always leaves the
-- runtime textures in build/content/textures.

target("Resonate.Tools.TexCook")
    set_kind("binary")
    add_files("src/**.cpp")
    add_packages("basisu")
    set_targetdir("$(builddir)/tools", {bindir = ".", libdir = "."})

    after_build(function (target)
        local project = os.projectdir()
        os.execv(target:targetfile(), {
            path.join(project, "content", "render", "textures"),
            path.join(project, "build", "content", "textures"),
        })
    end)
