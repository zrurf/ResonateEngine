-- The physics domain's interface: what a simulation world exposes, so a plugin
-- can implement it and a consumer can reach it without linking an implementation.
-- The default implementation is packages/modules/physics.

target("ResonateEngine.Physics.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("ResonateEngine.Module.Abi")
