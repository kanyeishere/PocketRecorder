using Recorder.Capture;
using Recorder.Recording;
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Threading;

namespace Recorder.Encoding;

internal sealed class NativeRecorderWriter : IOutputSink
{
    private const int MaxVideoQueueSize = 6;
    private const int MaxAudioQueueSize = 100;
    private const int NativeCodecH264 = 1;
    private const int PressureWindowMs = 1_000;

    private readonly int _videoBitrate;
    private NativeRecorderSession? _session;
    private BlockingCollection<VideoFrame>? _videoQueue;
    private BlockingCollection<AudioPacket>? _audioQueue;
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
    private int _droppedFrameCount;
    private int _audioPackets;
    private long _submitPressureUntilTicks;

    public NativeRecorderWriter(int videoBitrate)
    {
        _videoBitrate = videoBitrate;
    }

    public bool SupportsAudio => _hasAudio;
    public bool IsVideoBackedUp => _videoQueue != null && _videoQueue.Count >= MaxVideoQueueSize / 2;
    public bool IsVideoUnderPressure => IsVideoBackedUp || IsSubmitPressureActive();

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
        _droppedFrameCount = 0;
        _audioPackets = 0;
        _submitPressureUntilTicks = 0;
        _firstVideoFrameException = null;
        _firstVideoFrameSubmitted.Reset();

        _session = NativeRecorderBackend.Create(
            _outputPath,
            videoFormat,
            audioFormat,
            _videoBitrate,
            NativeCodecH264);

        _videoQueue = new BlockingCollection<VideoFrame>(MaxVideoQueueSize);
        _videoWriterThread = new Thread(VideoWriterLoop)
        {
            IsBackground = true,
            Name = "NativeRecorder-VideoWriter",
        };
        _videoWriterThread.Start();

        if (audioFormat != null)
        {
            _audioQueue = new BlockingCollection<AudioPacket>(MaxAudioQueueSize);
            _audioWriterThread = new Thread(AudioWriterLoop)
            {
                IsBackground = true,
                Name = "NativeRecorder-AudioWriter",
            };
            _audioWriterThread.Start();
        }

        Plugin.Log!.Info($"[NativeRecorder] Started native D3D11 texture writer: {videoFormat.Width}x{videoFormat.Height}@{_videoFps}fps, audio={audioFormat != null}, bitrate={_videoBitrate}");
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

        bool added = false;
        while (!added)
        {
            try
            {
                added = _videoQueue.TryAdd(frame, 0);
            }
            catch (InvalidOperationException)
            {
                frame.ReturnBuffer();
                return;
            }

            if (added)
                break;

            if (_videoQueue.TryTake(out var droppedFrame))
            {
                droppedFrame.ReturnBuffer();
                int dropped = Interlocked.Increment(ref _droppedFrameCount);
                if (dropped <= 5 || dropped % 60 == 0)
                    Plugin.Log!.Warning($"[NativeRecorder] Video queue full, dropped a captured texture frame. dropped={dropped}");
            }
            else
            {
                break;
            }
        }

        if (added)
            Interlocked.Increment(ref _inputFrameCount);
        else
            frame.ReturnBuffer();
    }

    public void WriteAudioPacket(AudioPacket packet)
    {
        if (_stopped || _audioQueue == null)
            return;

        if (!_audioQueue.TryAdd(packet, 0))
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

        foreach (var frame in _videoQueue!.GetConsumingEnumerable())
        {
            try
            {
                long submitStartTicks = Stopwatch.GetTimestamp();
                bool accepted = _session!.SubmitD3D11Texture(frame);
                if (!accepted)
                {
                    int dropped = Interlocked.Increment(ref _droppedFrameCount);
                    if (dropped <= 5 || dropped % 60 == 0)
                        Plugin.Log!.Info($"[NativeRecorder] Native texture was not ready, dropped one frame. dropped={dropped}");
                    continue;
                }

                frame.MarkD3D11TextureSubmitted();
                long submitTicks = Stopwatch.GetTimestamp() - submitStartTicks;
                MarkSubmitPressureIfSlow(submitTicks);

                int submitted = Interlocked.Increment(ref _submittedFrameCount);
                if (submitted == 1)
                    _firstVideoFrameSubmitted.Set();

                if (submitted % 300 == 0)
                    Plugin.Log!.Info($"[NativeRecorder] Submitted {submitted} texture frames (input={_inputFrameCount}, dropped={_droppedFrameCount}), audioPackets={_audioPackets}");
            }
            catch (Exception ex)
            {
                if (Volatile.Read(ref _submittedFrameCount) == 0)
                {
                    _firstVideoFrameException = ex;
                    _firstVideoFrameSubmitted.Set();
                }

                if (_stopped)
                    Plugin.Log!.Info($"[NativeRecorder] Video writer stopped while submitting: {ex.Message}");
                else
                    Plugin.Log!.Warning($"[NativeRecorder] Video submit failed: {ex.Message}");

                break;
            }
            finally
            {
                frame.ReturnBuffer();
            }
        }

        if (Volatile.Read(ref _submittedFrameCount) == 0 && _firstVideoFrameException == null)
            _firstVideoFrameSubmitted.Set();

        DrainQueuedVideoFrames();
        Plugin.Log!.Info($"[NativeRecorder] Video writer thread exiting. input={_inputFrameCount}, submitted={_submittedFrameCount}, dropped={_droppedFrameCount}");
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
                    Plugin.Log!.Warning($"[NativeRecorder] Audio submit failed: {ex.Message}");
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
        Plugin.Log!.Info($"[NativeRecorder] Stopping... input={_inputFrameCount}, submitted={_submittedFrameCount}, dropped={_droppedFrameCount}, audioPackets={_audioPackets}");

        try { _videoQueue?.CompleteAdding(); } catch { }
        try { _audioQueue?.CompleteAdding(); } catch { }

        if (_videoWriterThread != null && !_videoWriterThread.Join(5_000))
            Plugin.Log!.Warning("[NativeRecorder] Video writer did not finish in 5s.");

        if (_audioWriterThread != null && !_audioWriterThread.Join(5_000))
            Plugin.Log!.Warning("[NativeRecorder] Audio writer did not finish in 5s.");

        _session?.Stop();
        Plugin.Log!.Info("[NativeRecorder] Native writer finalized.");
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

        int drained = 0;
        while (_videoQueue.TryTake(out var pendingFrame))
        {
            pendingFrame.ReturnBuffer();
            drained++;
        }

        return drained;
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
}
