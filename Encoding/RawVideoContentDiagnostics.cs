using Recorder.Capture;
using System;
using System.Globalization;

namespace Recorder.Encoding;

internal sealed class RawVideoContentDiagnostics
{
    private const int ProbeIntervalFrames = 300;
    private const int GridColumns = 64;
    private const int GridRows = 36;

    private long _framesObserved;
    private long _probesAttempted;
    private long _probesCompleted;
    private long _invalidLayouts;
    private long _analysisFailures;
    private long _blackProbes;
    private long _hashChanges;
    private long _currentSameHashStreak;
    private long _maxSameHashStreak;
    private ulong _lastHash;
    private long _lastProbeFrame;
    private bool _hasHash;
    private string _lastSample = "none";

    public bool HasCompletedProbe => _probesCompleted > 0;
    public bool HasBlackProbe => _blackProbes > 0;
    public bool IsAllBlack => _probesCompleted > 0 && _blackProbes == _probesCompleted;

    public string Health => _probesCompleted == 0
        ? "no-probe"
        : IsAllBlack
            ? "all-black"
            : HasBlackProbe ? "intermittent-black" : "content-present";

    public void Reset()
    {
        _framesObserved = 0;
        _probesAttempted = 0;
        _probesCompleted = 0;
        _invalidLayouts = 0;
        _analysisFailures = 0;
        _blackProbes = 0;
        _hashChanges = 0;
        _currentSameHashStreak = 0;
        _maxSameHashStreak = 0;
        _lastHash = 0;
        _lastProbeFrame = 0;
        _hasHash = false;
        _lastSample = "none";
    }

    public bool Observe(
        ReadOnlySpan<byte> data,
        int width,
        int height,
        VideoPixelFormat pixelFormat,
        long frameIndex)
    {
        _framesObserved++;
        if (frameIndex != 1 && frameIndex % ProbeIntervalFrames != 0)
            return false;

        _probesAttempted++;
        try
        {
            if (width <= 0 || height <= 0 || data.IsEmpty)
            {
                _invalidLayouts++;
                return true;
            }

            return pixelFormat switch
            {
                VideoPixelFormat.Bgra or VideoPixelFormat.Rgba =>
                    AnalyzeRgb(data, width, height, pixelFormat, frameIndex),
                VideoPixelFormat.Nv12 => AnalyzeNv12(data, width, height, frameIndex),
                _ => MarkInvalidLayout(),
            };
        }
        catch
        {
            _analysisFailures++;
            return true;
        }
    }

    public string BuildSummary()
        => $"rawInputHealth={Health},framesObserved={_framesObserved},probesAttempted={_probesAttempted}," +
           $"probesCompleted={_probesCompleted},invalidLayouts={_invalidLayouts},analysisFailures={_analysisFailures}," +
           $"blackProbes={_blackProbes},hashChanges={_hashChanges},maxSameHashStreak={_maxSameHashStreak}," +
           $"lastHash=0x{_lastHash.ToString("X16", CultureInfo.InvariantCulture)},lastProbeFrame={_lastProbeFrame}," +
           $"lastSample=[{_lastSample}]";

    private bool AnalyzeRgb(
        ReadOnlySpan<byte> data,
        int width,
        int height,
        VideoPixelFormat pixelFormat,
        long frameIndex)
    {
        long requiredBytes = (long)width * height * 4;
        if (requiredBytes <= 0 || requiredBytes > data.Length)
            return MarkInvalidLayout();

        ulong hash = 1469598103934665603UL;
        long sumR = 0;
        long sumG = 0;
        long sumB = 0;
        int minR = 255;
        int minG = 255;
        int minB = 255;
        int maxR = 0;
        int maxG = 0;
        int maxB = 0;
        int maxLuma = 0;
        int samples = 0;

        for (int row = 0; row < GridRows; row++)
        {
            int y = Math.Min(height - 1, ((row * height) + (height / 2)) / GridRows);
            for (int column = 0; column < GridColumns; column++)
            {
                int x = Math.Min(width - 1, ((column * width) + (width / 2)) / GridColumns);
                int offset = checked(((y * width) + x) * 4);
                int r = pixelFormat == VideoPixelFormat.Rgba ? data[offset] : data[offset + 2];
                int g = data[offset + 1];
                int b = pixelFormat == VideoPixelFormat.Rgba ? data[offset + 2] : data[offset];

                sumR += r;
                sumG += g;
                sumB += b;
                minR = Math.Min(minR, r);
                minG = Math.Min(minG, g);
                minB = Math.Min(minB, b);
                maxR = Math.Max(maxR, r);
                maxG = Math.Max(maxG, g);
                maxB = Math.Max(maxB, b);
                maxLuma = Math.Max(maxLuma, ((54 * r) + (183 * g) + (19 * b)) >> 8);
                HashByte(ref hash, (byte)r);
                HashByte(ref hash, (byte)g);
                HashByte(ref hash, (byte)b);
                HashByte(ref hash, data[offset + 3]);
                samples++;
            }
        }

        int avgR = (int)(sumR / samples);
        int avgG = (int)(sumG / samples);
        int avgB = (int)(sumB / samples);
        _lastSample =
            $"R(avg/min/max)={avgR}/{minR}/{maxR},G={avgG}/{minG}/{maxG},B={avgB}/{minB}/{maxB}";
        CompleteProbe(frameIndex, hash, maxLuma <= 8);
        return true;
    }

    private bool AnalyzeNv12(ReadOnlySpan<byte> data, int width, int height, long frameIndex)
    {
        long yPlaneBytes = (long)width * height;
        long requiredBytes = yPlaneBytes + (yPlaneBytes / 2);
        if (width < 2 || height < 2 || (width & 1) != 0 || (height & 1) != 0 ||
            requiredBytes <= 0 || requiredBytes > data.Length)
        {
            return MarkInvalidLayout();
        }

        ulong hash = 1469598103934665603UL;
        long sumY = 0;
        long sumU = 0;
        long sumV = 0;
        int minY = 255;
        int minU = 255;
        int minV = 255;
        int maxY = 0;
        int maxU = 0;
        int maxV = 0;
        int samples = 0;
        int uvBase = checked((int)yPlaneBytes);

        for (int row = 0; row < GridRows; row++)
        {
            int y = Math.Min(height - 1, ((row * height) + (height / 2)) / GridRows);
            for (int column = 0; column < GridColumns; column++)
            {
                int x = Math.Min(width - 1, ((column * width) + (width / 2)) / GridColumns);
                int uvX = Math.Min(width - 2, x & ~1);
                int yValue = data[(y * width) + x];
                int uvOffset = uvBase + ((y / 2) * width) + uvX;
                int uValue = data[uvOffset];
                int vValue = data[uvOffset + 1];

                sumY += yValue;
                sumU += uValue;
                sumV += vValue;
                minY = Math.Min(minY, yValue);
                minU = Math.Min(minU, uValue);
                minV = Math.Min(minV, vValue);
                maxY = Math.Max(maxY, yValue);
                maxU = Math.Max(maxU, uValue);
                maxV = Math.Max(maxV, vValue);
                HashByte(ref hash, (byte)yValue);
                HashByte(ref hash, (byte)uValue);
                HashByte(ref hash, (byte)vValue);
                samples++;
            }
        }

        int avgY = (int)(sumY / samples);
        int avgU = (int)(sumU / samples);
        int avgV = (int)(sumV / samples);
        _lastSample =
            $"Y(avg/min/max)={avgY}/{minY}/{maxY},U={avgU}/{minU}/{maxU},V={avgV}/{minV}/{maxV}";
        CompleteProbe(frameIndex, hash, maxY <= 20);
        return true;
    }

    private void CompleteProbe(long frameIndex, ulong hash, bool isBlack)
    {
        if (isBlack)
            _blackProbes++;

        if (_hasHash && hash == _lastHash)
        {
            _currentSameHashStreak++;
        }
        else
        {
            if (_hasHash)
                _hashChanges++;
            _currentSameHashStreak = 1;
        }

        _maxSameHashStreak = Math.Max(_maxSameHashStreak, _currentSameHashStreak);
        _lastHash = hash;
        _lastProbeFrame = frameIndex;
        _hasHash = true;
        _probesCompleted++;
    }

    private bool MarkInvalidLayout()
    {
        _invalidLayouts++;
        return true;
    }

    private static void HashByte(ref ulong hash, byte value)
    {
        hash ^= value;
        hash *= 1099511628211UL;
    }
}
