using System;
using System.Collections.Generic;
using System.IO;
using Resonate.Builder.Packaging;

namespace Resonate.Builder.Commands;

/* Packaging assembles a build the xmake graph produced: the native binaries and
   plugin libraries, the shaders, the cooked content and the Android SDK's own
   format tools. Everything platform-specific is an argument, so the builder
   does not re-read xmake's configuration. */
internal static class PackageCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count == 0)
        {
            context.Error.WriteLine("resonate package: expected 'android' or 'desktop'");
            return 2;
        }
        try
        {
            if (args[0] == "android")
            {
                return Android(context, args);
            }
            if (args[0] == "desktop")
            {
                return Desktop(context, args);
            }
            context.Error.WriteLine($"resonate package: expected 'android' or 'desktop', "
                                    + $"got '{args[0]}'");
            return 2;
        }
        catch (BuilderException error)
        {
            context.Error.WriteLine($"package: {error.Message}");
            return 1;
        }
        catch (IOException error)
        {
            context.Error.WriteLine($"package: {error.Message}");
            return 1;
        }
    }

    private static int Android(BuildContext context, IReadOnlyList<string> args)
    {
        string? host = null;
        string? plugins = null;
        string? abi = null;
        string? ndk = null;
        string? sdk = null;
        string sdkVersion = "29";
        for (int index = 1; index < args.Count; ++index)
        {
            switch (args[index])
            {
                case "--host" when index + 1 < args.Count:
                    host = args[++index];
                    break;
                case "--plugins" when index + 1 < args.Count:
                    plugins = args[++index];
                    break;
                case "--abi" when index + 1 < args.Count:
                    abi = args[++index];
                    break;
                case "--ndk" when index + 1 < args.Count:
                    ndk = args[++index];
                    break;
                case "--sdk" when index + 1 < args.Count:
                    sdk = args[++index];
                    break;
                case "--sdk-version" when index + 1 < args.Count:
                    sdkVersion = args[++index];
                    break;
                default:
                    context.Error.WriteLine($"resonate package: unknown argument '{args[index]}'");
                    return 2;
            }
        }
        if (host is null || plugins is null || abi is null || ndk is null || sdk is null)
        {
            context.Error.WriteLine("resonate package android: --host --plugins --abi --ndk --sdk "
                                    + "are required");
            return 2;
        }
        new AndroidPackager(context, host, plugins, abi, ndk, sdk, sdkVersion).Run();
        return 0;
    }

    private static int Desktop(BuildContext context, IReadOnlyList<string> args)
    {
        string? exe = null;
        string? plugins = null;
        string? outDirectory = null;
        for (int index = 1; index < args.Count; ++index)
        {
            switch (args[index])
            {
                case "--exe" when index + 1 < args.Count:
                    exe = args[++index];
                    break;
                case "--plugins" when index + 1 < args.Count:
                    plugins = args[++index];
                    break;
                case "--out" when index + 1 < args.Count:
                    outDirectory = args[++index];
                    break;
                default:
                    context.Error.WriteLine($"resonate package: unknown argument '{args[index]}'");
                    return 2;
            }
        }
        if (exe is null || plugins is null)
        {
            context.Error.WriteLine("resonate package desktop: --exe --plugins are required; "
                                    + "--out names the tree (default build/package)");
            return 2;
        }
        new DesktopPackager(context, exe, plugins,
                            outDirectory ?? context.BuildPath("package")).Run();
        return 0;
    }
}
