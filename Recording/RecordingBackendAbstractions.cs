using Recorder.Capture;
using System;

namespace Recorder.Recording;

internal interface IRecordingBackend
{
    string Id { get; }
    string DisplayName { get; }
    string PreparingText { get; }
    RecordingBackendCapabilities Capabilities { get; }
    bool PrefersD3D11TextureFrames { get; }

    RecordingBackendProbeResult Probe(RecordingRequest request);

    RecordingBackendStartResult Start(
        RecordingRequest request,
        VideoFrame firstFrame,
        AudioFormat? audioFormat,
        Action<IOutputSink, string> fatalErrorHandler);
}

internal sealed record RecordingBackendCapabilities(
    bool AcceptsD3D11Texture,
    bool AcceptsNv12,
    bool AcceptsBgra,
    bool SupportsAudio);

internal readonly record struct RecordingBackendProbeResult(
    bool IsAvailable,
    string Reason,
    string? DiagnosticDetails = null)
{
    public static RecordingBackendProbeResult Available(string reason, string? diagnosticDetails = null)
        => new(true, reason, diagnosticDetails);

    public static RecordingBackendProbeResult Unavailable(string reason, string? diagnosticDetails = null)
        => new(false, reason, diagnosticDetails);
}

internal sealed record RecordingBackendPlan(
    IRecordingBackend Backend,
    RecordingBackendProbeResult Probe,
    string? NativeRecorderProbeReason = null)
{
    public string PreparingText => Backend.PreparingText;
    public bool PrefersD3D11TextureFrames => Backend.PrefersD3D11TextureFrames;
    public string Reason => Probe.Reason;
}

internal sealed record RecordingBackendStartResult(
    IOutputSink Sink,
    VideoFormat VideoFormat,
    string BackendLabel,
    bool CountFirstVideoFrame);
