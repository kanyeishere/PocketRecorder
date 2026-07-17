using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;

namespace Recorder.Encoding;

internal sealed class NativeRecorderTimingDiagnostics
{
    private readonly RollingSamples _captureDeltaHns = new();
    private readonly RollingSamples _captureJitterHns = new();
    private readonly RollingSamples _sampleAgeTicks = new();
    private readonly RollingSamples _submitAttemptTicks = new();
    private readonly RollingSamples _acceptedSubmitTicks = new();
    private readonly RollingSamples _rejectedSubmitTicks = new();

    private long _expectedFrameHns;
    private long _frameBudgetTicks;
    private long _lastCaptureTimestampHns;
    private long _earlyFrames;
    private long _lateFrames;
    private long _longGapFrames;
    private long _nonMonotonicCaptureFrames;
    private long _sampleAgeOverBudgetFrames;
    private long _submitOverBudgetFrames;
    private long _maxConsecutiveLongGaps;
    private long _currentConsecutiveLongGaps;

    public void Reset(int fps)
    {
        int safeFps = Math.Max(1, fps);
        _expectedFrameHns = Math.Max(1, 10_000_000L / safeFps);
        _frameBudgetTicks = Math.Max(1, Stopwatch.Frequency / safeFps);
        _lastCaptureTimestampHns = -1;
        _earlyFrames = 0;
        _lateFrames = 0;
        _longGapFrames = 0;
        _nonMonotonicCaptureFrames = 0;
        _sampleAgeOverBudgetFrames = 0;
        _submitOverBudgetFrames = 0;
        _maxConsecutiveLongGaps = 0;
        _currentConsecutiveLongGaps = 0;
        _captureDeltaHns.Clear();
        _captureJitterHns.Clear();
        _sampleAgeTicks.Clear();
        _submitAttemptTicks.Clear();
        _acceptedSubmitTicks.Clear();
        _rejectedSubmitTicks.Clear();
    }

    public void RecordSubmitAttempt(long submitTicks, bool accepted)
    {
        long safeSubmitTicks = Math.Max(0, submitTicks);
        _submitAttemptTicks.Add(safeSubmitTicks);
        if (accepted)
            _acceptedSubmitTicks.Add(safeSubmitTicks);
        else
            _rejectedSubmitTicks.Add(safeSubmitTicks);

        if (safeSubmitTicks > _frameBudgetTicks)
            _submitOverBudgetFrames++;
    }

    public void RecordSubmittedFrame(long captureTimestampHns, long sampleAgeTicks)
    {
        _sampleAgeTicks.Add(Math.Max(0, sampleAgeTicks));

        if (sampleAgeTicks > _frameBudgetTicks)
            _sampleAgeOverBudgetFrames++;

        if (_lastCaptureTimestampHns >= 0)
        {
            long deltaHns = captureTimestampHns - _lastCaptureTimestampHns;
            if (deltaHns <= 0)
            {
                _nonMonotonicCaptureFrames++;
            }
            else
            {
                _captureDeltaHns.Add(deltaHns);
                _captureJitterHns.Add(Math.Abs(deltaHns - _expectedFrameHns));

                if (deltaHns < _expectedFrameHns * 3 / 4)
                    _earlyFrames++;
                if (deltaHns > _expectedFrameHns * 5 / 4)
                    _lateFrames++;

                if (deltaHns > _expectedFrameHns * 2)
                {
                    _longGapFrames++;
                    _currentConsecutiveLongGaps++;
                    _maxConsecutiveLongGaps = Math.Max(_maxConsecutiveLongGaps, _currentConsecutiveLongGaps);
                }
                else
                {
                    _currentConsecutiveLongGaps = 0;
                }
            }
        }

        _lastCaptureTimestampHns = captureTimestampHns;
    }

    public string BuildSummary()
    {
        return "outputTiming=obsStyleSharedTextureCfr" +
               ", targetFrameMs=" + FormatMs(_expectedFrameHns / 10_000.0) +
               ", captureDeltaMs=" + FormatHnsSummary(_captureDeltaHns) +
               ", captureJitterMs=" + FormatHnsSummary(_captureJitterHns) +
               ", earlyFrames=" + _earlyFrames.ToString(CultureInfo.InvariantCulture) +
               ", lateFrames=" + _lateFrames.ToString(CultureInfo.InvariantCulture) +
               ", longGapFrames=" + _longGapFrames.ToString(CultureInfo.InvariantCulture) +
               ", maxConsecutiveLongGaps=" + _maxConsecutiveLongGaps.ToString(CultureInfo.InvariantCulture) +
               ", nonMonotonicCaptureFrames=" + _nonMonotonicCaptureFrames.ToString(CultureInfo.InvariantCulture) +
               ", sampleAgeMs=" + FormatTicksSummary(_sampleAgeTicks) +
               ", sampleAgeOverBudgetFrames=" + _sampleAgeOverBudgetFrames.ToString(CultureInfo.InvariantCulture) +
               ", submitMs=" + FormatTicksSummary(_submitAttemptTicks) +
               ", acceptedSubmitMs=" + FormatTicksSummary(_acceptedSubmitTicks) +
               ", rejectedSubmitMs=" + FormatTicksSummary(_rejectedSubmitTicks) +
               ", submitOverBudgetFrames=" + _submitOverBudgetFrames.ToString(CultureInfo.InvariantCulture) +
               ", thresholds=early<75%,late>125%,longGap>200%";
    }

    private static string FormatHnsSummary(RollingSamples values)
    {
        long[] snapshot = values.Snapshot();
        if (snapshot.Length == 0)
            return "count=0";

        return FormatSummary(snapshot, values.TotalCount, value => value / 10_000.0);
    }

    private static string FormatTicksSummary(RollingSamples values)
    {
        long[] snapshot = values.Snapshot();
        if (snapshot.Length == 0)
            return "count=0";

        return FormatSummary(snapshot, values.TotalCount, value => value * 1_000.0 / Stopwatch.Frequency);
    }

    private static string FormatSummary(long[] sorted, long totalCount, Func<long, double> toMilliseconds)
    {
        Array.Sort(sorted);

        double p50 = toMilliseconds(Percentile(sorted, 0.50));
        double p95 = toMilliseconds(Percentile(sorted, 0.95));
        double p99 = toMilliseconds(Percentile(sorted, 0.99));
        double max = toMilliseconds(sorted[^1]);
        double avg = 0;
        for (int i = 0; i < sorted.Length; i++)
            avg += toMilliseconds(sorted[i]);
        avg /= sorted.Length;

        return "count=" + totalCount.ToString(CultureInfo.InvariantCulture) +
               ",sampled=" + sorted.Length.ToString(CultureInfo.InvariantCulture) +
               ",avg=" + FormatMs(avg) +
               ",p50=" + FormatMs(p50) +
               ",p95=" + FormatMs(p95) +
               ",p99=" + FormatMs(p99) +
               ",max=" + FormatMs(max);
    }

    private static long Percentile(long[] sorted, double percentile)
    {
        if (sorted.Length == 0)
            return 0;

        int index = (int)Math.Ceiling(sorted.Length * percentile) - 1;
        index = Math.Clamp(index, 0, sorted.Length - 1);
        return sorted[index];
    }

    private static string FormatMs(double value)
        => value.ToString("0.###", CultureInfo.InvariantCulture);

    private sealed class RollingSamples
    {
        private const int Capacity = 65_536;
        private readonly List<long> _values = new(Capacity);
        private int _nextIndex;

        public long TotalCount { get; private set; }

        public void Add(long value)
        {
            TotalCount++;
            if (_values.Count < Capacity)
            {
                _values.Add(value);
                return;
            }

            _values[_nextIndex] = value;
            _nextIndex = (_nextIndex + 1) % Capacity;
        }

        public void Clear()
        {
            _values.Clear();
            _nextIndex = 0;
            TotalCount = 0;
        }

        public long[] Snapshot()
            => _values.ToArray();
    }
}
