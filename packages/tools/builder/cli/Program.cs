using System;
using System.IO;

namespace Resonate.Builder.Cli;

internal static class Program
{
    private static int Main(string[] args)
    {
        return BuilderProgram.Run(args, Console.Out, Console.Error);
    }
}
