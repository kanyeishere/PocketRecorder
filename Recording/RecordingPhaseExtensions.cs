namespace Recorder.Recording;

internal static class RecordingPhaseExtensions
{
    public static string ToDisplayText(this RecordingPhase phase)
    {
        return phase switch
        {
            RecordingPhase.Idle => "待机",
            RecordingPhase.Preparing => "准备中",
            RecordingPhase.Recording => "录制中",
            RecordingPhase.Finalizing => "保存中",
            _ => phase.ToString(),
        };
    }
}
