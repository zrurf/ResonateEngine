-- The gameplay paradigm's engine half: the intent bus and input actions.
-- Depends on the ECS (the world its intents name and its handler commands
-- record into), core's memory and job facilities, and the window's input ABI
-- (the action map samples the frame's raw input without linking a window).

target("Resonate.Gameplay")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.ECS", "Resonate.Core.Memory", "Resonate.Core.Job",
             "Resonate.Window.Abi")
