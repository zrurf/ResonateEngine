-- Project-local override of the xmake-repo blake3 package: the recipe's URLs
-- live on the archive host, whose git endpoint is unreachable here. The
-- codeload URL serves the same official release tarball.

package("blake3")
    set_homepage("https://blake3.io/")
    set_description("BLAKE3 cryptographic hash function")
    set_license("CC0-1.0")

    -- The codeload endpoint names the tarball by the ref, so the download needs
    -- an explicit file name for xmake to unpack it.
    add_urls("https://codeload.github.com/BLAKE3-team/BLAKE3/tar.gz/refs/tags/$(version)",
             {filename = "blake3-$(version).tar.gz"})
    add_versions("1.8.7", "c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c")

    on_install(function (package)
        -- The recipe lives in the project's tree; scriptdir() is the project
        -- root for an override.
        os.cp(path.join(os.projectdir(), "packages", "thirdparty", "blake3", "port",
                        "xmake.lua"), "xmake.lua")
        import("package.tools.xmake").install(package)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("blake3_hasher_init", {includes = "blake3.h"}))
    end)
