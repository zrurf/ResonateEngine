using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;

namespace Resonate.Builder;

/* Runs one external tool. A .bat is not an executable image on Windows, and
   several SDK tools ship that way, so a batch file goes through cmd.exe. */
internal static class ProcessRunner
{
    internal static int Run(string program, IReadOnlyList<string> arguments,
                            IReadOnlyDictionary<string, string>? environment = null)
    {
        List<string> actual = [.. arguments];
        string executable = program;
        if (OperatingSystem.IsWindows() &&
            program.EndsWith(".bat", StringComparison.OrdinalIgnoreCase))
        {
            executable = "cmd.exe";
            actual.Insert(0, program);
            actual.Insert(0, "/c");
        }
        ProcessStartInfo info = new(executable) { UseShellExecute = false };
        foreach (string argument in actual)
        {
            info.ArgumentList.Add(argument);
        }
        if (environment is not null)
        {
            foreach (KeyValuePair<string, string> variable in environment)
            {
                info.Environment[variable.Key] = variable.Value;
            }
        }
        try
        {
            using Process? process = Process.Start(info);
            if (process is null)
            {
                throw new BuilderException($"{program}: could not be started");
            }
            process.WaitForExit();
            return process.ExitCode;
        }
        catch (Win32Exception error)
        {
            throw new BuilderException($"{program}: {error.Message}");
        }
    }

    internal static void MustRun(string program, IReadOnlyList<string> arguments,
                                 IReadOnlyDictionary<string, string>? environment = null)
    {
        int status = Run(program, arguments, environment);
        if (status != 0)
        {
            throw new BuilderException($"{program} failed with code {status}");
        }
    }
}
