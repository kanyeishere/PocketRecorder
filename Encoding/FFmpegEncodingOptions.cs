using System;
using System.Collections.Generic;

namespace Recorder.Encoding;

internal static class FFmpegEncodingOptions
{
    public static void AddLowLatencyVideoOptions(ICollection<string> args, string codec)
    {
        if (IsNvenc(codec))
        {
            Add(args,
                "-tune", "ull",
                "-bf", "0",
                "-rc-lookahead", "0",
                "-delay", "0",
                "-zerolatency", "1");
            return;
        }

        if (IsAmf(codec))
        {
            Add(args,
                "-usage", "ultralowlatency",
                "-preanalysis", "0",
                "-async_depth", "1");

            if (codec.Equals("h264_amf", StringComparison.OrdinalIgnoreCase))
                Add(args, "-bf", "0");
            return;
        }

        if (IsQsv(codec))
        {
            Add(args,
                "-async_depth", "1",
                "-look_ahead", "0",
                "-bf", "0");
            return;
        }

        if (codec.Equals("libx264", StringComparison.OrdinalIgnoreCase))
        {
            Add(args,
                "-tune", "zerolatency",
                "-x264-params", "bframes=0:sync-lookahead=0");
            return;
        }

        if (codec.Equals("libx265", StringComparison.OrdinalIgnoreCase))
        {
            Add(args,
                "-tune", "zerolatency",
                "-x265-params", "bframes=0:rc-lookahead=0");
        }
    }

    private static bool IsNvenc(string codec)
        => codec.EndsWith("_nvenc", StringComparison.OrdinalIgnoreCase);

    private static bool IsAmf(string codec)
        => codec.EndsWith("_amf", StringComparison.OrdinalIgnoreCase);

    private static bool IsQsv(string codec)
        => codec.EndsWith("_qsv", StringComparison.OrdinalIgnoreCase);

    private static void Add(ICollection<string> args, params string[] values)
    {
        foreach (string value in values)
            args.Add(value);
    }
}
