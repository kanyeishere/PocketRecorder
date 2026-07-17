using System;
using System.Threading;

namespace Recorder.Diagnostics;

internal static class AmdRecordingDiagnosticLog
{
    private static int _enabled;

    public static bool IsAmdCodec(string? codec)
        => !string.IsNullOrWhiteSpace(codec) &&
           codec.Contains("amf", StringComparison.OrdinalIgnoreCase);

    public static bool IsAmdRelevant(string? codec, string? nativeReason, string? selectedBackendReason = null)
        => IsAmdCodec(codec) ||
           ContainsAmd(nativeReason) ||
           ContainsAmd(selectedBackendReason);

    public static void StartSession(
        int sessionId,
        int targetFps,
        int videoBitrate,
        string requestedCodec,
        string encoderPreset,
        bool useHardwareEncoder,
        AudioCaptureMode audioCaptureMode,
        bool includeOverlay,
        VideoOutputScaleMode videoOutputScaleMode,
        bool forceFfmpegRecording,
        bool preferNativeRecorder,
        string selectedBackendReason,
        string? nativeRecorderProbeReason)
    {
        Volatile.Write(
            ref _enabled,
            IsAmdRelevant(requestedCodec, nativeRecorderProbeReason, selectedBackendReason) ? 1 : 0);
        if (Volatile.Read(ref _enabled) != 0)
        {
            RecordingDiagnosticLog.WriteNativeEvent(
                "AMD.Record",
                $"diagnostics enabled, sessionId={sessionId}, fps={targetFps}, bitrate={videoBitrate}, " +
                $"codec={requestedCodec}, preset={encoderPreset}, hardware={useHardwareEncoder}, " +
                $"audio={audioCaptureMode}, overlay={includeOverlay}, scale={videoOutputScaleMode}, " +
                $"forceFFmpeg={forceFfmpegRecording}, preferNative={preferNativeRecorder}");
        }
    }

    public static void EnableImplicitSession(string reason)
    {
        Volatile.Write(ref _enabled, 1);
        RecordingDiagnosticLog.WriteNativeEvent("AMD.Record", $"implicit diagnostics enabled: {reason}");
    }

    public static void Write(string component, string message)
    {
        if (Volatile.Read(ref _enabled) != 0)
            RecordingDiagnosticLog.WriteNativeEvent($"AMD.{component}", message);
    }

    public static void WriteForAmdCodec(string? codec, string component, string message)
    {
        if (IsAmdCodec(codec))
        {
            Volatile.Write(ref _enabled, 1);
            RecordingDiagnosticLog.WriteNativeEvent($"AMD.{component}", message);
        }
    }

    public static void WriteIfEnabledOrAmdText(string component, string message)
    {
        if (Volatile.Read(ref _enabled) != 0 || ContainsAmd(message))
            RecordingDiagnosticLog.WriteNativeEvent($"AMD.{component}", message);
    }

    public static void FinishSession(string message)
    {
        if (Interlocked.Exchange(ref _enabled, 0) != 0)
            RecordingDiagnosticLog.WriteNativeEvent("AMD.Record", $"diagnostics finished: {message}");
    }

    private static bool ContainsAmd(string? value)
        => !string.IsNullOrWhiteSpace(value) &&
           (value.Contains("amd", StringComparison.OrdinalIgnoreCase) ||
            value.Contains("amf", StringComparison.OrdinalIgnoreCase) ||
            value.Contains("radeon", StringComparison.OrdinalIgnoreCase));
}
