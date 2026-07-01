using System.Diagnostics;
using System.IO;

namespace Recorder;

internal static class ShellHelpers
{
    public static void OpenDirectory(string directory)
    {
        if (!Directory.Exists(directory))
            Directory.CreateDirectory(directory);

        Process.Start(new ProcessStartInfo
        {
            FileName = directory,
            UseShellExecute = true,
            Verb = "open",
        });
    }
}
