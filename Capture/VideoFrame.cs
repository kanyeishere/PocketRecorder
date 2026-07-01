using System;
using System.Threading;

namespace Recorder.Capture;

/// <summary>一帧视频画面的数据。</summary>
internal sealed unsafe class VideoFrame
{
    private readonly bool _ownsBuffer;
    private readonly Action? _onConsumed;
    private int _bufferReturned;
    private bool _isNative;
    private byte* _dataPtr;

    public VideoFrame(
        byte[] data,
        int dataLength,
        int width,
        int height,
        int stride,
        long timestampHns,
        VideoPixelFormat pixelFormat,
        bool ownsBuffer = false)
    {
        Data = data;
        DataLength = dataLength;
        Width = width;
        Height = height;
        Stride = stride;
        TimestampHns = timestampHns;
        PixelFormat = pixelFormat;
        _ownsBuffer = ownsBuffer;
    }

    public VideoFrame(
        byte* dataPtr,
        int dataLength,
        int width,
        int height,
        int stride,
        long timestampHns,
        VideoPixelFormat pixelFormat,
        Action onConsumed)
    {
        _isNative = true;
        _dataPtr = dataPtr;
        Data = Array.Empty<byte>();
        DataLength = dataLength;
        Width = width;
        Height = height;
        Stride = stride;
        TimestampHns = timestampHns;
        PixelFormat = pixelFormat;
        _onConsumed = onConsumed;
    }

    public byte[] Data { get; }
    public int DataLength { get; }
    public int Width { get; }
    public int Height { get; }
    public int Stride { get; }
    public long TimestampHns { get; }
    public VideoPixelFormat PixelFormat { get; }
    public byte* DataPtr => _dataPtr;
    public bool IsNative => _isNative;

    public VideoFrame DetachToManagedCopyIfNative()
    {
        if (!_isNative)
            return this;

        byte[] buffer = RentBuffer(DataLength);
        try
        {
            new ReadOnlySpan<byte>(_dataPtr, DataLength).CopyTo(buffer.AsSpan(0, DataLength));
            var managedFrame = new VideoFrame(
                buffer,
                DataLength,
                Width,
                Height,
                Stride,
                TimestampHns,
                PixelFormat,
                ownsBuffer: true);
            ReturnBuffer();
            return managedFrame;
        }
        catch
        {
            ReturnBuffer(buffer);
            throw;
        }
    }

    public static byte[] RentBuffer(int minimumLength)
        => System.Buffers.ArrayPool<byte>.Shared.Rent(minimumLength);

    public static void ReturnBuffer(byte[] buffer)
        => System.Buffers.ArrayPool<byte>.Shared.Return(buffer);

    public void ReturnBuffer()
    {
        if (_isNative)
        {
            if (Interlocked.Exchange(ref _bufferReturned, 1) == 0)
                _onConsumed?.Invoke();
            return;
        }

        if (!_ownsBuffer)
            return;

        if (Interlocked.Exchange(ref _bufferReturned, 1) == 0)
            ReturnBuffer(Data);
    }
}
