target("ResonateEngine.Editor")
    set_kind("binary")
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Startup")

    add_packages("mimalloc")
