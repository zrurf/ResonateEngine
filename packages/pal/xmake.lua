-- PAL is one static library with two backends. Windows has its own set (IOCP,
-- Win32 threads, overlapped file handles); everything else shares the POSIX
-- implementation, whose only platform-specific part is the event-loop wakeup
-- (eventfd where the kernel has it, a pipe otherwise).
target("ResonateEngine.PAL")
    set_kind("static")
    add_includedirs("include", {public = true})

    -- Private include root, so a backend can include a shared private header
    -- (common/...) without a path that walks up out of its own directory.
    add_includedirs("src")

    -- Primitives the standard library provides identically everywhere.
    add_files("src/common/*.cpp")

    if is_plat("windows") then
        add_files("src/platform/os/windows/**.cpp")
        add_syslinks("user32", "kernel32")
    else
        add_files("src/common/posix/**.cpp")
        add_syslinks("pthread", "dl")
    end

    add_packages("libhv")
