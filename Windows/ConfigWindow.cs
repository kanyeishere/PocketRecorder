using Dalamud.Bindings.ImGui;
using Dalamud.Interface.Windowing;
using Recorder.Encoding;
using System;
using System.IO;
using System.Numerics;

namespace Recorder.Windows;

internal sealed class ConfigWindow : Window
{
    private readonly Plugin _plugin;
    private bool _isRecording;

    public ConfigWindow(Plugin plugin) : base("Recorder 录制器###RecorderConfig")
    {
        _plugin = plugin;
        Size = new Vector2(420, 520);
        SizeCondition = ImGuiCond.FirstUseEver;
        Flags = ImGuiWindowFlags.NoCollapse;

        _plugin.RecordingService.RecordingStateChanged += recording =>
        {
            _isRecording = recording;
        };
    }

    public override void Draw()
    {
        var config = _plugin.Config;

        // ── 录制状态 ──
        DrawRecordingStatus();

        ImGui.Separator();

        // ── 录制控制 ──
        DrawRecordingControls();

        ImGui.Separator();

        // ── 视频设置 ──
        DrawVideoSettings(config);

        ImGui.Separator();

        // ── 音频设置 ──
        DrawAudioSettings(config);

        ImGui.Separator();

        // ── 输出设置 ──
        DrawOutputSettings(config);

        ImGui.Separator();

        // ── FFmpeg 设置 ──
        DrawFFmpegSettings(config);

        // 保存按钮
        ImGui.Separator();
        if (ImGui.Button("保存设置"))
        {
            config.Save(Plugin.PluginInterface);
        }
    }

    private void DrawRecordingStatus()
    {
        if (_isRecording)
        {
            var elapsed = _plugin.RecordingService.Elapsed;
            int frameCount = _plugin.RecordingService.FrameCount;

            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.3f, 0.3f, 1f));
            ImGui.Text($"● 录制中  {elapsed:hh\\:mm\\:ss}");
            ImGui.PopStyleColor();
            ImGui.Text($"帧数: {frameCount}");
            if (!string.IsNullOrEmpty(_plugin.RecordingService.CurrentFilePath))
            {
                ImGui.TextWrapped($"文件: {_plugin.RecordingService.CurrentFilePath}");
            }
        }
        else
        {
            ImGui.Text("○ 待机中");
        }
    }

    private void DrawRecordingControls()
    {
        if (_isRecording)
        {
            if (ImGui.Button("■ 停止录制", new Vector2(-1, 32)))
            {
                _plugin.RecordingService.StopRecording();
            }
        }
        else
        {
            if (ImGui.Button("● 开始录制", new Vector2(-1, 32)))
            {
                _plugin.RecordingService.StartRecording();
            }
        }
        ImGui.TextDisabled("快捷命令: /record toggle");
    }

    private void DrawVideoSettings(Configuration config)
    {
        ImGui.Text("视频设置");

        int bitrate = config.VideoBitrate / 1_000_000;
        ImGui.SliderInt("码率 (Mbps)", ref bitrate, 1, 100);
        config.VideoBitrate = bitrate * 1_000_000;

        int fps = config.TargetFps;
        ImGui.SliderInt("目标帧率", ref fps, 15, 60);
        config.TargetFps = fps;

        string[] modes = { "自动", "兼容", "高级" };
        int modeIdx = config.VideoCodec == "auto"
            ? (config.UseHardwareEncoder ? 0 : 1)
            : 2;
        if (ImGui.Combo("编码模式", ref modeIdx, modes, modes.Length))
        {
            if (modeIdx == 0)
            {
                config.VideoCodec = "auto";
                config.UseHardwareEncoder = true;
                config.EncoderPreset = "auto";
            }
            else if (modeIdx == 1)
            {
                config.VideoCodec = "auto";
                config.UseHardwareEncoder = false;
                config.EncoderPreset = "auto";
            }
            else if (config.VideoCodec == "auto")
            {
                var encoder = FFmpegEncoderSelector.Select(config.GetEffectiveFFmpegPath(), config);
                config.VideoCodec = encoder.Codec;
                config.EncoderPreset = "auto";
                config.UseHardwareEncoder = true;
            }
        }

        bool lowLat = config.LowLatencyMode;
        ImGui.Checkbox("低延迟模式 (WebRTC 预留)", ref lowLat);
        config.LowLatencyMode = lowLat;
        if (lowLat)
        {
            ImGui.SameLine();
            ImGui.TextDisabled("(实验)");
        }
    }

    private void DrawAudioSettings(Configuration config)
    {
        ImGui.Text("音频设置");
        bool audio = config.CaptureAudio;
        ImGui.Checkbox("录制系统音频 (WASAPI Loopback)", ref audio);
        config.CaptureAudio = audio;
    }

    private void DrawFFmpegSettings(Configuration config)
    {
        ImGui.Text("FFmpeg 设置");

        // FFmpeg 路径
        string ffmpegPath = config.FFmpegPath;
        if (ImGui.InputText("FFmpeg 路径 (空=PATH)", ref ffmpegPath, 512))
        {
            config.FFmpegPath = ffmpegPath;
        }
        ImGui.TextDisabled($"当前: {config.GetEffectiveFFmpegPath()}");

        ImGui.TextDisabled($"编码模式: {GetEncodingModeText(config)}");

        if (config.VideoCodec != "auto")
        {
            string[] codecs = { "libx264", "libx265", "h264_nvenc", "hevc_nvenc", "av1_nvenc", "h264_qsv", "hevc_qsv", "h264_amf", "hevc_amf" };
            int codecIdx = Array.IndexOf(codecs, config.VideoCodec);
            if (codecIdx < 0) codecIdx = 0;
            if (ImGui.Combo("视频编码器", ref codecIdx, codecs, codecs.Length))
            {
                config.VideoCodec = codecs[codecIdx];
            }

            string preset = config.EncoderPreset;
            if (ImGui.InputText("预设 (auto=推荐)", ref preset, 64))
            {
                config.EncoderPreset = preset;
            }
            ImGui.TextDisabled($"实际预设: {config.ResolvePreset()}");
        }
    }

    private static string GetEncodingModeText(Configuration config)
    {
        if (config.VideoCodec != "auto")
            return $"高级 ({config.VideoCodec})";

        return config.UseHardwareEncoder
            ? "自动选择硬件编码器，失败时回退兼容模式"
            : "兼容模式";
    }

    private void DrawOutputSettings(Configuration config)
    {
        ImGui.Text("输出设置");

        string dir = config.OutputDirectory;
        if (ImGui.InputText("输出目录 (空=默认)", ref dir, 256))
        {
            config.OutputDirectory = dir;
        }

        string effectiveDir = config.GetEffectiveOutputDirectory(Plugin.PluginInterface);
        ImGui.TextDisabled($"当前: {effectiveDir}");

        if (ImGui.Button("打开输出目录"))
        {
            try
            {
                if (!Directory.Exists(effectiveDir))
                    Directory.CreateDirectory(effectiveDir);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                {
                    FileName = effectiveDir,
                    UseShellExecute = true,
                    Verb = "open"
                });
            }
            catch (Exception ex)
            {
                Plugin.Log!.Error($"Failed to open directory: {ex}");
            }
        }
    }
}
