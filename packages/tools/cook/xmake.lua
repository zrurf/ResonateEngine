-- Cooks the source content tree into the tree a packaged build ships.
-- Documents go through the binary codec and are reloaded to prove the bytes
-- carry the same tree; everything else the runtime reads is copied. The
-- library half is what the tests call directly.

target("Resonate.Cook")
    set_kind("static")
    set_default(false)
    add_includedirs("include", {public = true})
    add_files("src/cook.cpp")

    -- The generated component headers name ECS handles and math types, so the
    -- include paths those packages publish are needed even though the cook step
    -- never touches a world.
    add_deps("Resonate.Document", "Resonate.Animation", "Resonate.UI", "Resonate.ECS",
             "Resonate.Core.Math")

    -- The schema compositions a document is cooked against are the generated
    -- tables, so the compiler has to have written them before this compiles.
    add_includedirs(path.join(os.projectdir(), "build", "gen"), {public = true})
    on_load(function (target)
        os.mkdir(path.join(os.projectdir(), "build", "gen"))
    end)

    before_build(function (target)
        import("builder")
        builder.run({"schema"})
    end)

target("Resonate.Tools.Cook")
    set_kind("binary")
    set_default(false)
    add_files("src/main.cpp")
    set_targetdir("$(builddir)/tools", {bindir = ".", libdir = "."})

    add_deps("Resonate.Cook")

    after_build(function (target)
        local project = os.projectdir()
        os.execv(target:targetfile(), {
            path.join(project, "content"),
            path.join(project, "build", "cooked"),
        })
    end)
