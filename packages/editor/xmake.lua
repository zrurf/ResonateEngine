target("Resonate.Editor")
    set_kind("binary")
    add_files("src/**.cpp")
    add_includedirs(path.join(os.projectdir(), "build", "gen"))

    add_deps("Resonate.Startup", "Resonate.Hierarchy", "Resonate.Render")

    add_packages("mimalloc")
