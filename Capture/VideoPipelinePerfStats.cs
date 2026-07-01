using System;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace Recorder.Capture;

internal sealed class VideoPipelinePerfStats
{
    private const int DefaultWindowSize = 300;

    private readonly string _label;
    private readonly string[] _metricNames;
    private readonly long[][] _samples;
    private readonly long[] _sampleSums;
    private readonly int _windowSize;

    private int _sampleCount;
    private long _totalSamples;
    private long _windowCount;
    private int _width;
    private int _height;
    private int _dataLength;
    private VideoPixelFormat _pixelFormat;

    public VideoPipelinePerfStats(string label, params string[] metricNames)
        : this(label, DefaultWindowSize, metricNames)
    {
    }

    public VideoPipelinePerfStats(string label, int windowSize, params string[] metricNames)
    {
        if (metricNames.Length == 0)
            throw new ArgumentException("At least one metric is required.", nameof(metricNames));

        _label = label;
        _metricNames = metricNames;
        _windowSize = Math.Max(1, windowSize);
        _samples = new long[metricNames.Length][];
        _sampleSums = new long[metricNames.Length];
        for (int i = 0; i < _samples.Length; i++)
            _samples[i] = new long[_windowSize];
    }

    public void Reset()
    {
        _sampleCount = 0;
        _totalSamples = 0;
        _windowCount = 0;
        Array.Clear(_sampleSums, 0, _sampleSums.Length);
    }

    public void Record(
        int width,
        int height,
        int dataLength,
        VideoPixelFormat pixelFormat,
        ReadOnlySpan<long> elapsedTicks)
    {
        if (elapsedTicks.Length != _metricNames.Length)
            throw new ArgumentException("Metric count mismatch.", nameof(elapsedTicks));

        int index = _sampleCount;
        _width = width;
        _height = height;
        _dataLength = dataLength;
        _pixelFormat = pixelFormat;

        for (int i = 0; i < _samples.Length; i++)
        {
            long ticks = elapsedTicks[i];
            _samples[i][index] = ticks;
            _sampleSums[i] += ticks;
        }

        _sampleCount++;
        if (_sampleCount >= _windowSize)
            FlushIfAny();
    }

    public void FlushIfAny()
    {
        int count = _sampleCount;
        if (count == 0)
            return;

        _totalSamples += count;
        _windowCount++;

        var sb = new StringBuilder();
        sb.Append("[Perf] ");
        sb.Append(_label);
        sb.Append(" window=");
        sb.Append(_windowCount);
        sb.Append(" samples=");
        sb.Append(count);
        sb.Append(" total=");
        sb.Append(_totalSamples);
        sb.Append(" format=");
        sb.Append(_pixelFormat);
        sb.Append(' ');
        sb.Append(_width);
        sb.Append('x');
        sb.Append(_height);
        sb.Append(" bytes=");
        sb.Append(_dataLength);

        for (int i = 0; i < _samples.Length; i++)
        {
            sb.Append(' ');
            sb.Append(_metricNames[i]);
            sb.Append('=');
            AppendPercentiles(sb, _samples[i], count, _sampleSums[i]);
        }

        Plugin.Log!.Info(sb.ToString());

        _sampleCount = 0;
        Array.Clear(_sampleSums);
    }

    private static void AppendPercentiles(StringBuilder sb, long[] samples, int count, long sumTicks)
    {
        long[] sorted = new long[count];
        Array.Copy(samples, sorted, count);
        Array.Sort(sorted);

        sb.Append("p50/");
        AppendMilliseconds(sb, sorted[PercentileIndex(count, 0.50)]);
        sb.Append("ms,p95/");
        AppendMilliseconds(sb, sorted[PercentileIndex(count, 0.95)]);
        sb.Append("ms,p99/");
        AppendMilliseconds(sb, sorted[PercentileIndex(count, 0.99)]);
        sb.Append("ms,avg/");
        AppendMilliseconds(sb, sumTicks / (double)count);
        sb.Append("ms");
    }

    private static int PercentileIndex(int count, double percentile)
    {
        int index = (int)Math.Ceiling(count * percentile) - 1;
        return Math.Clamp(index, 0, count - 1);
    }

    private static void AppendMilliseconds(StringBuilder sb, long elapsedTicks)
        => sb.Append((elapsedTicks * 1000.0 / Stopwatch.Frequency).ToString("0.000", CultureInfo.InvariantCulture));

    private static void AppendMilliseconds(StringBuilder sb, double elapsedTicks)
        => sb.Append((elapsedTicks * 1000.0 / Stopwatch.Frequency).ToString("0.000", CultureInfo.InvariantCulture));
}
