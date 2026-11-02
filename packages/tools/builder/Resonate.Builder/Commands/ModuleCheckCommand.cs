using System.Collections.Generic;
using Resonate.Builder.ModuleCheck;

namespace Resonate.Builder.Commands;

/* Validates every module contract against the descriptor, the capability
   headers and the build table. The checker owns the rules; this is the
   command-line edge. */
internal static class ModuleCheckCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate module-check: unknown argument '{args[0]}'");
            return 2;
        }
        return new Checker(context).Execute();
    }
}
