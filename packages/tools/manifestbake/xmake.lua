-- Bakes one TOML manifest into the runtime's binary form. The packagers run it
-- per staged manifest; like texcook it is a host tool, so a cross build builds
-- it for the machine doing the packaging.

target("Resonate.Tools.ManifestBake")
    set_kind("binary")
    set_plat(os.host())
    set_arch(os.arch())
    add_files("src/**.cpp")
    add_packages("toml++")
    add_deps("Resonate.Asset.Meta")
    set_targetdir("$(builddir)/tools", {bindir = ".", libdir = "."})
