using System.Collections.Generic;
using System.Linq;

namespace Recorder.Recording;

internal sealed class RecordingBackendSelector
{
    private readonly NativeRecorderRecordingBackend _nativeRecorder = new();
    private readonly FFmpegRecordingBackend _ffmpeg;
    private readonly IRecorderLogger _log;

    public RecordingBackendSelector(IRecorderLogger log)
    {
        _log = log;
        _ffmpeg = new FFmpegRecordingBackend(log);
    }

    public RecordingBackendPlan SelectInitial(RecordingRequest request)
    {
        string? nativeRecorderProbeReason = null;

        foreach (IRecordingBackend backend in EnumeratePreferredBackends())
        {
            RecordingBackendProbeResult probe = backend.Probe(request);
            if (probe.IsAvailable)
            {
                string? nativeProbeReason = backend.Id == _nativeRecorder.Id
                    ? probe.Reason
                    : nativeRecorderProbeReason;
                return new RecordingBackendPlan(backend, probe, nativeProbeReason);
            }

            if (backend.Id == _nativeRecorder.Id)
            {
                nativeRecorderProbeReason = probe.DiagnosticDetails ?? probe.Reason;
                _log.Info($"[Record] NativeRecorder unavailable: {probe.Reason}");
            }
        }

        return SelectFFmpeg(request, "no preferred backend available", nativeRecorderProbeReason);
    }

    public RecordingBackendPlan SelectFFmpeg(
        RecordingRequest request,
        string reason,
        string? nativeRecorderProbeReason = null)
    {
        RecordingBackendProbeResult probe = _ffmpeg.Probe(request);
        string probeReason = string.IsNullOrWhiteSpace(reason)
            ? probe.Reason
            : $"{reason}; {probe.Reason}";
        return new RecordingBackendPlan(
            _ffmpeg,
            probe with { Reason = probeReason },
            nativeRecorderProbeReason);
    }

    private IEnumerable<IRecordingBackend> EnumeratePreferredBackends()
        => new IRecordingBackend[] { _nativeRecorder, _ffmpeg };

    public string DescribeAvailableBackends(RecordingRequest request)
    {
        return string.Join(
            ", ",
            EnumeratePreferredBackends().Select(backend =>
            {
                RecordingBackendProbeResult probe = backend.Probe(request);
                return $"{backend.DisplayName}:{(probe.IsAvailable ? "available" : probe.Reason)}";
            }));
    }
}
