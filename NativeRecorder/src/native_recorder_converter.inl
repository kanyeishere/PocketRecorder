// ReShade's DXGI proxy can fault while releasing probe staging textures during pr_stop.
// Keep GPU readback probes disabled until they have a teardown path independent of the game process.
static constexpr bool kEnableTextureContentProbes = false;

struct NativeTextureContentProbe
{
    static constexpr uint64_t kProbeIntervalFrames = 300;
    static constexpr UINT kGridColumns = 64;
    static constexpr UINT kGridRows = 36;

    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> completion_query;
    D3D11_TEXTURE2D_DESC staging_desc{};
    bool pending = false;
    uint64_t scheduled = 0;
    uint64_t completed = 0;
    uint64_t not_ready_polls = 0;
    uint64_t create_failures = 0;
    uint64_t query_failures = 0;
    uint64_t map_failures = 0;
    uint64_t analysis_failures = 0;
    uint64_t all_black_frames = 0;
    uint64_t hash_changes = 0;
    uint64_t current_same_hash_streak = 0;
    uint64_t max_same_hash_streak = 0;
    uint64_t last_hash = 0;
    uint64_t pending_frame_index = 0;
    uint64_t last_frame_index = 0;
    UINT last_avg0 = 0;
    UINT last_avg1 = 0;
    UINT last_avg2 = 0;
    UINT last_avg3 = 0;
    bool has_hash = false;
    std::string last_sample = "none";

    void tick(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        uint64_t frame_index) noexcept
    {
        try
        {
            poll(context);
            if (pending || device == nullptr || context == nullptr || texture == nullptr)
                return;
            if (frame_index != 0 && frame_index % kProbeIntervalFrames != 0)
                return;

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (!ensure_resources(device, desc))
                return;

            context->CopyResource(staging.Get(), texture);
            context->End(completion_query.Get());
            pending = true;
            pending_frame_index = frame_index;
            ++scheduled;
        }
        catch (...)
        {
            ++analysis_failures;
            pending = false;
        }
    }

    std::string summary(const char* label) const
    {
        std::string health = "no-probe";
        if (completed > 0)
        {
            health = all_black_frames == completed
                ? "all-black"
                : all_black_frames > 0 ? "intermittent-black" : "content-present";
        }

        return std::string(label != nullptr ? label : "texture") +
            "Health=" + health +
            ",scheduled=" + std::to_string(scheduled) +
            ",completed=" + std::to_string(completed) +
            ",notReadyPolls=" + std::to_string(not_ready_polls) +
            ",createFailures=" + std::to_string(create_failures) +
            ",queryFailures=" + std::to_string(query_failures) +
            ",mapFailures=" + std::to_string(map_failures) +
            ",analysisFailures=" + std::to_string(analysis_failures) +
            ",blackProbes=" + std::to_string(all_black_frames) +
            ",hashChanges=" + std::to_string(hash_changes) +
            ",maxSameHashStreak=" + std::to_string(max_same_hash_streak) +
            ",lastHash=" + hex_uint64(last_hash) +
            ",lastFrameIndex=" + std::to_string(last_frame_index) +
            ",lastSample=[" + last_sample + "]";
    }

    void reset()
    {
        staging.Reset();
        completion_query.Reset();
        staging_desc = {};
        pending = false;
        scheduled = 0;
        completed = 0;
        not_ready_polls = 0;
        create_failures = 0;
        query_failures = 0;
        map_failures = 0;
        analysis_failures = 0;
        all_black_frames = 0;
        hash_changes = 0;
        current_same_hash_streak = 0;
        max_same_hash_streak = 0;
        last_hash = 0;
        pending_frame_index = 0;
        last_frame_index = 0;
        last_avg0 = 0;
        last_avg1 = 0;
        last_avg2 = 0;
        last_avg3 = 0;
        has_hash = false;
        last_sample = "none";
    }

private:
    static std::string hex_uint64(uint64_t value)
    {
        char buffer[32]{};
        sprintf_s(buffer, "0x%016llX", static_cast<unsigned long long>(value));
        return buffer;
    }

    bool ensure_resources(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc)
    {
        if (staging && completion_query &&
            staging_desc.Width == desc.Width &&
            staging_desc.Height == desc.Height &&
            staging_desc.Format == desc.Format)
        {
            return true;
        }

        staging.Reset();
        completion_query.Reset();
        pending = false;

        D3D11_TEXTURE2D_DESC staging_candidate = desc;
        staging_candidate.MipLevels = 1;
        staging_candidate.ArraySize = 1;
        staging_candidate.SampleDesc.Count = 1;
        staging_candidate.SampleDesc.Quality = 0;
        staging_candidate.Usage = D3D11_USAGE_STAGING;
        staging_candidate.BindFlags = 0;
        staging_candidate.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_candidate.MiscFlags = 0;
        HRESULT hr = device->CreateTexture2D(&staging_candidate, nullptr, &staging);
        if (FAILED(hr) || !staging)
        {
            ++create_failures;
            return false;
        }

        D3D11_QUERY_DESC query_desc{};
        query_desc.Query = D3D11_QUERY_EVENT;
        hr = device->CreateQuery(&query_desc, &completion_query);
        if (FAILED(hr) || !completion_query)
        {
            staging.Reset();
            ++create_failures;
            return false;
        }

        staging_desc = staging_candidate;
        return true;
    }

    void poll(ID3D11DeviceContext* context)
    {
        if (!pending || context == nullptr || !completion_query || !staging)
            return;

        HRESULT hr = context->GetData(
            completion_query.Get(),
            nullptr,
            0,
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE)
        {
            ++not_ready_polls;
            return;
        }
        if (FAILED(hr))
        {
            ++query_failures;
            pending = false;
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(
            staging.Get(),
            0,
            D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT,
            &mapped);
        if (FAILED(hr) || mapped.pData == nullptr)
        {
            ++map_failures;
            pending = false;
            return;
        }

        analyze(mapped);
        context->Unmap(staging.Get(), 0);
        pending = false;
    }

    void analyze(const D3D11_MAPPED_SUBRESOURCE& mapped)
    {
        const auto* base = static_cast<const uint8_t*>(mapped.pData);
        if (base == nullptr || mapped.RowPitch == 0 || staging_desc.Width == 0 || staging_desc.Height == 0)
        {
            ++analysis_failures;
            return;
        }

        uint64_t hash = 1469598103934665603ull;
        auto hash_byte = [&hash](uint8_t value)
        {
            hash ^= value;
            hash *= 1099511628211ull;
        };

        uint64_t sum0 = 0;
        uint64_t sum1 = 0;
        uint64_t sum2 = 0;
        uint64_t sum3 = 0;
        UINT min0 = 255;
        UINT min1 = 255;
        UINT min2 = 255;
        UINT min3 = 255;
        UINT max0 = 0;
        UINT max1 = 0;
        UINT max2 = 0;
        UINT max3 = 0;
        UINT sample_count = 0;
        UINT max_luma = 0;

        const bool is_nv12 = staging_desc.Format == DXGI_FORMAT_NV12;
        const bool is_bgra = staging_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            staging_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
            staging_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;

        for (UINT row = 0; row < kGridRows; ++row)
        {
            const UINT y = std::min(
                staging_desc.Height - 1,
                static_cast<UINT>((static_cast<uint64_t>(row) * staging_desc.Height + staging_desc.Height / 2) / kGridRows));
            for (UINT column = 0; column < kGridColumns; ++column)
            {
                const UINT x = std::min(
                    staging_desc.Width - 1,
                    static_cast<UINT>((static_cast<uint64_t>(column) * staging_desc.Width + staging_desc.Width / 2) / kGridColumns));

                UINT value0 = 0;
                UINT value1 = 0;
                UINT value2 = 0;
                UINT value3 = 255;
                if (is_nv12)
                {
                    const auto* y_row = base + static_cast<size_t>(mapped.RowPitch) * y;
                    const auto* uv_base = base + static_cast<size_t>(mapped.RowPitch) * staging_desc.Height;
                    const auto* uv_row = uv_base + static_cast<size_t>(mapped.RowPitch) * (y / 2);
                    const UINT uv_x = std::min(staging_desc.Width - 2, x & ~1u);
                    value0 = y_row[x];
                    value1 = uv_row[uv_x];
                    value2 = uv_row[uv_x + 1];
                    value3 = 255;
                    max_luma = std::max(max_luma, value0);
                }
                else
                {
                    const auto* pixel = base + static_cast<size_t>(mapped.RowPitch) * y + static_cast<size_t>(x) * 4;
                    const UINT first = pixel[0];
                    const UINT second = pixel[1];
                    const UINT third = pixel[2];
                    value0 = is_bgra ? third : first;
                    value1 = second;
                    value2 = is_bgra ? first : third;
                    value3 = pixel[3];
                    const UINT luma = (54 * value0 + 183 * value1 + 19 * value2) >> 8;
                    max_luma = std::max(max_luma, luma);
                }

                const UINT values[4] = {value0, value1, value2, value3};
                sum0 += value0;
                sum1 += value1;
                sum2 += value2;
                sum3 += value3;
                min0 = std::min(min0, value0);
                min1 = std::min(min1, value1);
                min2 = std::min(min2, value2);
                min3 = std::min(min3, value3);
                max0 = std::max(max0, value0);
                max1 = std::max(max1, value1);
                max2 = std::max(max2, value2);
                max3 = std::max(max3, value3);
                for (UINT value : values)
                    hash_byte(static_cast<uint8_t>(value));
                ++sample_count;
            }
        }

        if (sample_count == 0)
        {
            ++analysis_failures;
            return;
        }

        const UINT avg0 = static_cast<UINT>(sum0 / sample_count);
        const UINT avg1 = static_cast<UINT>(sum1 / sample_count);
        const UINT avg2 = static_cast<UINT>(sum2 / sample_count);
        const UINT avg3 = static_cast<UINT>(sum3 / sample_count);
        last_avg0 = avg0;
        last_avg1 = avg1;
        last_avg2 = avg2;
        last_avg3 = avg3;
        last_frame_index = pending_frame_index;
        const bool black = is_nv12 ? max_luma <= 20 : max_luma <= 8;
        if (black)
            ++all_black_frames;

        if (has_hash && hash == last_hash)
        {
            ++current_same_hash_streak;
        }
        else
        {
            if (has_hash)
                ++hash_changes;
            current_same_hash_streak = 1;
        }
        max_same_hash_streak = std::max(max_same_hash_streak, current_same_hash_streak);
        has_hash = true;
        last_hash = hash;
        ++completed;

        if (is_nv12)
        {
            last_sample = "Y(avg/min/max)=" + std::to_string(avg0) + "/" + std::to_string(min0) + "/" + std::to_string(max0) +
                ",U=" + std::to_string(avg1) + "/" + std::to_string(min1) + "/" + std::to_string(max1) +
                ",V=" + std::to_string(avg2) + "/" + std::to_string(min2) + "/" + std::to_string(max2) +
                ",rowPitch=" + std::to_string(mapped.RowPitch);
        }
        else
        {
            last_sample = "R(avg/min/max)=" + std::to_string(avg0) + "/" + std::to_string(min0) + "/" + std::to_string(max0) +
                ",G=" + std::to_string(avg1) + "/" + std::to_string(min1) + "/" + std::to_string(max1) +
                ",B=" + std::to_string(avg2) + "/" + std::to_string(min2) + "/" + std::to_string(max2) +
                ",A=" + std::to_string(avg3) + "/" + std::to_string(min3) + "/" + std::to_string(max3) +
                ",rowPitch=" + std::to_string(mapped.RowPitch);
        }
    }
};

struct SharedTextureNv12Converter
{
    pr_video_config video{};
    UINT required_vendor_id = kNvidiaVendorId;
    const char* required_vendor_name = "NVIDIA";
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> device_context;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<ID3D11VideoProcessorEnumerator> video_processor_enum;
    ComPtr<ID3D11VideoProcessor> video_processor;
    HANDLE cached_shared_handle = nullptr;
    ComPtr<ID3D11Texture2D> cached_source_texture;
    ComPtr<IDXGIKeyedMutex> cached_keyed_mutex;
    ComPtr<ID3D11Texture2D> source_copy_texture;
    ComPtr<ID3D11VideoProcessorInputView> source_copy_input_view;
    bool source_copy_ready = false;
    struct OutputViewCacheEntry
    {
        ID3D11Texture2D* texture = nullptr;
        ComPtr<ID3D11VideoProcessorOutputView> view;
    };
    std::vector<OutputViewCacheEntry> output_view_cache;
    std::string adapter_name;
    LUID adapter_luid{};
    UINT source_format_support = 0;
    UINT nv12_format_support = 0;
    uint64_t frame_index = 0;
    std::atomic<uint64_t> keyed_acquire_successes{0};
    std::atomic<uint64_t> keyed_acquire_timeouts{0};
    std::atomic<uint64_t> keyed_release_successes{0};
    std::atomic<uint64_t> fresh_shared_copies{0};
    std::atomic<uint64_t> reused_private_copies{0};
    mutable std::mutex content_probe_mutex;
    NativeTextureContentProbe source_content_probe;
    NativeTextureContentProbe nv12_content_probe;
    uint64_t matched_color_probes = 0;
    uint64_t mismatched_color_probes = 0;
    uint64_t matched_source_hash_changes = 0;
    uint64_t matched_nv12_hash_changes = 0;
    uint64_t last_matched_color_frame = 0;
    uint64_t last_matched_source_hash = 0;
    uint64_t last_matched_nv12_hash = 0;
    int last_expected_y = 0;
    int last_expected_u = 0;
    int last_expected_v = 0;
    int last_actual_y = 0;
    int last_actual_u = 0;
    int last_actual_v = 0;
    int last_delta_y = 0;
    int last_delta_u = 0;
    int last_delta_v = 0;
    bool has_matched_color_probe = false;
    bool last_color_probe_plausible = false;
    bool initialized = false;

    int source_width() const { return video.width; }
    int source_height() const { return video.height; }
    int output_width() const { return video_output_width(video); }
    int output_height() const { return video_output_height(video); }
    int encoded_width() const { return video_encoded_width(video); }
    int encoded_height() const { return video_encoded_height(video); }

    HRESULT initialize(ID3D11Device* source_device, DXGI_FORMAT source_format)
    {
        if (initialized)
            return S_OK;
        if (source_device == nullptr)
            return E_POINTER;
        if (video.width <= 0 || video.height <= 0 || output_width() <= 0 || output_height() <= 0 || video.fps <= 0)
            return E_INVALIDARG;

        UINT vendor_id = 0;
        HRESULT hr = get_device_adapter_info(source_device, &vendor_id, &adapter_name, &adapter_luid);
        if (FAILED(hr))
            return hr;
        if (vendor_id != required_vendor_id)
        {
            set_last_error(std::string("NativeRecorder source game device is not on a ") +
                required_vendor_name + " adapter; using FFmpeg fallback.");
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        const std::string source_adapter_name = adapter_name;
        std::string system_adapter_name;
        std::string system_adapter_report;
        ComPtr<IDXGIAdapter> system_adapter;
        hr = find_system_adapter_by_luid(
            adapter_luid,
            &system_adapter_name,
            &system_adapter,
            &system_adapter_report);
        if (FAILED(hr) || !system_adapter)
        {
            return fail_step(
                "System32 CreateDXGIFactory1/EnumAdapters1(adapter LUID)",
                FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND,
                "sourceAdapter=" + source_adapter_name +
                    ", sourceLuid=" + adapter_luid_to_string(adapter_luid) +
                    ", systemAdapters=[" + system_adapter_report + "]");
        }
        adapter_name = system_adapter_name;

        UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL feature_levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL feature_level{};
        auto create_device = get_system_d3d11_create_device();
        if (create_device == nullptr)
            return fail_step("LoadLibraryExW(System32\\d3d11.dll)", HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));

        hr = create_device(
            system_adapter.Get(),
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
        {
            return fail_step(
                "System32 D3D11CreateDevice(system adapter by LUID)",
                hr,
                "sourceAdapter=" + source_adapter_name +
                    ", sourceLuid=" + adapter_luid_to_string(adapter_luid) +
                    ", systemAdapter=" + adapter_name);
        }

        device->GetImmediateContext(&device_context);
        if (!device_context)
            return E_FAIL;

        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);

        hr = device.As(&video_device);
        if (FAILED(hr))
            return fail_step("QueryInterface(ID3D11VideoDevice)", hr);
        hr = device_context.As(&video_context);
        if (FAILED(hr))
            return fail_step("QueryInterface(ID3D11VideoContext)", hr);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = static_cast<UINT>(std::max(1, video.fps));
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = static_cast<UINT>(source_width());
        content.InputHeight = static_cast<UINT>(source_height());
        content.OutputFrameRate.Numerator = static_cast<UINT>(std::max(1, video.fps));
        content.OutputFrameRate.Denominator = 1;
        content.OutputWidth = static_cast<UINT>(encoded_width());
        content.OutputHeight = static_cast<UINT>(encoded_height());
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        hr = video_device->CreateVideoProcessorEnumerator(&content, &video_processor_enum);
        if (FAILED(hr))
            return fail_step("CreateVideoProcessorEnumerator", hr);

        hr = video_processor_enum->CheckVideoProcessorFormat(source_format, &source_format_support);
        if (FAILED(hr))
            return fail_step("CheckVideoProcessorFormat(source)", hr, "format=" + dxgi_format_to_string(source_format));
        if ((source_format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0)
        {
            set_last_error("NativeRecorder video processor does not support source texture as input; format=" +
                dxgi_format_to_string(source_format) +
                ", support=" + hex_uint32(source_format_support) +
                ". This GPU/driver likely needs shader NV12 shared texture or direct encoder input.");
            return E_INVALIDARG;
        }

        hr = video_processor_enum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12_format_support);
        if (FAILED(hr))
            return fail_step("CheckVideoProcessorFormat(NV12)", hr);
        if ((nv12_format_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
        {
            set_last_error("NativeRecorder video processor does not support NV12 output; support=" +
                hex_uint32(nv12_format_support) + ".");
            return E_INVALIDARG;
        }

        hr = video_device->CreateVideoProcessor(video_processor_enum.Get(), 0, &video_processor);
        if (FAILED(hr))
            return fail_step("CreateVideoProcessor", hr);

        RECT source_rect{0, 0, source_width(), source_height()};
        RECT dest_rect{0, 0, output_width(), output_height()};
        RECT output_rect{0, 0, encoded_width(), encoded_height()};
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        video_context->VideoProcessorSetStreamFrameFormat(video_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamSourceRect(video_processor.Get(), 0, TRUE, &source_rect);
        video_context->VideoProcessorSetStreamDestRect(video_processor.Get(), 0, TRUE, &dest_rect);
        video_context->VideoProcessorSetOutputTargetRect(video_processor.Get(), TRUE, &output_rect);
        video_context->VideoProcessorSetOutputBackgroundColor(video_processor.Get(), FALSE, &background);

        initialized = true;
        return S_OK;
    }

    HRESULT ensure_source_copy_texture(const D3D11_TEXTURE2D_DESC& source_desc, DXGI_FORMAT source_format)
    {
        if (source_copy_texture)
        {
            D3D11_TEXTURE2D_DESC existing{};
            source_copy_texture->GetDesc(&existing);
            constexpr UINT required_bind_flags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (existing.Width == source_desc.Width &&
                existing.Height == source_desc.Height &&
                existing.Format == source_format &&
                (existing.BindFlags & required_bind_flags) == required_bind_flags &&
                source_copy_input_view)
            {
                return S_OK;
            }

            source_copy_input_view.Reset();
            source_copy_texture.Reset();
            source_copy_ready = false;
        }

        D3D11_TEXTURE2D_DESC copy_desc = source_desc;
        copy_desc.Format = source_format;
        copy_desc.MipLevels = 1;
        copy_desc.ArraySize = 1;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        copy_desc.CPUAccessFlags = 0;
        copy_desc.MiscFlags = 0;

        HRESULT hr = device->CreateTexture2D(&copy_desc, nullptr, &source_copy_texture);
        if (FAILED(hr))
            return hr;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc{};
        input_view_desc.FourCC = 0;
        input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_view_desc.Texture2D.MipSlice = 0;
        input_view_desc.Texture2D.ArraySlice = 0;

        hr = video_device->CreateVideoProcessorInputView(
            source_copy_texture.Get(),
            video_processor_enum.Get(),
            &input_view_desc,
            &source_copy_input_view);
        if (FAILED(hr))
        {
            source_copy_texture.Reset();
            source_copy_ready = false;
            return hr;
        }

        return S_OK;
    }

    HRESULT ensure_shared_source_texture(HANDLE shared_handle, DXGI_FORMAT source_format, D3D11_TEXTURE2D_DESC& source_desc)
    {
        if (shared_handle == nullptr)
            return E_POINTER;

        if (shared_handle != cached_shared_handle || !cached_source_texture)
        {
            cached_keyed_mutex.Reset();
            cached_source_texture.Reset();
            cached_shared_handle = nullptr;
            source_copy_ready = false;

            HRESULT legacy_hr = device->OpenSharedResource(
                shared_handle,
                __uuidof(ID3D11Texture2D),
                &cached_source_texture);
            HRESULT nt_hr = E_NOINTERFACE;
            if (FAILED(legacy_hr))
            {
                ComPtr<ID3D11Device1> device1;
                nt_hr = device.As(&device1);
                if (SUCCEEDED(nt_hr))
                {
                    nt_hr = device1->OpenSharedResource1(
                        shared_handle,
                        __uuidof(ID3D11Texture2D),
                        &cached_source_texture);
                }
            }
            if (!cached_source_texture)
            {
                set_last_error("NativeRecorder failed to open shared texture on the source adapter: legacy=" +
                    hresult_to_string(legacy_hr) + ", nt=" + hresult_to_string(nt_hr));
                return FAILED(nt_hr) ? nt_hr : legacy_hr;
            }

            cached_shared_handle = shared_handle;
            HRESULT hr = cached_source_texture.As(&cached_keyed_mutex);
            if (FAILED(hr) || !cached_keyed_mutex)
            {
                set_last_error("NativeRecorder opened the shared texture but IDXGIKeyedMutex was unavailable: " +
                    hresult_to_string(hr));
                return FAILED(hr) ? hr : E_NOINTERFACE;
            }
        }

        cached_source_texture->GetDesc(&source_desc);
        if (source_desc.Width != static_cast<UINT>(source_width()) ||
            source_desc.Height != static_cast<UINT>(source_height()))
        {
            set_last_error("NativeRecorder source texture size changed; expected=" +
                std::to_string(source_width()) + "x" + std::to_string(source_height()) +
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
            return E_INVALIDARG;
        }

        bool source_format_compatible = source_desc.Format == source_format ||
            (source_desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                (source_format == DXGI_FORMAT_R8G8B8A8_UNORM || source_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) ||
            (source_desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
                (source_format == DXGI_FORMAT_B8G8R8A8_UNORM || source_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
        if (!source_format_compatible)
        {
            set_last_error("NativeRecorder shared resource format is not copy-compatible with the submitted format; resource=" +
                dxgi_format_to_string(source_desc.Format) + ", submitted=" + dxgi_format_to_string(source_format));
            return E_INVALIDARG;
        }

        HRESULT hr = ensure_source_copy_texture(source_desc, source_format);
        if (FAILED(hr))
            return fail_step("CreateTexture2D(source copy)", hr, "source=" + texture_desc_to_string(source_desc));

        return S_OK;
    }

    HRESULT ensure_output_view(ID3D11Texture2D* output_texture, ID3D11VideoProcessorOutputView** output_view)
    {
        if (output_texture == nullptr || output_view == nullptr)
            return E_POINTER;

        *output_view = nullptr;
        for (auto& entry : output_view_cache)
        {
            if (entry.texture == output_texture && entry.view)
            {
                *output_view = entry.view.Get();
                return S_OK;
            }
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
        output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output_view_desc.Texture2D.MipSlice = 0;

        OutputViewCacheEntry entry{};
        entry.texture = output_texture;
        HRESULT hr = video_device->CreateVideoProcessorOutputView(
            output_texture,
            video_processor_enum.Get(),
            &output_view_desc,
            &entry.view);
        if (FAILED(hr))
            return hr;

        output_view_cache.push_back(std::move(entry));
        *output_view = output_view_cache.back().view.Get();
        return S_OK;
    }

    HRESULT create_nv12_texture(ComPtr<ID3D11Texture2D>& nv12_texture)
    {
        D3D11_TEXTURE2D_DESC output_desc{};
        output_desc.Width = static_cast<UINT>(encoded_width());
        output_desc.Height = static_cast<UINT>(encoded_height());
        output_desc.MipLevels = 1;
        output_desc.ArraySize = 1;
        output_desc.Format = DXGI_FORMAT_NV12;
        output_desc.SampleDesc.Count = 1;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&output_desc, nullptr, &nv12_texture);
        if (FAILED(hr))
            return fail_step("CreateTexture2D(NV12 output)", hr, "output=" + texture_desc_to_string(output_desc));

        return S_OK;
    }

    HRESULT convert_shared_texture_to(HANDLE shared_handle, DXGI_FORMAT source_format, ID3D11Texture2D* output_texture)
    {
        if (!initialized)
            return E_UNEXPECTED;
        if (output_texture == nullptr)
            return E_POINTER;

        D3D11_TEXTURE2D_DESC source_desc{};
        HRESULT hr = ensure_shared_source_texture(shared_handle, source_format, source_desc);
        if (FAILED(hr))
            return hr;

        bool copy_shared_source = true;
        if (cached_keyed_mutex)
        {
            hr = cached_keyed_mutex->AcquireSync(kEncoderReadKey, kSharedTextureAcquireTimeoutMs);
            if (hr == WAIT_TIMEOUT || hr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                ++keyed_acquire_timeouts;
                if (!source_copy_ready)
                {
                    set_last_error("NativeRecorder shared texture was not ready; dropping one frame.");
                    return DXGI_ERROR_WAS_STILL_DRAWING;
                }

                // CFR output can intentionally sample the same source frame more than once.
                // Reuse the private synchronized copy until the producer publishes key 1 again.
                copy_shared_source = false;
                ++reused_private_copies;
            }
            else if (FAILED(hr))
                return fail_step("IDXGIKeyedMutex::AcquireSync", hr, "source=" + texture_desc_to_string(source_desc));
            else
                ++keyed_acquire_successes;
        }

        if (copy_shared_source)
        {
            device_context->CopyResource(source_copy_texture.Get(), cached_source_texture.Get());
            ++fresh_shared_copies;
            if (cached_keyed_mutex)
            {
                HRESULT release_hr = cached_keyed_mutex->ReleaseSync(kGameWriteKey);
                if (FAILED(release_hr))
                    return fail_step("IDXGIKeyedMutex::ReleaseSync", release_hr, "source=" + texture_desc_to_string(source_desc));
                ++keyed_release_successes;
            }

            source_copy_ready = true;
        }

        if constexpr (kEnableTextureContentProbes)
        {
            std::lock_guard lock(content_probe_mutex);
            source_content_probe.tick(device.Get(), device_context.Get(), source_copy_texture.Get(), frame_index);
        }

        ID3D11VideoProcessorOutputView* output_view = nullptr;
        hr = ensure_output_view(output_texture, &output_view);
        if (FAILED(hr))
            return fail_step("CreateVideoProcessorOutputView", hr);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = source_copy_input_view.Get();

        hr = video_context->VideoProcessorBlt(
            video_processor.Get(),
            output_view,
            static_cast<UINT>(frame_index),
            1,
            &stream);
        if (FAILED(hr))
            return fail_step("VideoProcessorBlt", hr, "source=" + texture_desc_to_string(source_desc));

        if constexpr (kEnableTextureContentProbes)
        {
            std::lock_guard lock(content_probe_mutex);
            nv12_content_probe.tick(device.Get(), device_context.Get(), output_texture, frame_index);
            update_color_probe_match();
        }

        ++frame_index;
        return S_OK;
    }

    std::string synchronization_diagnostics() const
    {
        const uint64_t acquire_successes = keyed_acquire_successes.load(std::memory_order_relaxed);
        const uint64_t acquire_timeouts = keyed_acquire_timeouts.load(std::memory_order_relaxed);
        const uint64_t release_successes = keyed_release_successes.load(std::memory_order_relaxed);
        const uint64_t shared_copies = fresh_shared_copies.load(std::memory_order_relaxed);
        const uint64_t private_copies = reused_private_copies.load(std::memory_order_relaxed);
        const bool balanced = acquire_successes == release_successes &&
            acquire_successes == shared_copies;
        return "keyedHealth=" + std::string(balanced ? "balanced" : "mismatch") +
            ",acquireSuccess=" + std::to_string(acquire_successes) +
            ",acquireTimeout=" + std::to_string(acquire_timeouts) +
            ",releaseSuccess=" + std::to_string(release_successes) +
            ",freshCopies=" + std::to_string(shared_copies) +
            ",privateReuses=" + std::to_string(private_copies);
    }

    std::string content_diagnostics() const
    {
        std::lock_guard lock(content_probe_mutex);
        return source_content_probe.summary("sourceBgra") + ", " +
            nv12_content_probe.summary("encoderNv12") + ", " +
            color_conversion_diagnostics();
    }

    std::string color_conversion_diagnostics() const
    {
        if (!has_matched_color_probe)
        {
            return "colorTransformHealth=no-matched-probe,matchedProbes=0";
        }

        return "colorTransformHealth=" + std::string(mismatched_color_probes == 0 ? "plausible" : "mismatch-observed") +
            ",matchedProbes=" + std::to_string(matched_color_probes) +
            ",mismatchedProbes=" + std::to_string(mismatched_color_probes) +
            ",lastProbePlausible=" + std::string(last_color_probe_plausible ? "true" : "false") +
            ",matchedSourceHashChanges=" + std::to_string(matched_source_hash_changes) +
            ",matchedNv12HashChanges=" + std::to_string(matched_nv12_hash_changes) +
            ",probeFrame=" + std::to_string(last_matched_color_frame) +
            ",expectedYuv=" + std::to_string(last_expected_y) + "/" +
                std::to_string(last_expected_u) + "/" + std::to_string(last_expected_v) +
            ",actualYuv=" + std::to_string(last_actual_y) + "/" +
                std::to_string(last_actual_u) + "/" + std::to_string(last_actual_v) +
            ",deltaYuv=" + std::to_string(last_delta_y) + "/" +
                std::to_string(last_delta_u) + "/" + std::to_string(last_delta_v);
    }

    bool color_conversion_plausible() const
    {
        return has_matched_color_probe && mismatched_color_probes == 0;
    }

    void update_color_probe_match()
    {
        if (source_content_probe.completed == 0 || nv12_content_probe.completed == 0 ||
            source_content_probe.last_frame_index != nv12_content_probe.last_frame_index)
        {
            return;
        }

        const uint64_t probe_frame = source_content_probe.last_frame_index;
        if (has_matched_color_probe && probe_frame == last_matched_color_frame)
            return;

        const int r = static_cast<int>(source_content_probe.last_avg0);
        const int g = static_cast<int>(source_content_probe.last_avg1);
        const int b = static_cast<int>(source_content_probe.last_avg2);
        const int expected_y = std::clamp(16 + ((47 * r + 157 * g + 16 * b) >> 8), 0, 255);
        const int expected_u = std::clamp(128 + ((-26 * r - 87 * g + 112 * b) >> 8), 0, 255);
        const int expected_v = std::clamp(128 + ((112 * r - 102 * g - 10 * b) >> 8), 0, 255);
        const int actual_y = static_cast<int>(nv12_content_probe.last_avg0);
        const int actual_u = static_cast<int>(nv12_content_probe.last_avg1);
        const int actual_v = static_cast<int>(nv12_content_probe.last_avg2);
        const int delta_y = std::abs(actual_y - expected_y);
        const int delta_u = std::abs(actual_u - expected_u);
        const int delta_v = std::abs(actual_v - expected_v);
        const bool plausible = delta_y <= 24 && delta_u <= 40 && delta_v <= 40;

        if (has_matched_color_probe)
        {
            if (source_content_probe.last_hash != last_matched_source_hash)
                ++matched_source_hash_changes;
            if (nv12_content_probe.last_hash != last_matched_nv12_hash)
                ++matched_nv12_hash_changes;
        }

        ++matched_color_probes;
        if (!plausible)
            ++mismatched_color_probes;
        last_matched_color_frame = probe_frame;
        last_matched_source_hash = source_content_probe.last_hash;
        last_matched_nv12_hash = nv12_content_probe.last_hash;
        last_expected_y = expected_y;
        last_expected_u = expected_u;
        last_expected_v = expected_v;
        last_actual_y = actual_y;
        last_actual_u = actual_u;
        last_actual_v = actual_v;
        last_delta_y = delta_y;
        last_delta_u = delta_u;
        last_delta_v = delta_v;
        last_color_probe_plausible = plausible;
        has_matched_color_probe = true;
    }

    void reset()
    {
        output_view_cache.clear();
        source_copy_input_view.Reset();
        source_copy_texture.Reset();
        source_copy_ready = false;
        cached_keyed_mutex.Reset();
        cached_source_texture.Reset();
        cached_shared_handle = nullptr;
        video_processor.Reset();
        video_processor_enum.Reset();
        video_context.Reset();
        video_device.Reset();
        device_context.Reset();
        device.Reset();
        source_content_probe.reset();
        nv12_content_probe.reset();
        matched_color_probes = 0;
        mismatched_color_probes = 0;
        matched_source_hash_changes = 0;
        matched_nv12_hash_changes = 0;
        last_matched_color_frame = 0;
        last_matched_source_hash = 0;
        last_matched_nv12_hash = 0;
        last_expected_y = 0;
        last_expected_u = 0;
        last_expected_v = 0;
        last_actual_y = 0;
        last_actual_u = 0;
        last_actual_v = 0;
        last_delta_y = 0;
        last_delta_u = 0;
        last_delta_v = 0;
        has_matched_color_probe = false;
        last_color_probe_plausible = false;
        initialized = false;
        frame_index = 0;
        keyed_acquire_successes = 0;
        keyed_acquire_timeouts = 0;
        keyed_release_successes = 0;
        fresh_shared_copies = 0;
        reused_private_copies = 0;
    }
};

struct LibavPacketHolder
{
    AVPacket* packet = nullptr;

    LibavPacketHolder()
        : packet(av_packet_alloc())
    {
    }

    ~LibavPacketHolder()
    {
        av_packet_free(&packet);
    }

    AVPacket* get() const { return packet; }
};

