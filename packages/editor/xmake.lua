target("Resonate.Editor")
    set_kind("binary")
    add_files("src/**.cpp")

    add_deps("Resonate.Startup", "Resonate.Hierarchy")

    add_packages("mimalloc")
