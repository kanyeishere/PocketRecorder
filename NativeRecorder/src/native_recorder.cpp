#define PR_NATIVE_RECORDER_BUILD
#include "pocket_recorder_native.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kNvidiaVendorId = 0x10DE;
constexpr DWORD kInvalidStream = 0xFFFFFFFFu;
constexpr UINT64 kGameWriteKey = 0;
constexpr UINT64 kEncoderReadKey = 1;
constexpr DWORD kSharedTextureAcquireTimeoutMs = 5;

std::mutex g_error_mutex;
std::string g_last_error;
std::mutex g_mf_mutex;
int g_mf_ref_count = 0;

void set_last_error(const std::string& message)
{
    std::lock_guard lock(g_error_mutex);
    g_last_error = message;
}

void set_last_error(const char* message)
{
    set_last_error(std::string(message != nullptr ? message : ""));
}

std::string get_last_error_copy()
{
    std::lock_guard lock(g_error_mutex);
    return g_last_error;
}

void copy_text(char* destination, size_t capacity, const char* source)
{
    if (destination == nullptr || capacity == 0)
        return;

    const char* text = source != nullptr ? source : "";
    size_t length = std::min(capacity - 1, std::strlen(text));
    std::memcpy(destination, text, length);
    destination[length] = '\0';
}

std::string wide_to_utf8(const wchar_t* text)
{
    if (text == nullptr || *text == L'\0')
        return {};

    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};

    std::string result(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::string hresult_to_string(HRESULT hr)
{
    char* message = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);

    std::string result = "HRESULT 0x" + std::to_string(static_cast<uint32_t>(hr));
    if (length > 0 && message != nullptr)
    {
        result.assign(message, length);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == '.'))
            result.pop_back();
        result += " (0x";
        char hex[16]{};
        sprintf_s(hex, "%08X", static_cast<uint32_t>(hr));
        result += hex;
        result += ")";
    }

    if (message != nullptr)
        LocalFree(message);

    return result;
}

const char* dxgi_format_name(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "DXGI_FORMAT_B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return "DXGI_FORMAT_B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return "DXGI_FORMAT_R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_NV12:
        return "DXGI_FORMAT_NV12";
    default:
        return "DXGI_FORMAT_UNKNOWN";
    }
}

std::string dxgi_format_to_string(DXGI_FORMAT format)
{
    return std::string(dxgi_format_name(format)) + "(" + std::to_string(static_cast<int>(format)) + ")";
}

std::string hex_uint32(uint32_t value)
{
    char buffer[32]{};
    sprintf_s(buffer, "0x%X", value);
    return buffer;
}

std::string texture_desc_to_string(const D3D11_TEXTURE2D_DESC& desc)
{
    return std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
        ", format=" + dxgi_format_to_string(desc.Format) +
        ", usage=" + std::to_string(static_cast<uint32_t>(desc.Usage)) +
        ", bind=" + hex_uint32(desc.BindFlags) +
        ", cpu=" + hex_uint32(desc.CPUAccessFlags) +
        ", misc=" + hex_uint32(desc.MiscFlags) +
        ", sample=" + std::to_string(desc.SampleDesc.Count) + "/" + std::to_string(desc.SampleDesc.Quality);
}

HRESULT fail_step(const char* operation, HRESULT hr, const std::string& details = {})
{
    std::string message = "NativeRecorder ";
    message += operation != nullptr ? operation : "operation";
    message += " failed: ";
    message += hresult_to_string(hr);
    if (!details.empty())
    {
        message += "; ";
        message += details;
    }
    set_last_error(message);
    return hr;
}

int32_t fail_hr(const char* operation, HRESULT hr)
{
    std::string previous = get_last_error_copy();
    std::string message = operation != nullptr ? operation : "Operation failed";
    message += ": ";
    message += hresult_to_string(hr);
    if (!previous.empty())
    {
        message += ". ";
        message += previous;
    }
    set_last_error(message);
    return static_cast<int32_t>(hr);
}

HRESULT ensure_thread_com_initialized()
{
    thread_local bool initialized = false;
    if (initialized)
        return S_OK;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
    {
        initialized = true;
        return S_OK;
    }

    if (hr == RPC_E_CHANGED_MODE)
        return S_OK;

    return hr;
}

HRESULT retain_mf()
{
    std::lock_guard lock(g_mf_mutex);
    if (g_mf_ref_count == 0)
    {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr))
            return hr;
    }

    ++g_mf_ref_count;
    return S_OK;
}

void release_mf()
{
    std::lock_guard lock(g_mf_mutex);
    if (g_mf_ref_count <= 0)
        return;

    --g_mf_ref_count;
    if (g_mf_ref_count == 0)
        MFShutdown();
}

bool nvenc_runtime_present()
{
    HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (module == nullptr)
        return false;

    FreeLibrary(module);
    return true;
}

HRESULT find_nvidia_adapter(std::string* adapter_name, IDXGIAdapter1** adapter_out = nullptr)
{
    if (adapter_out != nullptr)
        *adapter_out = nullptr;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return hr;

    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            return hr;

        DXGI_ADAPTER_DESC1 desc{};
        hr = adapter->GetDesc1(&desc);
        if (FAILED(hr))
            return hr;

        if (desc.VendorId == kNvidiaVendorId)
        {
            if (adapter_name != nullptr)
                *adapter_name = wide_to_utf8(desc.Description);
            if (adapter_out != nullptr)
                *adapter_out = adapter.Detach();
            return S_OK;
        }
    }

    return DXGI_ERROR_NOT_FOUND;
}

HRESULT get_device_adapter_info(
    ID3D11Device* device,
    UINT* vendor_id,
    std::string* adapter_name,
    LUID* adapter_luid = nullptr,
    IDXGIAdapter** adapter_out = nullptr)
{
    if (adapter_out != nullptr)
        *adapter_out = nullptr;

    if (device == nullptr)
        return E_POINTER;

    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr))
        return hr;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr))
        return hr;

    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr))
        return hr;

    if (vendor_id != nullptr)
        *vendor_id = desc.VendorId;
    if (adapter_name != nullptr)
        *adapter_name = wide_to_utf8(desc.Description);
    if (adapter_luid != nullptr)
        *adapter_luid = desc.AdapterLuid;
    if (adapter_out != nullptr)
        *adapter_out = adapter.Detach();

    return S_OK;
}

bool is_supported_texture_format(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return true;
    default:
        return false;
    }
}

uint64_t make_sample_duration_hns(int fps)
{
    int safe_fps = std::max(1, fps);
    return 10'000'000ull / static_cast<uint64_t>(safe_fps);
}

int align_to_even(int value)
{
    int safe_value = std::max(1, value);
    return (safe_value + 1) & ~1;
}

HRESULT set_video_type_common(IMFMediaType* type, int width, int height, int fps)
{
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    if (FAILED(hr)) return hr;
    hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, static_cast<UINT32>(std::max(1, fps)), 1);
    if (FAILED(hr)) return hr;
    return MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
}

HRESULT set_input_audio_type_common(IMFMediaType* type, const pr_audio_config& audio)
{
    UINT32 bytes_per_sample = static_cast<UINT32>(std::max(1, audio.bits_per_sample / 8));
    UINT32 channels = static_cast<UINT32>(std::max(1, audio.channels));
    UINT32 sample_rate = static_cast<UINT32>(std::max(1, audio.sample_rate));
    UINT32 block_align = bytes_per_sample * channels;

    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) return hr;
    hr = type->SetGUID(MF_MT_SUBTYPE, audio.is_float ? MFAudioFormat_Float : MFAudioFormat_PCM);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, static_cast<UINT32>(audio.bits_per_sample));
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align);
    if (FAILED(hr)) return hr;
    return type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * block_align);
}

HRESULT set_output_aac_type(IMFMediaType* type, const pr_audio_config& audio)
{
    UINT32 channels = static_cast<UINT32>(std::max(1, audio.channels));
    UINT32 sample_rate = static_cast<UINT32>(std::max(1, audio.sample_rate));
    constexpr UINT32 audio_bitrate_bps = 192'000;

    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) return hr;
    hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audio_bitrate_bps / 8);
    if (FAILED(hr)) return hr;
    hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(hr)) return hr;
    return type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
}
}

struct pr_recorder_t
{
    std::mutex mutex;
    pr_video_config video{};
    pr_audio_config audio{};
    std::wstring output_path;
    bool mf_retained = false;
    bool initialized = false;
    bool stopped = false;
    UINT dxgi_reset_token = 0;
    DWORD video_stream_index = kInvalidStream;
    DWORD audio_stream_index = kInvalidStream;
    int64_t first_video_timestamp_hns = -1;
    int64_t first_audio_timestamp_hns = -1;
    int64_t last_video_sample_time_hns = -1;
    int64_t last_audio_sample_time_hns = -1;
    uint64_t video_sample_duration_hns = 0;
    uint64_t frame_index = 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> device_context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> video_processor_enum;
    ComPtr<ID3D11VideoProcessor> video_processor;
    ComPtr<ID3D11Texture2D> source_copy_texture;
    ComPtr<IMFDXGIDeviceManager> dxgi_manager;
    ComPtr<IMFSinkWriter> sink_writer;

    ~pr_recorder_t()
    {
        sink_writer.Reset();
        source_copy_texture.Reset();
        video_processor.Reset();
        video_processor_enum.Reset();
        video_context.Reset();
        video_device.Reset();
        device_context.Reset();
        dxgi_manager.Reset();
        device.Reset();

        if (mf_retained)
        {
            release_mf();
            mf_retained = false;
        }
    }

    HRESULT initialize_with_adapter(ID3D11Device* source_device, DXGI_FORMAT source_format)
    {
        if (initialized)
            return S_OK;

        if (source_device == nullptr)
            return E_POINTER;
        if (output_path.empty())
            return E_INVALIDARG;
        if (video.width <= 0 || video.height <= 0 || video.fps <= 0)
            return E_INVALIDARG;
        if (video.codec != PR_CODEC_H264)
            return MF_E_TOPO_CODEC_NOT_FOUND;

        const int source_width = video.width;
        const int source_height = video.height;
        const int encoded_width = align_to_even(source_width);
        const int encoded_height = align_to_even(source_height);

        std::string adapter_name;
        UINT vendor_id = 0;
        LUID adapter_luid{};
        ComPtr<IDXGIAdapter> adapter;
        HRESULT hr = get_device_adapter_info(source_device, &vendor_id, &adapter_name, &adapter_luid, &adapter);
        if (FAILED(hr))
            return hr;
        if (vendor_id != kNvidiaVendorId)
        {
            set_last_error("NativeRecorder source game device is not on an NVIDIA adapter; using FFmpeg fallback.");
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        if (!adapter)
            return DXGI_ERROR_NOT_FOUND;

        hr = ensure_thread_com_initialized();
        if (FAILED(hr))
            return hr;

        hr = retain_mf();
        if (FAILED(hr))
            return hr;
        mf_retained = true;

        UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL feature_levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL feature_level{};
        hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            create_flags,
            feature_levels,
            _countof(feature_levels),
            D3D11_SDK_VERSION,
            &device,
            &feature_level,
            &device_context);
        if (FAILED(hr))
            return fail_step("D3D11CreateDevice", hr, "adapter=" + adapter_name);

        device->GetImmediateContext(&device_context);
        if (!device_context)
            return E_FAIL;

        hr = device.As(&video_device);
        if (FAILED(hr))
            return fail_step("QueryInterface(ID3D11VideoDevice)", hr);
        hr = device_context.As(&video_context);
        if (FAILED(hr))
            return fail_step("QueryInterface(ID3D11VideoContext)", hr);

        hr = MFCreateDXGIDeviceManager(&dxgi_reset_token, &dxgi_manager);
        if (FAILED(hr))
            return fail_step("MFCreateDXGIDeviceManager", hr);
        hr = dxgi_manager->ResetDevice(device.Get(), dxgi_reset_token);
        if (FAILED(hr))
            return fail_step("IMFDXGIDeviceManager::ResetDevice", hr);

        ComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes(&attributes, 4);
        if (FAILED(hr))
            return hr;
        hr = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgi_manager.Get());
        if (FAILED(hr))
            return hr;
        hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (FAILED(hr))
            return hr;

        hr = MFCreateSinkWriterFromURL(output_path.c_str(), nullptr, attributes.Get(), &sink_writer);
        if (FAILED(hr))
            return fail_step("MFCreateSinkWriterFromURL", hr, "output=" + wide_to_utf8(output_path.c_str()));

        ComPtr<IMFMediaType> video_out;
        hr = MFCreateMediaType(&video_out);
        if (FAILED(hr))
            return hr;
        hr = set_video_type_common(video_out.Get(), encoded_width, encoded_height, video.fps);
        if (FAILED(hr))
            return fail_step("Set video output media type common attributes", hr);
        hr = video_out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(hr))
            return fail_step("Set video output subtype H264", hr);
        hr = video_out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(video.bitrate_bps > 0 ? video.bitrate_bps : 12'000'000));
        if (FAILED(hr))
            return fail_step("Set video output bitrate", hr);
        hr = sink_writer->AddStream(video_out.Get(), &video_stream_index);
        if (FAILED(hr))
            return fail_step("IMFSinkWriter::AddStream(video H264)", hr, "encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height));

        ComPtr<IMFMediaType> video_in;
        hr = MFCreateMediaType(&video_in);
        if (FAILED(hr))
            return hr;
        hr = set_video_type_common(video_in.Get(), encoded_width, encoded_height, video.fps);
        if (FAILED(hr))
            return fail_step("Set video input media type common attributes", hr);
        hr = video_in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(hr))
            return fail_step("Set video input subtype NV12", hr);
        hr = video_in->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        if (FAILED(hr))
            return fail_step("Set video input fixed-size samples", hr);
        hr = video_in->SetUINT32(MF_MT_SAMPLE_SIZE, static_cast<UINT32>(encoded_width * encoded_height * 3 / 2));
        if (FAILED(hr))
            return fail_step("Set video input sample size", hr, "encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height));
        hr = sink_writer->SetInputMediaType(video_stream_index, video_in.Get(), nullptr);
        if (FAILED(hr))
            return fail_step("IMFSinkWriter::SetInputMediaType(video NV12)", hr, "source=" + std::to_string(source_width) + "x" + std::to_string(source_height) + ", encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height));

        if (audio.enabled)
        {
            ComPtr<IMFMediaType> audio_out;
            hr = MFCreateMediaType(&audio_out);
            if (FAILED(hr))
                return hr;
            hr = set_output_aac_type(audio_out.Get(), audio);
            if (FAILED(hr))
                return hr;
            hr = sink_writer->AddStream(audio_out.Get(), &audio_stream_index);
            if (FAILED(hr))
                return hr;

            ComPtr<IMFMediaType> audio_in;
            hr = MFCreateMediaType(&audio_in);
            if (FAILED(hr))
                return hr;
            hr = set_input_audio_type_common(audio_in.Get(), audio);
            if (FAILED(hr))
                return hr;
            hr = sink_writer->SetInputMediaType(audio_stream_index, audio_in.Get(), nullptr);
            if (FAILED(hr))
                return hr;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = static_cast<UINT>(std::max(1, video.fps));
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = static_cast<UINT>(source_width);
        content.InputHeight = static_cast<UINT>(source_height);
        content.OutputFrameRate.Numerator = static_cast<UINT>(std::max(1, video.fps));
        content.OutputFrameRate.Denominator = 1;
        content.OutputWidth = static_cast<UINT>(encoded_width);
        content.OutputHeight = static_cast<UINT>(encoded_height);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        hr = video_device->CreateVideoProcessorEnumerator(&content, &video_processor_enum);
        if (FAILED(hr))
            return fail_step("CreateVideoProcessorEnumerator", hr);

        UINT source_format_support = 0;
        hr = video_processor_enum->CheckVideoProcessorFormat(source_format, &source_format_support);
        if (FAILED(hr))
            return fail_step("CheckVideoProcessorFormat(source)", hr, "format=" + dxgi_format_to_string(source_format));
        if ((source_format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0)
        {
            set_last_error("NativeRecorder video processor does not support source texture as input; format=" +
                dxgi_format_to_string(source_format) +
                ", support=" + hex_uint32(source_format_support) +
                ". This GPU/driver likely needs shader NV12 shared texture or direct NVENC ABGR input.");
            return MF_E_INVALIDMEDIATYPE;
        }

        UINT nv12_format_support = 0;
        hr = video_processor_enum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12_format_support);
        if (FAILED(hr))
            return fail_step("CheckVideoProcessorFormat(NV12)", hr);
        if ((nv12_format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
        {
            set_last_error("NativeRecorder video processor does not support NV12 output; support=" +
                hex_uint32(nv12_format_support) + ".");
            return MF_E_INVALIDMEDIATYPE;
        }

        hr = video_device->CreateVideoProcessor(video_processor_enum.Get(), 0, &video_processor);
        if (FAILED(hr))
            return fail_step("CreateVideoProcessor", hr);

        RECT source_rect{0, 0, source_width, source_height};
        RECT dest_rect{0, 0, source_width, source_height};
        RECT output_rect{0, 0, encoded_width, encoded_height};
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        video_context->VideoProcessorSetStreamFrameFormat(video_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamSourceRect(video_processor.Get(), 0, TRUE, &source_rect);
        video_context->VideoProcessorSetStreamDestRect(video_processor.Get(), 0, TRUE, &dest_rect);
        video_context->VideoProcessorSetOutputTargetRect(video_processor.Get(), TRUE, &output_rect);
        video_context->VideoProcessorSetOutputBackgroundColor(video_processor.Get(), FALSE, &background);

        hr = sink_writer->BeginWriting();
        if (FAILED(hr))
            return fail_step("IMFSinkWriter::BeginWriting", hr);

        video_sample_duration_hns = make_sample_duration_hns(video.fps);
        initialized = true;

        std::string message = "NativeRecorder initialized: source NVIDIA adapter=" + adapter_name +
            ", luid=" + std::to_string(static_cast<uint32_t>(adapter_luid.HighPart)) + ":" +
            std::to_string(adapter_luid.LowPart) +
            ", sourceFormat=" + dxgi_format_to_string(source_format) +
            ", encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height) +
            ", pad=" + std::to_string(encoded_width - source_width) + "x" + std::to_string(encoded_height - source_height) +
            ", vpSourceSupport=" + hex_uint32(source_format_support) +
            ", vpNv12Support=" + hex_uint32(nv12_format_support) +
            ", output=H264/MP4 via Media Foundation D3D11 texture input.";
        set_last_error(message);
        return S_OK;
    }

    HRESULT ensure_source_copy_texture(const D3D11_TEXTURE2D_DESC& source_desc)
    {
        if (source_copy_texture)
        {
            D3D11_TEXTURE2D_DESC existing{};
            source_copy_texture->GetDesc(&existing);
            constexpr UINT required_bind_flags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (existing.Width == source_desc.Width &&
                existing.Height == source_desc.Height &&
                existing.Format == source_desc.Format &&
                (existing.BindFlags & required_bind_flags) == required_bind_flags)
            {
                return S_OK;
            }

            source_copy_texture.Reset();
        }

        D3D11_TEXTURE2D_DESC copy_desc = source_desc;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        copy_desc.CPUAccessFlags = 0;
        copy_desc.MiscFlags = 0;

        return device->CreateTexture2D(&copy_desc, nullptr, &source_copy_texture);
    }

    HRESULT submit_shared_texture(ID3D11Device* source_device, HANDLE shared_handle, DXGI_FORMAT source_format, int64_t timestamp_hns)
    {
        if (stopped)
            return MF_E_SHUTDOWN;
        if (source_device == nullptr || shared_handle == nullptr)
            return E_POINTER;

        HRESULT hr = initialize_with_adapter(source_device, source_format);
        if (FAILED(hr))
            return hr;

        ComPtr<ID3D11Texture2D> source_texture;
        hr = device->OpenSharedResource(shared_handle, __uuidof(ID3D11Texture2D), &source_texture);
        if (FAILED(hr))
        {
            set_last_error("NativeRecorder failed to open shared texture on the source adapter: " + hresult_to_string(hr));
            return hr;
        }

        D3D11_TEXTURE2D_DESC source_desc{};
        source_texture->GetDesc(&source_desc);
        if (source_desc.Width != static_cast<UINT>(video.width) ||
            source_desc.Height != static_cast<UINT>(video.height))
        {
            set_last_error("NativeRecorder source texture size changed; expected=" +
                std::to_string(video.width) + "x" + std::to_string(video.height) +
                ", actual=" + texture_desc_to_string(source_desc));
            return E_INVALIDARG;
        }
        if (source_desc.SampleDesc.Count != 1)
        {
            set_last_error("NativeRecorder does not support MSAA swap-chain textures.");
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        if (!is_supported_texture_format(source_desc.Format) || !is_supported_texture_format(source_format))
        {
            set_last_error("NativeRecorder source texture format is not supported.");
            return MF_E_INVALIDMEDIATYPE;
        }

        const int encoded_width = align_to_even(video.width);
        const int encoded_height = align_to_even(video.height);

        ComPtr<IDXGIKeyedMutex> keyed_mutex;
        hr = source_texture.As(&keyed_mutex);

        hr = ensure_source_copy_texture(source_desc);
        if (FAILED(hr))
            return fail_step("CreateTexture2D(source copy)", hr, "source=" + texture_desc_to_string(source_desc));

        bool mutex_acquired = false;
        if (keyed_mutex)
        {
            hr = keyed_mutex->AcquireSync(kEncoderReadKey, kSharedTextureAcquireTimeoutMs);
            if (hr == WAIT_TIMEOUT || hr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                set_last_error("NativeRecorder shared texture was not ready; dropping one frame.");
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
            if (FAILED(hr))
                return fail_step("IDXGIKeyedMutex::AcquireSync", hr, "source=" + texture_desc_to_string(source_desc));

            mutex_acquired = true;
        }

        device_context->CopyResource(source_copy_texture.Get(), source_texture.Get());
        if (keyed_mutex)
        {
            HRESULT release_hr = keyed_mutex->ReleaseSync(kGameWriteKey);
            mutex_acquired = false;
            if (FAILED(release_hr))
                return fail_step("IDXGIKeyedMutex::ReleaseSync", release_hr, "source=" + texture_desc_to_string(source_desc));
        }

        D3D11_TEXTURE2D_DESC output_desc{};
        output_desc.Width = static_cast<UINT>(encoded_width);
        output_desc.Height = static_cast<UINT>(encoded_height);
        output_desc.MipLevels = 1;
        output_desc.ArraySize = 1;
        output_desc.Format = DXGI_FORMAT_NV12;
        output_desc.SampleDesc.Count = 1;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> nv12_texture;
        hr = device->CreateTexture2D(&output_desc, nullptr, &nv12_texture);
        if (FAILED(hr))
            return fail_step("CreateTexture2D(NV12 output)", hr, "output=" + texture_desc_to_string(output_desc));

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc{};
        input_view_desc.FourCC = 0;
        input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_view_desc.Texture2D.MipSlice = 0;
        input_view_desc.Texture2D.ArraySlice = 0;

        ComPtr<ID3D11VideoProcessorInputView> input_view;
        hr = video_device->CreateVideoProcessorInputView(source_copy_texture.Get(), video_processor_enum.Get(), &input_view_desc, &input_view);
        if (FAILED(hr))
        {
            if (mutex_acquired && keyed_mutex)
                keyed_mutex->ReleaseSync(kGameWriteKey);
            return fail_step("CreateVideoProcessorInputView", hr, "source=" + texture_desc_to_string(source_desc));
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
        output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output_view_desc.Texture2D.MipSlice = 0;

        ComPtr<ID3D11VideoProcessorOutputView> output_view;
        hr = video_device->CreateVideoProcessorOutputView(nv12_texture.Get(), video_processor_enum.Get(), &output_view_desc, &output_view);
        if (FAILED(hr))
            return fail_step("CreateVideoProcessorOutputView", hr, "output=" + texture_desc_to_string(output_desc));

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = input_view.Get();

        hr = video_context->VideoProcessorBlt(
            video_processor.Get(),
            output_view.Get(),
            static_cast<UINT>(frame_index),
            1,
            &stream);
        if (FAILED(hr))
            return fail_step("VideoProcessorBlt", hr, "source=" + texture_desc_to_string(source_desc) + "; output=" + texture_desc_to_string(output_desc));

        ComPtr<IMFMediaBuffer> dxgi_buffer;
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_texture.Get(), 0, FALSE, &dxgi_buffer);
        if (FAILED(hr))
            return fail_step("MFCreateDXGISurfaceBuffer(NV12)", hr, "output=" + texture_desc_to_string(output_desc));

        DWORD nv12_byte_count = static_cast<DWORD>(encoded_width * encoded_height * 3 / 2);
        hr = dxgi_buffer->SetCurrentLength(nv12_byte_count);
        if (FAILED(hr))
            return fail_step("IMFMediaBuffer::SetCurrentLength(NV12)", hr, "bytes=" + std::to_string(nv12_byte_count));

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr))
            return fail_step("MFCreateSample(video)", hr);
        hr = sample->AddBuffer(dxgi_buffer.Get());
        if (FAILED(hr))
            return fail_step("IMFSample::AddBuffer(video)", hr);

        if (first_video_timestamp_hns < 0)
            first_video_timestamp_hns = timestamp_hns;

        int64_t sample_time = std::max<int64_t>(0, timestamp_hns - first_video_timestamp_hns);
        if (last_video_sample_time_hns >= 0 && sample_time <= last_video_sample_time_hns)
            sample_time = last_video_sample_time_hns + static_cast<int64_t>(video_sample_duration_hns);

        hr = sample->SetSampleTime(sample_time);
        if (FAILED(hr))
            return fail_step("IMFSample::SetSampleTime(video)", hr, "sampleTime=" + std::to_string(sample_time));
        hr = sample->SetSampleDuration(static_cast<LONGLONG>(video_sample_duration_hns));
        if (FAILED(hr))
            return fail_step("IMFSample::SetSampleDuration(video)", hr, "duration=" + std::to_string(video_sample_duration_hns));

        hr = sink_writer->WriteSample(video_stream_index, sample.Get());
        if (FAILED(hr))
            return fail_step("IMFSinkWriter::WriteSample(video)", hr, "sampleTime=" + std::to_string(sample_time) + ", duration=" + std::to_string(video_sample_duration_hns));

        last_video_sample_time_hns = sample_time;
        ++frame_index;
        return S_OK;
    }

    HRESULT submit_audio(const void* data, int32_t byte_count, int64_t timestamp_hns)
    {
        if (!audio.enabled || audio_stream_index == kInvalidStream)
            return S_OK;
        if (!initialized)
            return MF_E_NOT_INITIALIZED;
        if (data == nullptr || byte_count <= 0)
            return E_INVALIDARG;

        HRESULT hr = ensure_thread_com_initialized();
        if (FAILED(hr))
            return hr;

        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(byte_count), &buffer);
        if (FAILED(hr))
            return hr;

        BYTE* dst = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        hr = buffer->Lock(&dst, &max_length, &current_length);
        if (FAILED(hr))
            return hr;

        std::memcpy(dst, data, static_cast<size_t>(byte_count));
        buffer->Unlock();
        hr = buffer->SetCurrentLength(static_cast<DWORD>(byte_count));
        if (FAILED(hr))
            return hr;

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr))
            return hr;
        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr))
            return hr;

        int block_align = std::max(1, audio.channels * audio.bits_per_sample / 8);
        int64_t sample_count = byte_count / block_align;
        int64_t duration_hns = sample_count * 10'000'000ll / std::max(1, audio.sample_rate);

        if (first_audio_timestamp_hns < 0)
            first_audio_timestamp_hns = timestamp_hns;

        int64_t sample_time = std::max<int64_t>(0, timestamp_hns - first_audio_timestamp_hns);
        if (last_audio_sample_time_hns >= 0 && sample_time <= last_audio_sample_time_hns)
            sample_time = last_audio_sample_time_hns + duration_hns;

        hr = sample->SetSampleTime(sample_time);
        if (FAILED(hr))
            return hr;
        hr = sample->SetSampleDuration(duration_hns);
        if (FAILED(hr))
            return hr;

        hr = sink_writer->WriteSample(audio_stream_index, sample.Get());
        if (FAILED(hr))
            return hr;

        last_audio_sample_time_hns = sample_time;
        return S_OK;
    }

    HRESULT stop()
    {
        if (stopped)
            return S_OK;

        stopped = true;
        if (sink_writer)
            return sink_writer->Finalize();

        return S_OK;
    }
};

PR_API int32_t PR_CALL pr_get_abi_version(void)
{
    return PR_ABI_VERSION;
}

PR_API int32_t PR_CALL pr_probe(pr_probe_info* info)
{
    if (info == nullptr)
        return PR_E_INVALID_ARGUMENT;

    std::memset(info, 0, sizeof(*info));

    HRESULT hr = ensure_thread_com_initialized();
    if (FAILED(hr))
    {
        std::string message = "COM initialization failed: " + hresult_to_string(hr);
        copy_text(info->message, sizeof(info->message), message.c_str());
        set_last_error(message);
        return PR_OK;
    }

    std::string adapter_name;
    hr = find_nvidia_adapter(&adapter_name);
    if (FAILED(hr))
    {
        copy_text(info->message, sizeof(info->message), "No NVIDIA DXGI adapter was found.");
        set_last_error(info->message);
        return PR_OK;
    }

    if (!nvenc_runtime_present())
    {
        copy_text(info->adapter_name, sizeof(info->adapter_name), adapter_name.c_str());
        copy_text(info->message, sizeof(info->message), "NVIDIA adapter was found, but nvEncodeAPI64.dll was not found.");
        info->is_nvidia_adapter = 1;
        set_last_error(info->message);
        return PR_OK;
    }

    HRESULT mf_hr = retain_mf();
    if (FAILED(mf_hr))
    {
        std::string message = "Media Foundation startup failed: " + hresult_to_string(mf_hr);
        copy_text(info->adapter_name, sizeof(info->adapter_name), adapter_name.c_str());
        copy_text(info->message, sizeof(info->message), message.c_str());
        info->is_nvidia_adapter = 1;
        set_last_error(message);
        return PR_OK;
    }
    release_mf();

    copy_text(info->adapter_name, sizeof(info->adapter_name), adapter_name.c_str());
    copy_text(info->message, sizeof(info->message), "NativeRecorder NVIDIA D3D11 texture path is available.");
    info->is_nvidia_adapter = 1;
    info->supports_d3d11_texture_input = 1;
    set_last_error(info->message);
    return PR_OK;
}

PR_API int32_t PR_CALL pr_create(const pr_video_config* video, const pr_audio_config* audio, pr_recorder_t** recorder)
{
    if (video == nullptr || recorder == nullptr)
        return PR_E_INVALID_ARGUMENT;

    *recorder = nullptr;
    if (video->output_path == nullptr || video->width <= 0 || video->height <= 0 || video->fps <= 0)
    {
        set_last_error("Invalid NativeRecorder video configuration.");
        return PR_E_INVALID_ARGUMENT;
    }

    if (video->codec != PR_CODEC_H264)
    {
        set_last_error("NativeRecorder prototype currently supports H.264 only.");
        return PR_E_NOT_AVAILABLE;
    }

    auto* instance = new (std::nothrow) pr_recorder_t();
    if (instance == nullptr)
    {
        set_last_error("Failed to allocate NativeRecorder instance.");
        return E_OUTOFMEMORY;
    }

    instance->video = *video;
    if (audio != nullptr)
        instance->audio = *audio;
    instance->output_path = video->output_path;

    *recorder = instance;
    set_last_error("NativeRecorder instance created; waiting for first D3D11 texture.");
    return PR_OK;
}

PR_API int32_t PR_CALL pr_submit_d3d11_texture(
    pr_recorder_t* recorder,
    void* d3d11_device,
    void* d3d11_texture,
    int32_t dxgi_format,
    int64_t timestamp_hns)
{
    (void)recorder;
    (void)d3d11_device;
    (void)d3d11_texture;
    (void)dxgi_format;
    (void)timestamp_hns;
    set_last_error("Direct D3D11 texture pointer submission is disabled; use shared texture ABI v5.");
    return PR_E_NOT_IMPLEMENTED;
}

PR_API int32_t PR_CALL pr_submit_d3d11_shared_texture(
    pr_recorder_t* recorder,
    void* d3d11_device,
    void* shared_handle,
    int32_t dxgi_format,
    int64_t timestamp_hns)
{
    if (recorder == nullptr || d3d11_device == nullptr || shared_handle == nullptr)
        return PR_E_INVALID_ARGUMENT;

    HRESULT hr = ensure_thread_com_initialized();
    if (FAILED(hr))
        return fail_hr("COM initialization failed", hr);

    std::lock_guard lock(recorder->mutex);
    hr = recorder->submit_shared_texture(
        static_cast<ID3D11Device*>(d3d11_device),
        static_cast<HANDLE>(shared_handle),
        static_cast<DXGI_FORMAT>(dxgi_format),
        timestamp_hns);
    if (FAILED(hr))
        return fail_hr("NativeRecorder texture submit failed", hr);

    return PR_OK;
}

PR_API int32_t PR_CALL pr_submit_audio(pr_recorder_t* recorder, const void* data, int32_t byte_count, int64_t timestamp_hns)
{
    if (recorder == nullptr)
        return PR_E_INVALID_ARGUMENT;

    std::lock_guard lock(recorder->mutex);
    HRESULT hr = recorder->submit_audio(data, byte_count, timestamp_hns);
    if (FAILED(hr))
        return fail_hr("NativeRecorder audio submit failed", hr);

    return PR_OK;
}

PR_API int32_t PR_CALL pr_stop(pr_recorder_t* recorder)
{
    if (recorder == nullptr)
        return PR_E_INVALID_ARGUMENT;

    HRESULT hr;
    {
        std::lock_guard lock(recorder->mutex);
        hr = recorder->stop();
    }

    if (FAILED(hr))
        return fail_hr("NativeRecorder finalize failed", hr);

    set_last_error("NativeRecorder finalized.");
    return PR_OK;
}

PR_API void PR_CALL pr_destroy(pr_recorder_t* recorder)
{
    delete recorder;
}

PR_API int32_t PR_CALL pr_get_last_error(char* buffer, int32_t buffer_size)
{
    if (buffer == nullptr || buffer_size <= 0)
        return PR_E_INVALID_ARGUMENT;

    std::lock_guard lock(g_error_mutex);
    copy_text(buffer, static_cast<size_t>(buffer_size), g_last_error.c_str());
    return PR_OK;
}
