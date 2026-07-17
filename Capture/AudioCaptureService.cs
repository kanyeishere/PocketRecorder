using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;
using NAudio.Wasapi.CoreAudioApi.Interfaces;
using NAudio.Wave;
using Recorder;
using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace Recorder.Capture;

/// <summary>
/// 捕获系统默认音频输出设备的回放流，或只捕获当前游戏进程树的回放流。
/// NAudio 是成熟的 .NET 音频库，正确处理了 WASAPI COM 互操作的所有细节。
/// </summary>
internal sealed class AudioCaptureService : IDisposable
{
    private const long ReftimesPerSecond = 10_000_000;
    private const long ReftimesPerMillisecond = 10_000;
    private const string ProcessLoopbackDevice = "VAD\\Process_Loopback";

    private readonly Action<AudioPacket> _onAudio;
    private readonly AudioCaptureMode _mode;
    private readonly int _targetProcessId;
    private Thread? _captureThread;
    private CancellationTokenSource? _cts;
    private WasapiLoopbackCapture? _capture;
    private AudioClient? _processAudioClient;
    private bool _running;
    private bool _isFloat;

    public int SampleRate { get; private set; }
    public int Channels { get; private set; }
    public int BitsPerSample { get; private set; }

    public string? LastError { get; private set; }
    public bool Initialized { get; private set; }

    // 累计已采集的样本数，用于计算时间戳
    private long _totalSamples;
    private long _diagnosticPackets;
    private long _diagnosticSampleValues;
    private long _diagnosticSilentPackets;
    private long _diagnosticClippedValues;
    private long _diagnosticInvalidValues;
    private long _wasapiSilentBuffers;
    private long _wasapiDiscontinuities;
    private long _wasapiTimestampErrors;
    private double _diagnosticSumSquares;
    private double _diagnosticPeak;
    private int _diagnosticsLogged;
    private string _diagnosticSummary = "not-finalized";

    public string DiagnosticSummary => _diagnosticSummary;

    public AudioCaptureService(AudioCaptureMode mode, int targetProcessId, Action<AudioPacket> onAudio)
    {
        _mode = mode;
        _targetProcessId = targetProcessId;
        _onAudio = onAudio;
    }

    public void Start()
    {
        ResetDiagnostics();
        _cts = new CancellationTokenSource();
        _running = true;
        _captureThread = new Thread(() => CaptureLoop(_cts.Token))
        {
            IsBackground = true,
            Name = "Recorder-AudioCapture",
        };
        _captureThread.Start();
    }

    public void Stop()
    {
        _running = false;
        try { _cts?.Cancel(); } catch { }

        try { _capture?.StopRecording(); } catch { }

        _captureThread?.Join(3000);
        LogDiagnosticsOnce();
        _cts?.Dispose();
        _cts = null;
        _captureThread = null;
    }

    private void CaptureLoop(CancellationToken ct)
    {
        Plugin.Log!.Info($"[Audio] Starting capture mode={_mode}, targetProcessId={_targetProcessId}...");

        try
        {
            if (_mode == AudioCaptureMode.Game)
            {
                CaptureProcessLoopback(ct);
            }
            else
            {
                CaptureSystemLoopback(ct);
            }
        }
        catch (Exception ex)
        {
            LastError = ex.Message;
            Plugin.Log!.Error($"[Audio] NAudio initialization failed: {ex}");
        }
        finally
        {
            try { _capture?.StopRecording(); } catch { }

            if (_capture != null)
            {
                _capture.DataAvailable -= OnDataAvailable;
                _capture.RecordingStopped -= OnRecordingStopped;
                _capture.Dispose();
                _capture = null;
            }

            try { _processAudioClient?.Dispose(); } catch { }
            _processAudioClient = null;

            Plugin.Log!.Info("[Audio] Capture thread exiting.");
        }
    }

    private void CaptureSystemLoopback(CancellationToken ct)
    {
        // NAudio 内部处理 COM 初始化，不需要手动 CoInitializeEx
        _capture = new WasapiLoopbackCapture();

        var wf = _capture.WaveFormat;
        SampleRate = wf.SampleRate;
        Channels = wf.Channels;
        BitsPerSample = wf.BitsPerSample;
        bool isFloat = wf.AsStandardWaveFormat().Encoding == WaveFormatEncoding.IeeeFloat;
        _isFloat = isFloat;

        Plugin.Log!.Info($"[Audio] System WaveFormat: {SampleRate}Hz, {Channels}ch, {BitsPerSample}bit, encoding={wf.Encoding}, float={isFloat}");
        Plugin.Log.Info($"[Audio] BlockAlign={wf.BlockAlign}, AvgBytesPerSec={wf.AverageBytesPerSecond}");

        Initialized = true;
        _totalSamples = 0;

        _capture.DataAvailable += OnDataAvailable;
        _capture.RecordingStopped += OnRecordingStopped;

        Plugin.Log.Info("[Audio] Starting system loopback recording...");
        _capture.StartRecording();
        Plugin.Log.Info("[Audio] System loopback recording started successfully.");

        while (_running && !ct.IsCancellationRequested)
        {
            Thread.Sleep(100);
        }
    }

    private void CaptureProcessLoopback(CancellationToken ct)
    {
        if (_targetProcessId <= 0)
            throw new InvalidOperationException("无法确定游戏进程 ID。");

        _processAudioClient = ActivateProcessLoopbackAsync(_targetProcessId)
            .GetAwaiter()
            .GetResult();

        WaveFormat wf = WaveFormat.CreateIeeeFloatWaveFormat(48_000, 2);
        SampleRate = wf.SampleRate;
        Channels = wf.Channels;
        BitsPerSample = wf.BitsPerSample;
        _isFloat = true;

        _processAudioClient.Initialize(
            AudioClientShareMode.Shared,
            AudioClientStreamFlags.Loopback | AudioClientStreamFlags.AutoConvertPcm | AudioClientStreamFlags.SrcDefaultQuality,
            ReftimesPerMillisecond * 100,
            0,
            wf,
            Guid.Empty);

        int bufferFrameCount = SampleRate / 10;
        int bytesPerFrame = wf.BlockAlign;
        byte[] recordBuffer = new byte[Math.Max(bytesPerFrame, bufferFrameCount * bytesPerFrame)];

        Plugin.Log!.Info($"[Audio] Game process loopback: pid={_targetProcessId}, {SampleRate}Hz, {Channels}ch, {BitsPerSample}bit, bufferFrames={bufferFrameCount}");

        Initialized = true;
        _totalSamples = 0;

        var capture = _processAudioClient.AudioCaptureClient;
        _processAudioClient.Start();
        Plugin.Log.Info("[Audio] Game process loopback recording started successfully.");

        while (_running && !ct.IsCancellationRequested)
        {
            Thread.Sleep(20);
            ReadProcessLoopbackPackets(capture, recordBuffer, bytesPerFrame);
        }
    }

    private void ReadProcessLoopbackPackets(AudioCaptureClient capture, byte[] recordBuffer, int bytesPerFrame)
    {
        int packetSize = capture.GetNextPacketSize();
        int recordBufferOffset = 0;

        while (packetSize != 0)
        {
            IntPtr buffer = capture.GetBuffer(out int framesAvailable, out AudioClientBufferFlags flags);
            int bytesAvailable = framesAvailable * bytesPerFrame;

            if ((flags & (AudioClientBufferFlags)0x1) != 0)
                _wasapiDiscontinuities++;
            if ((flags & (AudioClientBufferFlags)0x2) != 0)
                _wasapiSilentBuffers++;
            if ((flags & (AudioClientBufferFlags)0x4) != 0)
                _wasapiTimestampErrors++;

            if (recordBuffer.Length - recordBufferOffset < bytesAvailable && recordBufferOffset > 0)
            {
                EmitAudio(recordBuffer, recordBufferOffset);
                recordBufferOffset = 0;
            }

            if (recordBuffer.Length < bytesAvailable)
                Array.Resize(ref recordBuffer, bytesAvailable);

            if ((flags & AudioClientBufferFlags.Silent) != AudioClientBufferFlags.Silent)
                Marshal.Copy(buffer, recordBuffer, recordBufferOffset, bytesAvailable);
            else
                Array.Clear(recordBuffer, recordBufferOffset, bytesAvailable);

            recordBufferOffset += bytesAvailable;
            capture.ReleaseBuffer(framesAvailable);
            packetSize = capture.GetNextPacketSize();
        }

        if (recordBufferOffset > 0)
            EmitAudio(recordBuffer, recordBufferOffset);
    }

    private void OnDataAvailable(object? sender, WaveInEventArgs e)
    {
        if (!_running || e.BytesRecorded == 0) return;

        int blockAlign = Channels * BitsPerSample / 8;
        if (blockAlign <= 0) return;

        EmitAudio(e.Buffer, e.BytesRecorded);
    }

    private void EmitAudio(byte[] sourceBuffer, int bytesRecorded)
    {
        long captureTicks = Stopwatch.GetTimestamp();
        // 计算时间戳（基于已采集样本数）
        int blockAlign = Channels * BitsPerSample / 8;
        if (blockAlign <= 0) return;

        byte[] buffer = new byte[bytesRecorded];
        Array.Copy(sourceBuffer, buffer, bytesRecorded);

        AnalyzeAudio(sourceBuffer, bytesRecorded);

        long samplesInPacket = bytesRecorded / blockAlign;
        long timestampHns = _totalSamples * 10_000_000L / SampleRate;
        _totalSamples += samplesInPacket;

        _onAudio(new AudioPacket(
            buffer,
            SampleRate,
            Channels,
            BitsPerSample,
            timestampHns,
            captureTicks));
    }

    private void ResetDiagnostics()
    {
        _diagnosticPackets = 0;
        _diagnosticSampleValues = 0;
        _diagnosticSilentPackets = 0;
        _diagnosticClippedValues = 0;
        _diagnosticInvalidValues = 0;
        _wasapiSilentBuffers = 0;
        _wasapiDiscontinuities = 0;
        _wasapiTimestampErrors = 0;
        _diagnosticSumSquares = 0;
        _diagnosticPeak = 0;
        _diagnosticsLogged = 0;
        _diagnosticSummary = "not-finalized";
    }

    private void AnalyzeAudio(byte[] buffer, int byteCount)
    {
        if (byteCount <= 0)
            return;

        double packetPeak = 0;
        double packetSumSquares = 0;
        long packetValues = 0;
        long packetClipped = 0;
        long packetInvalid = 0;
        ReadOnlySpan<byte> bytes = buffer.AsSpan(0, Math.Min(byteCount, buffer.Length));

        if (_isFloat && BitsPerSample == 32)
        {
            foreach (float sample in MemoryMarshal.Cast<byte, float>(bytes))
            {
                bool valid = float.IsFinite(sample);
                double value = valid ? Math.Abs((double)sample) : 1.0;
                if (!valid)
                    packetInvalid++;
                packetPeak = Math.Max(packetPeak, value);
                packetSumSquares += value * value;
                packetValues++;
                if (value >= 0.999)
                    packetClipped++;
            }
        }
        else if (BitsPerSample == 16)
        {
            foreach (short sample in MemoryMarshal.Cast<byte, short>(bytes))
            {
                double value = Math.Abs(sample / 32768.0);
                packetPeak = Math.Max(packetPeak, value);
                packetSumSquares += value * value;
                packetValues++;
                if (value >= 0.999)
                    packetClipped++;
            }
        }
        else if (BitsPerSample == 32)
        {
            foreach (int sample in MemoryMarshal.Cast<byte, int>(bytes))
            {
                double value = Math.Abs(sample / 2147483648.0);
                packetPeak = Math.Max(packetPeak, value);
                packetSumSquares += value * value;
                packetValues++;
                if (value >= 0.999)
                    packetClipped++;
            }
        }
        else if (BitsPerSample == 24)
        {
            for (int offset = 0; offset + 2 < bytes.Length; offset += 3)
            {
                int sample = bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16);
                if ((sample & 0x800000) != 0)
                    sample |= unchecked((int)0xFF000000);
                double value = Math.Abs(sample / 8388608.0);
                packetPeak = Math.Max(packetPeak, value);
                packetSumSquares += value * value;
                packetValues++;
                if (value >= 0.999)
                    packetClipped++;
            }
        }

        _diagnosticPackets++;
        _diagnosticSampleValues += packetValues;
        _diagnosticSumSquares += packetSumSquares;
        _diagnosticPeak = Math.Max(_diagnosticPeak, packetPeak);
        _diagnosticClippedValues += packetClipped;
        _diagnosticInvalidValues += packetInvalid;
        if (packetPeak < 0.0001)
            _diagnosticSilentPackets++;
    }

    private void LogDiagnosticsOnce()
    {
        if (Interlocked.Exchange(ref _diagnosticsLogged, 1) != 0)
            return;

        double rms = _diagnosticSampleValues > 0
            ? Math.Sqrt(_diagnosticSumSquares / _diagnosticSampleValues)
            : 0;
        string health = _diagnosticPackets == 0
            ? "no-audio-packets"
            : _diagnosticInvalidValues > 0
                ? "invalid-samples"
            : _diagnosticSilentPackets == _diagnosticPackets
                ? "all-silent"
                : _wasapiDiscontinuities > 0 || _wasapiTimestampErrors > 0
                    ? "discontinuity"
                    : "ok";
        string summary =
            $"signalHealth={health}, packets={_diagnosticPackets}, sampleValues={_diagnosticSampleValues}, " +
            $"rms={rms:0.000000}, peak={_diagnosticPeak:0.000000}, silentPackets={_diagnosticSilentPackets}, " +
            $"clippedValues={_diagnosticClippedValues}, invalidValues={_diagnosticInvalidValues}, " +
            $"wasapiSilentBuffers={_wasapiSilentBuffers}, " +
            $"wasapiDiscontinuities={_wasapiDiscontinuities}, wasapiTimestampErrors={_wasapiTimestampErrors}";
        _diagnosticSummary = summary;
        Plugin.Log!.Info($"[Audio] Signal diagnostics: {summary}");
        Diagnostics.RecordingDiagnosticLog.WriteIfEnabled("Audio", $"signal diagnostics: {summary}");
    }

    private void OnRecordingStopped(object? sender, StoppedEventArgs e)
    {
        if (e.Exception != null)
        {
            Plugin.Log!.Warning($"[Audio] Recording stopped with exception: {e.Exception}");
        }
        else
        {
            Plugin.Log!.Info("[Audio] Recording stopped normally.");
        }
    }

    public void Dispose()
    {
        Stop();
    }

    private static async Task<AudioClient> ActivateProcessLoopbackAsync(int targetProcessId)
    {
        var completionHandler = new ActivateAudioInterfaceCompletionHandler();
        IntPtr audioClientActivationParamsPtr = IntPtr.Zero;
        IntPtr propVariantPtr = IntPtr.Zero;

        try
        {
            BuildProcessLoopbackActivationParams(
                targetProcessId,
                out audioClientActivationParamsPtr,
                out propVariantPtr);

            ActivateAudioInterfaceAsync(
                ProcessLoopbackDevice,
                typeof(IAudioClient).GUID,
                propVariantPtr,
                completionHandler,
                out _);

            object activated = await completionHandler;
            return new AudioClient((IAudioClient)activated);
        }
        finally
        {
            if (propVariantPtr != IntPtr.Zero)
                Marshal.FreeHGlobal(propVariantPtr);
            if (audioClientActivationParamsPtr != IntPtr.Zero)
                Marshal.FreeHGlobal(audioClientActivationParamsPtr);
        }
    }

    private static void BuildProcessLoopbackActivationParams(
        int targetProcessId,
        out IntPtr audioClientActivationParamsPtr,
        out IntPtr propVariantPtr)
    {
        var activationParams = new AudioClientActivationParamsNative
        {
            ActivationType = AudioClientActivationTypeNative.ProcessLoopback,
            ProcessLoopbackParams = new AudioClientProcessLoopbackParamsNative
            {
                TargetProcessId = targetProcessId,
                ProcessLoopbackMode = ProcessLoopbackModeNative.IncludeTargetProcessTree,
            },
        };

        audioClientActivationParamsPtr = Marshal.AllocHGlobal(Marshal.SizeOf<AudioClientActivationParamsNative>());
        Marshal.StructureToPtr(activationParams, audioClientActivationParamsPtr, false);

        var propVariant = new PropVariantBlob
        {
            VariantType = 65, // VT_BLOB
            Blob = new Blob
            {
                ByteCount = Marshal.SizeOf<AudioClientActivationParamsNative>(),
                Data = audioClientActivationParamsPtr,
            },
        };

        propVariantPtr = Marshal.AllocHGlobal(Marshal.SizeOf<PropVariantBlob>());
        Marshal.StructureToPtr(propVariant, propVariantPtr, false);
    }

    [DllImport("Mmdevapi.dll", ExactSpelling = true, PreserveSig = false)]
    private static extern void ActivateAudioInterfaceAsync(
        [MarshalAs(UnmanagedType.LPWStr)] string deviceInterfacePath,
        [MarshalAs(UnmanagedType.LPStruct)] Guid riid,
        IntPtr activationParams,
        IActivateAudioInterfaceCompletionHandler completionHandler,
        out IActivateAudioInterfaceAsyncOperation activationOperation);

    private sealed class ActivateAudioInterfaceCompletionHandler :
        IActivateAudioInterfaceCompletionHandler,
        IAgileObject
    {
        private readonly TaskCompletionSource<object> _completion = new();

        public void ActivateCompleted(IActivateAudioInterfaceAsyncOperation activateOperation)
        {
            activateOperation.GetActivateResult(out int hr, out object activatedInterface);
            if (hr != 0)
            {
                _completion.TrySetException(Marshal.GetExceptionForHR(hr) ?? new COMException("ActivateAudioInterfaceAsync failed.", hr));
                return;
            }

            _completion.TrySetResult(activatedInterface);
        }

        public TaskAwaiter<object> GetAwaiter() => _completion.Task.GetAwaiter();
    }

    [ComImport]
    [Guid("94ea2b94-e9cc-49e0-c0ff-ee64ca8f5b90")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IAgileObject
    {
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AudioClientActivationParamsNative
    {
        public AudioClientActivationTypeNative ActivationType;
        public AudioClientProcessLoopbackParamsNative ProcessLoopbackParams;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AudioClientProcessLoopbackParamsNative
    {
        public int TargetProcessId;
        public ProcessLoopbackModeNative ProcessLoopbackMode;
    }

    private enum AudioClientActivationTypeNative
    {
        Default = 0,
        ProcessLoopback = 1,
    }

    private enum ProcessLoopbackModeNative
    {
        IncludeTargetProcessTree = 0,
        ExcludeTargetProcessTree = 1,
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PropVariantBlob
    {
        public ushort VariantType;
        public ushort Reserved1;
        public ushort Reserved2;
        public ushort Reserved3;
        public Blob Blob;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Blob
    {
        public int ByteCount;
        public IntPtr Data;
    }
}

/// <summary>一包音频数据。</summary>
internal sealed record AudioPacket(
    byte[] Data,
    int SampleRate,
    int Channels,
    int BitsPerSample,
    long TimestampHns,
    long CaptureTicks);
