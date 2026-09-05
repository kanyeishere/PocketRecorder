using Recorder.Capture;

namespace Recorder.Recording;

internal sealed record RecordingRequest(
    int SessionId,
    string OutputPath,
    string FFmpegPath,
    string PluginConfigDirectory,
    int VideoBitrate,
    int TargetFps,
    AudioCaptureMode AudioCaptureMode,
    bool CaptureMicrophone,
    string VideoCodec,
    string EncoderPreset,
    bool UseHardwareEncoder,
    bool IncludeOverlay,
    VideoOutputScaleMode VideoOutputScaleMode,
    bool ForceFFmpegRecording,
    GameGraphicsDeviceProbeResult GameGraphicsDevice)
{
    public bool HasAudio => AudioCaptureMode != AudioCaptureMode.Off || CaptureMicrophone;

    public VideoFormat ToVideoFormat(VideoFrame frame)
    {
        VideoOutputDimensions output = GetOutputDimensions(frame.Width, frame.Height);
        return new VideoFormat(
            frame.Width,
            frame.Height,
            output.Width,
            output.Height,
            TargetFps,
            frame.PixelFormat);
    }

    public VideoOutputDimensions GetOutputDimensions(int sourceWidth, int sourceHeight)
        => VideoOutputScale.Resolve(sourceWidth, sourceHeight, VideoOutputScaleMode);
}
