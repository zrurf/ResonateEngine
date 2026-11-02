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
        if (args.Count == 0 || args[0] != "android")
        {
            context.Error.WriteLine("resonate package: expected 'android'");
            return 2;
        }
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
        try
        {
            new AndroidPackager(context, host, plugins, abi, ndk, sdk, sdkVersion).Run();
            return 0;
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
}
