-- The UI domain's interface: what an immediate-mode context exposes, so a plugin
-- can implement it and a consumer can reach it without linking an implementation.
-- The default implementation is packages/modules/ui.

target("Resonate.UI.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("Resonate.Module.Abi")
