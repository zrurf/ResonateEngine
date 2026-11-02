-- Indexes the content tree's identity sidecars into one products manifest.
-- Like texcook, the after_build hook runs the freshly linked tool so building
-- this target leaves the manifest current; the xmake "assets" task is the
-- explicit entry point.

target("Resonate.Tools.AssetIndex")
    set_kind("binary")
    add_files("src/**.cpp")
    add_packages("toml++")
    set_targetdir("$(builddir)/tools", {bindir = ".", libdir = "."})

    add_deps("Resonate.Runtime")

    after_build(function (target)
        local project = os.projectdir()
        os.execv(target:targetfile(), {
            path.join(project, "content"),
            path.join(project, "build", "content"),
        })
    end)
