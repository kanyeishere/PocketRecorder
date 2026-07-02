using Recorder.Capture;
using Recorder.Recording;
using System;
using System.Threading;

namespace Recorder.Encoding;

internal sealed class NativeRecorderSession : IDisposable
{
    private IntPtr _handle;
    private int _disposed;

    public NativeRecorderSession(IntPtr handle)
    {
        _handle = handle;
    }

    public bool SubmitD3D11Texture(VideoFrame frame)
    {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(NativeRecorderSession));

        if (frame.D3D11SharedHandle == IntPtr.Zero)
            throw new InvalidOperationException("NativeRecorder requires a D3D11 shared texture handle.");
        if (frame.D3D11DevicePtr == IntPtr.Zero)
            throw new InvalidOperationException("NativeRecorder requires the source D3D11 device for adapter matching.");

        return NativeRecorderBackend.SubmitD3D11SharedTexture(
            _handle,
            frame.D3D11DevicePtr,
            frame.D3D11SharedHandle,
            frame.DxgiFormat,
            frame.TimestampHns);
    }

    public void SubmitAudio(AudioPacket packet)
    {
        if (_handle == IntPtr.Zero)
            throw new ObjectDisposedException(nameof(NativeRecorderSession));

        NativeRecorderBackend.SubmitAudio(_handle, packet.Data, packet.Data.Length, packet.TimestampHns);
    }

    public void Stop()
    {
        IntPtr handle = _handle;
        if (handle == IntPtr.Zero)
            return;

        NativeRecorderBackend.Stop(handle);
    }

    public string GetLastStatus()
        => NativeRecorderBackend.GetLastStatus();

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;

        IntPtr handle = Interlocked.Exchange(ref _handle, IntPtr.Zero);
        NativeRecorderBackend.Destroy(handle);
    }
}

internal readonly record struct NativeRecorderProbeResult(bool IsAvailable, string Message)
{
    public static NativeRecorderProbeResult Available(string adapterName)
        => new(true, string.IsNullOrWhiteSpace(adapterName) ? "NVIDIA D3D11 texture recorder available." : adapterName);

    public static NativeRecorderProbeResult Unavailable(string reason)
        => new(false, reason);
}
