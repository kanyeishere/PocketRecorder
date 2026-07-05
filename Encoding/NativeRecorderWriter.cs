using Recorder.Capture;
using Recorder.Diagnostics;
using Recorder.Recording;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

namespace Recorder.Encoding;

internal sealed class NativeRecorderWriter : IOutputSink
{
    private const int MaxVideoQueueSize = 18;
    private const int MaxAudioQueueSize = 100;
    private const int NativeCodecH264 = 1;
    private const int NativeCodecHevc = 2;
    private const int PressureWindowMs = 1_000;
    private const int FirstFrameRetryMs = 300;

    private readonly int _videoBitrate;
    private readonly string _videoCodec;
    private readonly int _nativeCodec;
    private readonly string _nativeCodecName;
    private NativeRecorderSession? _session;
    private BoundedMediaQueue<NativeQueuedVideoFrame>? _videoQueue;
    private BoundedMediaQueue<AudioPacket>? _audioQueue;
    private Thread? _videoWriterThread;
    private Thread? _audioWriterThread;
    private readonly ManualResetEventSlim _firstVideoFrameSubmitted = new(false);
    private Exception? _firstVideoFrameException;
    private string _outputPath = string.Empty;
    private volatile bool _stopped;
    private bool _hasAudio;
    private int _videoFps;
    private int _inputFrameCount;
    private int _submittedFrameCount;
    private int _duplicateFrameCount;
    private int _droppedFrameCount;
    private int _audioPackets;
    private long _nextSourceFrameId;
    private long _lastSubmittedSourceFrameId;
    private long _maxSubmittedSourceFrameId;
    private long _sourceFrameRepeatCount;
    private long _sourceFrameRegressionCount;
    private long _maxSourceFrameRegressionDistance;
    private long _submitPressureUntilTicks;
    private long _videoFrameDurationHns;
    private long _videoFrameDurationTicks;
    private long _firstVideoTimestampHns;
    private long _lastSubmittedVideoTimestampHns;

    public NativeRecorderWriter(int videoBitrate, string videoCodec)
    {
        _videoBitrate = videoBitrate;
        _videoCodec = videoCodec;
        _nativeCodec = ResolveNativeCodec(videoCodec);
        _nativeCodecName = _nativeCodec == NativeCodecH264 ? "H.264" : "HEVC";
    }

    public bool SupportsAudio => _hasAudio;
    public bool IsVideoBackedUp => _videoQueue != null && _videoQueue.Count >= MaxVideoQueueSize / 2;
    public bool IsVideoUnderPressure => IsVideoBackedUp || IsSubmitPressureActive();
    public event Action<IOutputSink, string>? FatalError;

    public void SetOutputPath(string path) => _outputPath = path;

    public void Start(VideoFormat videoFormat, AudioFormat? audioFormat)
    {
        if (videoFormat.PixelFormat != VideoPixelFormat.D3D11Texture)
            throw new InvalidOperationException($"NativeRecorder requires D3D11 texture frames, got {videoFormat.PixelFormat}.");

        _videoFps = Math.Max(1, videoFormat.Fps);
        _hasAudio = audioFormat != null;
        _stopped = false;
        _inputFrameCount = 0;
        _submittedFrameCount = 0;
        _duplicateFrameCount = 0;
        _droppedFrameCount = 0;
        _audioPackets = 0;
        _nextSourceFrameId = 0;
        _lastSubmittedSourceFrameId = 0;
        _maxSubmittedSourceFrameId = 0;
        _sourceFrameRepeatCount = 0;
        _sourceFrameRegressionCount = 0;
        _maxSourceFrameRegressionDistance = 0;
        _submitPressureUntilTicks = 0;
        _videoFrameDurationHns = Math.Max(1, 10_000_000L / _videoFps);
        _videoFrameDurationTicks = Math.Max(1, Stopwatch.Frequency / _videoFps);
        _firstVideoTimestampHns = -1;
        _lastSubmittedVideoTimestampHns = -1;
        _firstVideoFrameException = null;
        _firstVideoFrameSubmitted.Reset();

        string startMessage = $"starting native writer, video={videoFormat.Describe()}@{_videoFps}, codec={_nativeCodecName}, requested={_videoCodec}, audio={audioFormat != null}, bitrate={_videoBitrate}";
        RecordingDiagnosticLog.WriteNativeEvent("NativeRecorder", startMessage);
        AmdRecordingDiagnosticLog.Write("NativeRecorder", startMessage);

        _session = NativeRecorderBackend.Create(
            _outputPath,
            videoFormat,
            audioFormat,
            _videoBitrate,
            _nativeCodec);
        LogNativeStatusToDiagnostics("NativeRecorder create status");

        _videoQueue = new BoundedMediaQueue<NativeQueuedVideoFrame>(MaxVideoQueueSize);
        _videoWriterThread = new Thread(VideoWriterLoop)
        {
            IsBackground = true,
            Name = "NativeRecorder-VideoWriter",
        };
        _videoWriterThread.Start();

        if (audioFormat != null)
        {
            _audioQueue = new BoundedMediaQueue<AudioPacket>(MaxAudioQueueSize);
            _audioWriterThread = new Thread(AudioWriterLoop)
            {
                IsBackground = true,
                Name = "NativeRecorder-AudioWriter",
            };
            _audioWriterThread.Start();
        }

        Plugin.Log!.Info($"[NativeRecorder] Started native D3D11 texture writer: {videoFormat.Describe()}@{_videoFps}fps, codec={_nativeCodecName}, requested={_videoCodec}, audio={audioFormat != null}, bitrate={_videoBitrate}");
        Plugin.Log!.Info($"[NativeRecorder] Video timing: OBS-like CFR output clock with oversampled capture candidates, frameDurationHns={_videoFrameDurationHns}");
    }

    public void WriteVideoFrame(VideoFrame frame)
    {
        if (_stopped || _videoQueue == null)
        {
            frame.ReturnBuffer();
            return;
        }

        if (!frame.IsD3D11Texture)
        {
            frame.ReturnBuffer();
            Plugin.Log!.Warning($"[NativeRecorder] Dropped non-D3D11 frame: {frame.PixelFormat}.");
            return;
        }

        long sourceFrameId = Interlocked.Increment(ref _nextSourceFrameId);
        NativeQueuedVideoFrame queuedFrame = new(frame, sourceFrameId);
        if (_videoQueue.TryEnqueueDropOldest(queuedFrame, droppedFrame => droppedFrame.Frame.ReturnBuffer(), out int droppedCount))
        {
            if (droppedCount > 0)
            {
                int dropped = Interlocked.Add(ref _droppedFrameCount, droppedCount);
                if (dropped <= 5 || dropped % 60 == 0)
                    Plugin.Log!.Warning($"[NativeRecorder] Video queue full, dropped a captured texture frame. dropped={dropped}");
            }

            Interlocked.Increment(ref _inputFrameCount);
            return;
        }

        frame.ReturnBuffer();
    }

    public void WriteAudioPacket(AudioPacket packet)
    {
        if (_stopped || _audioQueue == null)
            return;

        if (!_audioQueue.TryEnqueueDropIncoming(packet))
            Plugin.Log!.Warning("[NativeRecorder] Audio queue full, dropped a packet.");
    }

    public void WaitForFirstVideoFrameSubmitted(int timeoutMs)
    {
        if (!_firstVideoFrameSubmitted.Wait(timeoutMs))
            throw new TimeoutException($"NativeRecorder did not accept the first video frame within {timeoutMs}ms.");

        if (_firstVideoFrameException != null)
            throw new InvalidOperationException("NativeRecorder failed to submit the first video frame.", _firstVideoFrameException);
    }

    private void VideoWriterLoop()
    {
        Plugin.Log!.Info("[NativeRecorder] Video writer thread started.");

        var candidates = new List<NativeQueuedVideoFrame>(MaxVideoQueueSize);
        NativeQueuedVideoFrame retainedFrame = default;
        bool hasRetainedFrame = false;
        long nextOutputFrameIndex = 0;
        long nextOutputDueTicks = 0;

        try
        {
            if (TryTakeInitialFrame(out retainedFrame))
            {
                hasRetainedFrame = true;
                bool firstSubmitted = SubmitOutputTick(
                    retainedFrame.Frame,
                    retainedFrame.SourceFrameId,
                    timestampHns: 0,
                    duplicate: false,
                    isFirstSubmittedFrame: true);

                if (firstSubmitted)
                {
                    nextOutputFrameIndex = 1;
                    nextOutputDueTicks = Stopwatch.GetTimestamp() + _videoFrameDurationTicks;

                    while (!_stopped)
                    {
                        WaitUntil(nextOutputDueTicks);
                        if (_stopped)
                            break;

                        DrainQueuedVideoFramesTo(candidates);
                        long timestampHns = nextOutputFrameIndex * _videoFrameDurationHns;
                        NativeQueuedVideoFrame selectedFrame = default;
                        bool selectedFrameOwned = false;
                        try
                        {
                            bool hasNewFrame = TrySelectNearestCandidate(
                                candidates,
                                timestampHns,
                                out selectedFrame);
                            selectedFrameOwned = hasNewFrame;

                            VideoFrame frameToSubmit = hasNewFrame ? selectedFrame.Frame : retainedFrame.Frame;
                            long sourceFrameId = hasNewFrame ? selectedFrame.SourceFrameId : retainedFrame.SourceFrameId;
                            bool accepted = SubmitOutputTick(
                                frameToSubmit,
                                sourceFrameId,
                                timestampHns,
                                duplicate: !hasNewFrame,
                                isFirstSubmittedFrame: false);

                            if (accepted && hasNewFrame)
                            {
                                retainedFrame.Frame.ReturnBuffer();
                                retainedFrame = selectedFrame;
                                hasRetainedFrame = true;
                                selectedFrameOwned = false;
                            }
                        }
                        finally
                        {
                            if (selectedFrameOwned)
                                selectedFrame.Frame.ReturnBuffer();
                        }

                        nextOutputFrameIndex++;
                        nextOutputDueTicks += _videoFrameDurationTicks;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            if (Volatile.Read(ref _submittedFrameCount) == 0)
            {
                _firstVideoFrameException = ex;
                _firstVideoFrameSubmitted.Set();
            }

            if (_stopped)
            {
                Plugin.Log!.Info($"[NativeRecorder] Video writer stopped while submitting: {ex.Message}");
            }
            else
            {
                Plugin.Log!.Warning($"[NativeRecorder] Video submit failed: {ex.Message}");
                RecordingDiagnosticLog.WriteNativeFailure(
                    "NativeRecorder",
                    $"video submit failed, exception={ex}, lastStatus={_session?.GetLastStatus()}");
                AmdRecordingDiagnosticLog.WriteIfEnabledOrAmdText(
                    "NativeRecorder",
                    $"video submit failed, exception={ex}, lastStatus={_session?.GetLastStatus()}");
                if (Volatile.Read(ref _submittedFrameCount) > 0)
                    NotifyFatalError($"NativeRecorder video submit failed: {ex.Message}");
            }
        }
        finally
        {
            ReturnCandidateFrames(candidates);
            if (hasRetainedFrame)
                retainedFrame.Frame.ReturnBuffer();
        }

        if (Volatile.Read(ref _submittedFrameCount) == 0 && _firstVideoFrameException == null)
            _firstVideoFrameSubmitted.Set();

        DrainQueuedVideoFrames();
        string sourceFrameSummary = BuildSourceFrameSummary();
        Plugin.Log!.Info($"[NativeRecorder] Video writer thread exiting. input={_inputFrameCount}, submitted={_submittedFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}, {sourceFrameSummary}");
        RecordingDiagnosticLog.WriteIfEnabled(
            "NativeRecorder",
            $"video writer exiting, input={_inputFrameCount}, submitted={_submittedFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}, {sourceFrameSummary}");
        AmdRecordingDiagnosticLog.Write(
            "NativeRecorder",
            $"video writer exiting, input={_inputFrameCount}, submitted={_submittedFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}, {sourceFrameSummary}");
    }

    private bool TryTakeInitialFrame(out NativeQueuedVideoFrame frame)
    {
        while (!_stopped && _videoQueue != null)
        {
            if (_videoQueue.TryTake(100, out frame))
            {
                Interlocked.CompareExchange(ref _firstVideoTimestampHns, frame.Frame.TimestampHns, -1);
                return true;
            }
        }

        frame = default;
        return false;
    }

    private void WaitUntil(long dueTicks)
    {
        while (!_stopped)
        {
            long remainingTicks = dueTicks - Stopwatch.GetTimestamp();
            if (remainingTicks <= 0)
                return;

            long remainingMs = remainingTicks * 1000 / Stopwatch.Frequency;
            if (remainingMs > 1)
                Thread.Sleep((int)Math.Min(remainingMs, 5));
            else
                Thread.Yield();
        }
    }

    private void DrainQueuedVideoFramesTo(List<NativeQueuedVideoFrame> candidates)
    {
        if (_videoQueue == null)
            return;

        while (_videoQueue.TryTake(out NativeQueuedVideoFrame frame))
            candidates.Add(frame);
    }

    private bool TrySelectNearestCandidate(
        List<NativeQueuedVideoFrame> candidates,
        long targetTimestampHns,
        out NativeQueuedVideoFrame selectedFrame)
    {
        selectedFrame = default;
        if (candidates.Count == 0)
            return false;

        int selectedIndex = 0;
        long selectedDistance = Math.Abs(GetRelativeCaptureTimestampHns(candidates[0].Frame) - targetTimestampHns);
        for (int i = 1; i < candidates.Count; i++)
        {
            long distance = Math.Abs(GetRelativeCaptureTimestampHns(candidates[i].Frame) - targetTimestampHns);
            if (distance < selectedDistance)
            {
                selectedDistance = distance;
                selectedIndex = i;
            }
        }

        for (int i = 0; i < selectedIndex; i++)
            candidates[i].Frame.ReturnBuffer();

        selectedFrame = candidates[selectedIndex];
        candidates.RemoveRange(0, selectedIndex + 1);
        return true;
    }

    private long GetRelativeCaptureTimestampHns(VideoFrame frame)
    {
        long first = Volatile.Read(ref _firstVideoTimestampHns);
        if (first < 0)
        {
            Interlocked.CompareExchange(ref _firstVideoTimestampHns, frame.TimestampHns, -1);
            first = Volatile.Read(ref _firstVideoTimestampHns);
        }

        return Math.Max(0, frame.TimestampHns - first);
    }

    private static void ReturnCandidateFrames(List<NativeQueuedVideoFrame> candidates)
    {
        foreach (NativeQueuedVideoFrame candidate in candidates)
            candidate.Frame.ReturnBuffer();

        candidates.Clear();
    }

    private bool SubmitOutputTick(
        VideoFrame frame,
        long sourceFrameId,
        long timestampHns,
        bool duplicate,
        bool isFirstSubmittedFrame)
    {
        long submitStartTicks = Stopwatch.GetTimestamp();
        bool accepted = SubmitD3D11TextureWithStartupRetry(frame, timestampHns, isFirstSubmittedFrame);
        if (!accepted)
        {
            int dropped = Interlocked.Increment(ref _droppedFrameCount);
            if (dropped <= 5 || dropped % 60 == 0)
            {
                string status = _session?.GetLastStatus() ?? string.Empty;
                string suffix = string.IsNullOrWhiteSpace(status) ? string.Empty : $" lastStatus={status}";
                Plugin.Log!.Info($"[NativeRecorder] Native texture was not ready, dropped one output tick. dropped={dropped}, duplicate={duplicate}.{suffix}");
            }

            if (isFirstSubmittedFrame)
            {
                string status = _session?.GetLastStatus() ?? string.Empty;
                _firstVideoFrameException = new TimeoutException(
                    string.IsNullOrWhiteSpace(status)
                        ? "NativeRecorder did not accept the startup texture."
                        : $"NativeRecorder did not accept the startup texture. {status}");
                _firstVideoFrameSubmitted.Set();
            }

            return false;
        }

        frame.MarkD3D11TextureSubmitted();
        long submitTicks = Stopwatch.GetTimestamp() - submitStartTicks;

        int submitted = Interlocked.Increment(ref _submittedFrameCount);
        if (duplicate)
            Interlocked.Increment(ref _duplicateFrameCount);
        Volatile.Write(ref _lastSubmittedVideoTimestampHns, timestampHns);
        RecordSubmittedSourceFrame(sourceFrameId, timestampHns, duplicate);

        if (submitted > 1)
            MarkSubmitPressureIfSlow(submitTicks);
        if (submitted == 1)
        {
            LogNativeStatus("Native backend status");
            LogNativeStatusToDiagnostics("First texture submit status");
            _firstVideoFrameSubmitted.Set();
        }

        if (submitted % 300 == 0)
            Plugin.Log!.Info($"[NativeRecorder] Submitted {submitted} texture frames (input={_inputFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}), audioPackets={_audioPackets}");

        return true;
    }

    private void RecordSubmittedSourceFrame(long sourceFrameId, long timestampHns, bool duplicate)
    {
        long previousSourceFrameId = _lastSubmittedSourceFrameId;
        long previousMaxSourceFrameId = _maxSubmittedSourceFrameId;

        if (previousSourceFrameId <= 0)
        {
            _lastSubmittedSourceFrameId = sourceFrameId;
            _maxSubmittedSourceFrameId = Math.Max(previousMaxSourceFrameId, sourceFrameId);
            return;
        }

        if (sourceFrameId == previousSourceFrameId)
        {
            Interlocked.Increment(ref _sourceFrameRepeatCount);
        }
        else if (sourceFrameId < previousSourceFrameId)
        {
            long regressions = Interlocked.Increment(ref _sourceFrameRegressionCount);
            long distanceFromPrevious = previousSourceFrameId - sourceFrameId;
            long distanceFromMax = Math.Max(0, previousMaxSourceFrameId - sourceFrameId);
            UpdateMaxSourceFrameRegressionDistance(Math.Max(distanceFromPrevious, distanceFromMax));
            Plugin.Log!.Warning(
                $"[NativeRecorder] Source frame id regressed. outputTimestampHns={timestampHns}, sourceFrameId={sourceFrameId}, previousSourceFrameId={previousSourceFrameId}, maxSourceFrameId={previousMaxSourceFrameId}, regressionDistance={distanceFromPrevious}, distanceFromMax={distanceFromMax}, duplicate={duplicate}, regressions={regressions}");
        }

        _lastSubmittedSourceFrameId = sourceFrameId;
        if (sourceFrameId > previousMaxSourceFrameId)
            _maxSubmittedSourceFrameId = sourceFrameId;
    }

    private void UpdateMaxSourceFrameRegressionDistance(long distance)
    {
        long current;
        do
        {
            current = Volatile.Read(ref _maxSourceFrameRegressionDistance);
            if (distance <= current)
                return;
        }
        while (Interlocked.CompareExchange(ref _maxSourceFrameRegressionDistance, distance, current) != current);
    }

    private bool SubmitD3D11TextureWithStartupRetry(VideoFrame frame, long timestampHns, bool isFirstSubmittedFrame)
    {
        if (!isFirstSubmittedFrame)
            return _session!.SubmitD3D11Texture(frame, timestampHns);

        Stopwatch retrySw = Stopwatch.StartNew();
        int attempts = 0;
        while (!_stopped)
        {
            attempts++;
            if (_session!.SubmitD3D11Texture(frame, timestampHns))
            {
                if (attempts > 1)
                    Plugin.Log!.Info($"[NativeRecorder] First texture accepted after retry. attempts={attempts}, retryMs={retrySw.ElapsedMilliseconds}");
                return true;
            }

            if (retrySw.ElapsedMilliseconds >= FirstFrameRetryMs)
            {
                string status = _session.GetLastStatus();
                string suffix = string.IsNullOrWhiteSpace(status) ? string.Empty : $" lastStatus={status}";
                Plugin.Log!.Warning($"[NativeRecorder] First texture was not ready after retry. attempts={attempts}, retryMs={retrySw.ElapsedMilliseconds}.{suffix}");
                RecordingDiagnosticLog.WriteNativeFailure(
                    "NativeRecorder",
                    $"first texture was not ready after retry, attempts={attempts}, retryMs={retrySw.ElapsedMilliseconds}, lastStatus={status}");
                return false;
            }

            Thread.Sleep(2);
        }

        return false;
    }

    private void AudioWriterLoop()
    {
        Plugin.Log!.Info("[NativeRecorder] Audio writer thread started.");

        foreach (var packet in _audioQueue!.GetConsumingEnumerable())
        {
            try
            {
                _session!.SubmitAudio(packet);
                Interlocked.Increment(ref _audioPackets);
            }
            catch (Exception ex)
            {
                if (_stopped)
                    Plugin.Log!.Info($"[NativeRecorder] Audio writer stopped while submitting: {ex.Message}");
                else
                {
                    Plugin.Log!.Warning($"[NativeRecorder] Audio submit failed: {ex.Message}");
                    RecordingDiagnosticLog.WriteNativeFailure(
                        "NativeRecorder",
                        $"audio submit failed, exception={ex}, lastStatus={_session?.GetLastStatus()}");
                    AmdRecordingDiagnosticLog.WriteIfEnabledOrAmdText(
                        "NativeRecorder",
                        $"audio submit failed, exception={ex}, lastStatus={_session?.GetLastStatus()}");
                }
                break;
            }
        }

        Plugin.Log!.Info("[NativeRecorder] Audio writer thread exiting.");
    }

    public void Stop(TimeSpan? finalVideoDuration = null)
    {
        if (_stopped)
            return;

        _stopped = true;
        Plugin.Log!.Info($"[NativeRecorder] Stopping... input={_inputFrameCount}, submitted={_submittedFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}, audioPackets={_audioPackets}");
        RecordingDiagnosticLog.WriteIfEnabled(
            "NativeRecorder",
            $"stopping, input={_inputFrameCount}, submitted={_submittedFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}, audioPackets={_audioPackets}");
        AmdRecordingDiagnosticLog.Write(
            "NativeRecorder",
            $"stopping, input={_inputFrameCount}, submitted={_submittedFrameCount}, duplicates={_duplicateFrameCount}, dropped={_droppedFrameCount}, audioPackets={_audioPackets}");

        _videoQueue?.CompleteAdding();
        _audioQueue?.CompleteAdding();

        if (_videoWriterThread != null && !_videoWriterThread.Join(5_000))
        {
            Plugin.Log!.Warning("[NativeRecorder] Video writer did not finish in 5s.");
            RecordingDiagnosticLog.WriteNativeFailure("NativeRecorder", "video writer did not finish in 5s");
            AmdRecordingDiagnosticLog.Write("NativeRecorder", "video writer did not finish in 5s");
        }

        if (_audioWriterThread != null && !_audioWriterThread.Join(5_000))
        {
            Plugin.Log!.Warning("[NativeRecorder] Audio writer did not finish in 5s.");
            RecordingDiagnosticLog.WriteNativeFailure("NativeRecorder", "audio writer did not finish in 5s");
            AmdRecordingDiagnosticLog.Write("NativeRecorder", "audio writer did not finish in 5s");
        }

        _session?.Stop();
        LogNativeStatus("Native writer finalized");
        LogNativeStatusToDiagnostics("Native writer finalized");
    }

    public void Dispose()
    {
        try { Stop(); } catch { }
        _session?.Dispose();
        _session = null;
        try { _videoQueue?.Dispose(); } catch { }
        try { _audioQueue?.Dispose(); } catch { }
        try { _firstVideoFrameSubmitted.Dispose(); } catch { }
    }

    private int DrainQueuedVideoFrames()
    {
        if (_videoQueue == null)
            return 0;

        return _videoQueue.Drain(pendingFrame => pendingFrame.Frame.ReturnBuffer());
    }

    private readonly record struct NativeQueuedVideoFrame(VideoFrame Frame, long SourceFrameId);

    private string BuildSourceFrameSummary()
    {
        return $"sourceFrameRepeats={Volatile.Read(ref _sourceFrameRepeatCount)}, " +
               $"sourceFrameRegressions={Volatile.Read(ref _sourceFrameRegressionCount)}, " +
               $"maxSourceFrameRegressionDistance={Volatile.Read(ref _maxSourceFrameRegressionDistance)}, " +
               $"lastSourceFrameId={Volatile.Read(ref _lastSubmittedSourceFrameId)}, " +
               $"maxSourceFrameId={Volatile.Read(ref _maxSubmittedSourceFrameId)}";
    }

    private bool IsSubmitPressureActive()
    {
        long untilTicks = Volatile.Read(ref _submitPressureUntilTicks);
        return untilTicks > Stopwatch.GetTimestamp();
    }

    private void MarkSubmitPressureIfSlow(long submitTicks)
    {
        int fps = Math.Max(1, _videoFps);
        long frameBudgetTicks = Math.Max(1, Stopwatch.Frequency / fps);
        if (submitTicks <= frameBudgetTicks)
            return;

        long pressureTicks = Stopwatch.GetTimestamp() +
            (Stopwatch.Frequency * PressureWindowMs / 1_000);
        Volatile.Write(ref _submitPressureUntilTicks, pressureTicks);
    }

    private void LogNativeStatus(string prefix)
    {
        string status = _session?.GetLastStatus() ?? string.Empty;
        if (string.IsNullOrWhiteSpace(status))
            Plugin.Log!.Info($"[NativeRecorder] {prefix}.");
        else
            Plugin.Log!.Info($"[NativeRecorder] {prefix}: {status}");
    }

    private void LogNativeStatusToDiagnostics(string prefix)
    {
        string status = _session?.GetLastStatus() ?? string.Empty;
        RecordingDiagnosticLog.WriteNativeEvent(
            "NativeRecorder",
            string.IsNullOrWhiteSpace(status) ? prefix : $"{prefix}: {status}");
        AmdRecordingDiagnosticLog.WriteIfEnabledOrAmdText(
            "NativeRecorder",
            string.IsNullOrWhiteSpace(status) ? prefix : $"{prefix}: {status}");
    }

    private void NotifyFatalError(string message)
    {
        try { FatalError?.Invoke(this, message); }
        catch (Exception ex)
        {
            Plugin.Log!.Warning($"[NativeRecorder] Fatal error callback failed: {ex.Message}");
        }
    }

    private static int ResolveNativeCodec(string codec)
    {
        if (string.IsNullOrWhiteSpace(codec) ||
            codec.Equals("auto", StringComparison.OrdinalIgnoreCase) ||
            codec.Equals("hevc", StringComparison.OrdinalIgnoreCase) ||
            codec.Equals("h265", StringComparison.OrdinalIgnoreCase) ||
            codec.Equals("hevc_amf", StringComparison.OrdinalIgnoreCase) ||
            codec.Equals("hevc_nvenc", StringComparison.OrdinalIgnoreCase))
        {
            return NativeCodecHevc;
        }

        if (codec.Equals("h264", StringComparison.OrdinalIgnoreCase) ||
            codec.Equals("h264_amf", StringComparison.OrdinalIgnoreCase) ||
            codec.Equals("h264_nvenc", StringComparison.OrdinalIgnoreCase))
        {
            return NativeCodecH264;
        }

        throw new InvalidOperationException($"NativeRecorder does not support codec '{codec}'.");
    }
}
