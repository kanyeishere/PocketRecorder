using Recorder.Capture;
using System;

namespace Recorder.Recording;

/// <summary>
/// 输出目标抽象。当前实现为本地 FFmpeg 文件输出。
/// 将来可扩展 WebRTC 推流（WebRtcOutputSink），编码后通过 SIPSorcery 传输 H.264 NAL。
/// </summary>
internal interface IOutputSink : IDisposable
{
    /// <summary>是否支持音频输入。</summary>
    bool SupportsAudio { get; }

    /// <summary>视频写入队列是否积压，需要捕获端暂缓读回。</summary>
    bool IsVideoBackedUp { get; }

    /// <summary>设置输出路径。</summary>
    void SetOutputPath(string path);

    /// <summary>开始写入。传入视频和音频格式信息。</summary>
    void Start(VideoFormat videoFormat, AudioFormat? audioFormat);

    /// <summary>写入一帧视频画面。</summary>
    void WriteVideoFrame(VideoFrame frame);

    /// <summary>写入一包音频数据。</summary>
    void WriteAudioPacket(AudioPacket packet);

    /// <summary>停止写入并完成输出。</summary>
    void Stop(TimeSpan? finalVideoDuration = null);
}

internal sealed record VideoFormat(int Width, int Height, int Fps, VideoPixelFormat PixelFormat);

internal sealed record AudioFormat(int SampleRate, int Channels, int BitsPerSample, bool IsFloat);
