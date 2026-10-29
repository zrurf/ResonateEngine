-- Cooks the Lucide SVG sources under content/ui/icons into one alpha atlas and
-- the name-to-UV map the UI resolves Icon nodes through. Run with `xmake icons`;
-- the atlas and the map are committed, so a build never writes the source tree.

target("Resonate.Tools.IconCook")
    set_kind("binary")
    set_default(false)
    add_files("src/**.cpp")
    set_targetdir("$(builddir)/tools", {bindir = ".", libdir = "."})
