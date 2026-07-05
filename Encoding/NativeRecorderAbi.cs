using System;
using System.Runtime.InteropServices;

namespace Recorder.Encoding;

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrGetAbiVersion();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrProbe(ref NativeProbeInfo info);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrGetDiagnosticsReport(byte* buffer, int bufferSize);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrCreate(ref NativeVideoConfig video, ref NativeAudioConfig audio, out IntPtr recorder);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrSubmitD3D11SharedTexture(IntPtr recorder, IntPtr d3d11Device, IntPtr sharedHandle, int dxgiFormat, long timestampHns);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrSubmitAudio(IntPtr recorder, byte* data, int byteCount, long timestampHns);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrStop(IntPtr recorder);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate void PrDestroy(IntPtr recorder);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal unsafe delegate int PrGetLastError(byte* buffer, int bufferSize);

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal unsafe struct NativeProbeInfo
{
    public int IsSupportedAdapter;
    public int SupportsD3D11TextureInput;
    public fixed byte AdapterName[128];
    public fixed byte Message[256];
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct NativeVideoConfig
{
    public int Width;
    public int Height;
    public int Fps;
    public int BitrateBps;
    public int Codec;
    public int PixelFormat;
    public int OutputWidth;
    public int OutputHeight;
    public IntPtr OutputPath;
}

[StructLayout(LayoutKind.Sequential, Pack = 8)]
internal struct NativeAudioConfig
{
    public int Enabled;
    public int SampleRate;
    public int Channels;
    public int BitsPerSample;
    public int IsFloat;
}
