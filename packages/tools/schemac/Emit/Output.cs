using System.IO;

namespace Resonate.Tools.Schemac.Emit;

internal static class Output
{
    /* Writes only when the bytes differ: a build that changed nothing must not
       touch mtimes, or every downstream tool would see a moving tree. Answers
       whether the file was written. */
    internal static bool WriteIfChanged(string path, string content)
    {
        string? directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        if (File.Exists(path) && File.ReadAllText(path) == content)
        {
            return false;
        }

        File.WriteAllText(path, content);
        return true;
    }
}
