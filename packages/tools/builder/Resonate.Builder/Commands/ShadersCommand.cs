using System.Collections.Generic;
using Resonate.Builder.Tooling;

namespace Resonate.Builder.Commands;

internal static class ShadersCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate shaders: unknown argument '{args[0]}'");
            return 2;
        }
        return ShaderCompiler.Run(context);
    }
}
