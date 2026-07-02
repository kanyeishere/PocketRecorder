using TerraFX.Interop.DirectX;
using DXGI_FORMAT = TerraFX.Interop.DirectX.DXGI_FORMAT;

namespace Recorder.Capture;

internal static class VideoCaptureFormats
{
    public static bool IsSupportedReadbackFormat(DXGI_FORMAT format)
        => format == DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           format == DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_TYPELESS;

    public static bool IsNv12SupportedInput(DXGI_FORMAT format)
        => IsSupportedReadbackFormat(format);

    public static VideoPixelFormat GetReadbackPixelFormat(DXGI_FORMAT format)
        => IsRgbaFormat(format) ? VideoPixelFormat.Rgba : VideoPixelFormat.Bgra;

    public static DXGI_FORMAT GetNv12ShaderReadableFormat(DXGI_FORMAT format)
        => format switch
        {
            DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_UNORM_SRGB or
            DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_TYPELESS => DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM_SRGB or
            DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_TYPELESS => DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM,
            _ => format,
        };

    public static DXGI_FORMAT GetNativeSharedFormat(DXGI_FORMAT format)
        => GetNv12ShaderReadableFormat(format);

    public static uint AlignUp(uint value, uint alignment)
        => (value + alignment - 1) / alignment * alignment;

    private static bool IsRgbaFormat(DXGI_FORMAT format)
        => format == DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
