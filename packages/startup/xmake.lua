-- The startup the launcher and the editor share. It sits above the runtime and the
-- window package because it is the only layer that knows about both: the runtime
-- keeps its frame loop free of displays, and the window package keeps the display
-- free of the frame loop.

target("ResonateEngine.Startup")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Runtime", "ResonateEngine.Window", "ResonateEngine.Render")
