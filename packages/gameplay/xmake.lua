-- The gameplay paradigm's engine half: the intent bus. Depends on the ECS (the
-- world its intents name and its handler commands record into) and core's
-- memory and job facilities; the module host owns it and drains it at the
-- frame's sync points.

target("Resonate.Gameplay")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.ECS", "Resonate.Core.Memory", "Resonate.Core.Job")
