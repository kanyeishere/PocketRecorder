struct NvencLibavRecorderBackend final : NativeRecorderBackend
{
    pr_video_config video{};
    pr_audio_config audio{};
    std::wstring output_path;
    SharedTextureNv12Converter converter;
    std::unique_ptr<NvEncoderD3D11> encoder;
    AsyncLibavMp4Muxer muxer;
    bool initialized = false;
    bool stopped = false;
    uint64_t submitted_frames = 0;
    uint64_t written_packets = 0;
    uint64_t audio_packets = 0;
    int64_t video_sample_duration_hns = 0;
    std::deque<int64_t> pending_video_timestamps_hns;
    struct NvencInputSlot
    {
        ComPtr<ID3D11Texture2D> texture;
        bool in_use = false;
    };
    struct PendingVideoFrame
    {
        size_t slot_index = 0;
        int64_t timestamp_hns = 0;
        bool force_idr = false;
    };
    std::vector<NvencInputSlot> nv12_pool;
    size_t next_nv12_slot = 0;
    std::mutex input_mutex;
    std::mutex d3d_context_mutex;
    std::mutex video_queue_mutex;
    std::condition_variable video_queue_cv;
    std::deque<PendingVideoFrame> video_queue;
    std::thread video_worker;
    std::atomic<HRESULT> video_worker_result{S_OK};
    bool accepting_video = false;
    bool stopping_video = false;

    NvencLibavRecorderBackend(const pr_video_config& video_config, const pr_audio_config& audio_config, std::wstring output)
        : video(video_config), audio(audio_config), output_path(std::move(output))
    {
        converter.video = video;
    }

    ~NvencLibavRecorderBackend() override
    {
        stop();
    }

    const char* backend_name() const override
    {
        return "NvEncoderD3D11+libavformat";
    }

    HRESULT initialize(ID3D11Device* source_device, DXGI_FORMAT source_format) override
    {
        if (initialized)
            return S_OK;
        if (source_device == nullptr)
            return E_POINTER;
        if (output_path.empty())
            return E_INVALIDARG;
        if (video.width <= 0 || video.height <= 0 || video.fps <= 0)
            return E_INVALIDARG;
        if (!is_supported_recording_codec(video.codec))
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (!nvenc_runtime_present())
        {
            set_last_error("NVIDIA NVENC runtime nvEncodeAPI64.dll was not found.");
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        HRESULT hr = converter.initialize(source_device, source_format);
        if (FAILED(hr))
            return hr;

        const int encoded_width = converter.encoded_width();
        const int encoded_height = converter.encoded_height();
        video_sample_duration_hns = static_cast<int64_t>(make_sample_duration_hns(video.fps));

        try
        {
            encoder = std::make_unique<NvEncoderD3D11>(
                converter.device.Get(),
                static_cast<uint32_t>(encoded_width),
                static_cast<uint32_t>(encoded_height),
                NV_ENC_BUFFER_FORMAT_NV12,
                3);

            NV_ENC_INITIALIZE_PARAMS initialize_params = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encode_config = { NV_ENC_CONFIG_VER };
            initialize_params.encodeConfig = &encode_config;
            encoder->CreateDefaultEncoderParams(
                &initialize_params,
                nvenc_codec_guid(video.codec),
                nvenc_preset_guid(),
                NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

            initialize_params.frameRateNum = static_cast<uint32_t>(std::max(1, video.fps));
            initialize_params.frameRateDen = 1;
            initialize_params.encodeWidth = static_cast<uint32_t>(encoded_width);
            initialize_params.encodeHeight = static_cast<uint32_t>(encoded_height);
            initialize_params.darWidth = static_cast<uint32_t>(encoded_width);
            initialize_params.darHeight = static_cast<uint32_t>(encoded_height);
            initialize_params.enablePTD = 1;
#if defined(_WIN32)
            initialize_params.enableEncodeAsync =
                encoder->GetCapabilityValue(nvenc_codec_guid(video.codec), NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT) ? 1 : 0;
#endif

            auto* cfg = initialize_params.encodeConfig;
            cfg->gopLength = static_cast<uint32_t>(std::max(1, video.fps) * 2);
            cfg->frameIntervalP = 1;
            cfg->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            cfg->rcParams.averageBitRate = static_cast<uint32_t>(video.bitrate_bps > 0 ? video.bitrate_bps : 12'000'000);
            cfg->rcParams.maxBitRate = cfg->rcParams.averageBitRate;
            cfg->rcParams.vbvBufferSize = std::max<uint32_t>(
                1,
                (cfg->rcParams.averageBitRate / std::max(1, video.fps)) * 3);
            cfg->rcParams.vbvInitialDelay = cfg->rcParams.vbvBufferSize;
            cfg->rcParams.zeroReorderDelay = 1;
            cfg->rcParams.lowDelayKeyFrameScale = 1;
            cfg->rcParams.enableLookahead = 0;
            cfg->rcParams.lookaheadDepth = 0;

            if (video.codec == PR_CODEC_H264)
            {
                cfg->encodeCodecConfig.h264Config.idrPeriod = cfg->gopLength;
                cfg->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
                cfg->encodeCodecConfig.h264Config.outputAUD = 0;
                cfg->encodeCodecConfig.h264Config.maxNumRefFrames = 1;
                cfg->encodeCodecConfig.h264Config.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
            }
            else
            {
                cfg->encodeCodecConfig.hevcConfig.idrPeriod = cfg->gopLength;
                cfg->encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
                cfg->encodeCodecConfig.hevcConfig.outputAUD = 0;
                cfg->encodeCodecConfig.hevcConfig.maxNumRefFramesInDPB = 1;
                cfg->encodeCodecConfig.hevcConfig.numRefL0 = NV_ENC_NUM_REF_FRAMES_1;
            }

            encoder->CreateEncoder(&initialize_params);

            std::vector<uint8_t> sequence_params;
            encoder->GetSequenceParams(sequence_params);
            hr = muxer.open(output_path, video, audio, sequence_params);
            if (FAILED(hr))
                return hr;

            hr = ensure_nv12_pool();
            if (FAILED(hr))
                return hr;
        }
        catch (const std::exception& ex)
        {
            return fail_exception("NvEncoderD3D11 initialize", ex,
                "codec=" + std::string(codec_name(video.codec)) +
                ", encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height));
        }

        video_worker_result.store(S_OK);
        accepting_video = true;
        stopping_video = false;
        video_worker = std::thread([this] { video_worker_loop(); });

        initialized = true;
        std::string message = "NativeRecorder initialized: source NVIDIA adapter=" + converter.adapter_name +
            ", luid=" + std::to_string(static_cast<uint32_t>(converter.adapter_luid.HighPart)) + ":" +
            std::to_string(converter.adapter_luid.LowPart) +
            ", sourceFormat=" + dxgi_format_to_string(source_format) +
            ", encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height) +
            ", pad=" + std::to_string(encoded_width - video.width) + "x" + std::to_string(encoded_height - video.height) +
            ", nvencBuffers=" + std::to_string(encoder ? encoder->GetEncoderBufferCount() : 0) +
            ", vpSourceSupport=" + hex_uint32(converter.source_format_support) +
            ", vpNv12Support=" + hex_uint32(converter.nv12_format_support) +
            ", output=" + std::string(codec_name(video.codec)) + "/MP4 via NvEncoderD3D11 + libavformat.";
        set_last_error(message);
        return S_OK;
    }

    HRESULT submit_shared_texture(ID3D11Device* source_device, HANDLE shared_handle, DXGI_FORMAT source_format, int64_t timestamp_hns) override
    {
        if (stopped)
            return E_ABORT;
        if (source_device == nullptr || shared_handle == nullptr)
            return E_POINTER;

        HRESULT hr = initialize(source_device, source_format);
        if (FAILED(hr))
            return hr;

        hr = video_worker_result.load();
        if (FAILED(hr))
            return hr;

        size_t slot_index = 0;
        ID3D11Texture2D* nv12_texture = nullptr;
        hr = acquire_nv12_slot(slot_index, &nv12_texture);
        if (FAILED(hr))
            return hr;

        {
            std::lock_guard context_lock(d3d_context_mutex);
            hr = converter.convert_shared_texture_to(shared_handle, source_format, nv12_texture);
        }
        if (FAILED(hr))
        {
            release_nv12_slot(slot_index);
            return hr;
        }

        const bool force_idr = submitted_frames == 0;
        hr = enqueue_video_frame(slot_index, std::max<int64_t>(0, timestamp_hns), force_idr);
        if (FAILED(hr))
        {
            release_nv12_slot(slot_index);
            return hr;
        }

        ++submitted_frames;
        return S_OK;
    }

    HRESULT ensure_nv12_pool()
    {
        std::lock_guard lock(input_mutex);
        if (!nv12_pool.empty())
            return S_OK;

        nv12_pool.reserve(kNvencNv12PoolSize);
        for (size_t i = 0; i < kNvencNv12PoolSize; ++i)
        {
            NvencInputSlot slot{};
            HRESULT hr = converter.create_nv12_texture(slot.texture);
            if (FAILED(hr))
                return hr;
            nv12_pool.push_back(std::move(slot));
        }

        next_nv12_slot = 0;
        return S_OK;
    }

    HRESULT acquire_nv12_slot(size_t& slot_index, ID3D11Texture2D** texture)
    {
        if (texture == nullptr)
            return E_POINTER;

        std::lock_guard lock(input_mutex);
        for (size_t offset = 0; offset < nv12_pool.size(); ++offset)
        {
            size_t index = (next_nv12_slot + offset) % nv12_pool.size();
            if (nv12_pool[index].in_use)
                continue;

            nv12_pool[index].in_use = true;
            next_nv12_slot = (index + 1) % nv12_pool.size();
            slot_index = index;
            *texture = nv12_pool[index].texture.Get();
            return S_OK;
        }

        set_last_error("NativeRecorder NVENC NV12 input pool is full; dropping one frame.");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    void release_nv12_slot(size_t slot_index)
    {
        std::lock_guard lock(input_mutex);
        if (slot_index < nv12_pool.size())
            nv12_pool[slot_index].in_use = false;
    }

    HRESULT enqueue_video_frame(size_t slot_index, int64_t timestamp_hns, bool force_idr)
    {
        std::unique_lock lock(video_queue_mutex);
        if (!accepting_video)
            return E_ABORT;
        if (video_queue.size() >= kMaxNativeVideoQueueItems)
        {
            set_last_error("NativeRecorder NVENC input queue is full; dropping one frame.");
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }

        video_queue.push_back(PendingVideoFrame{slot_index, timestamp_hns, force_idr});
        lock.unlock();
        video_queue_cv.notify_one();
        return S_OK;
    }

    void video_worker_loop()
    {
        for (;;)
        {
            PendingVideoFrame frame{};
            {
                std::unique_lock lock(video_queue_mutex);
                video_queue_cv.wait(lock, [this] { return stopping_video || !video_queue.empty(); });
                if (video_queue.empty())
                {
                    if (stopping_video)
                        return;
                    continue;
                }

                frame = video_queue.front();
                video_queue.pop_front();
            }

            HRESULT hr = encode_queued_frame(frame);
            if (FAILED(hr))
            {
                video_worker_result.store(hr);
                std::vector<size_t> abandoned_slots;
                {
                    std::lock_guard lock(video_queue_mutex);
                    accepting_video = false;
                    stopping_video = true;
                    while (!video_queue.empty())
                    {
                        abandoned_slots.push_back(video_queue.front().slot_index);
                        video_queue.pop_front();
                    }
                }
                for (size_t slot : abandoned_slots)
                    release_nv12_slot(slot);
                video_queue_cv.notify_one();
                return;
            }
        }
    }

    HRESULT encode_queued_frame(const PendingVideoFrame& frame)
    {
        try
        {
            const NvEncInputFrame* input_frame = encoder->GetNextInputFrame();
            auto* input_texture = reinterpret_cast<ID3D11Texture2D*>(input_frame->inputPtr);
            if (input_texture == nullptr)
            {
                release_nv12_slot(frame.slot_index);
                return E_POINTER;
            }

            ComPtr<ID3D11Texture2D> nv12_texture;
            bool invalid_slot = false;
            {
                std::lock_guard lock(input_mutex);
                if (frame.slot_index >= nv12_pool.size() || !nv12_pool[frame.slot_index].texture)
                    invalid_slot = true;
                else
                    nv12_texture = nv12_pool[frame.slot_index].texture;
            }
            if (invalid_slot)
            {
                release_nv12_slot(frame.slot_index);
                return E_INVALIDARG;
            }

            {
                std::lock_guard context_lock(d3d_context_mutex);
                converter.device_context->CopyResource(
                    reinterpret_cast<ID3D11Resource*>(input_texture),
                    reinterpret_cast<ID3D11Resource*>(nv12_texture.Get()));
            }
            release_nv12_slot(frame.slot_index);

            NV_ENC_PIC_PARAMS picture_params = { NV_ENC_PIC_PARAMS_VER };
            picture_params.inputTimeStamp = static_cast<uint64_t>(std::max<int64_t>(0, frame.timestamp_hns));
            if (frame.force_idr)
                picture_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

            std::vector<NvEncOutputFrame> packets;
            pending_video_timestamps_hns.push_back(std::max<int64_t>(0, frame.timestamp_hns));
            encoder->EncodeFrame(packets, &picture_params);
            for (const NvEncOutputFrame& packet : packets)
            {
                if (packet.frame.empty())
                    continue;

                int64_t packet_timestamp = take_output_timestamp(static_cast<int64_t>(packet.timeStamp));
                HRESULT hr = muxer.enqueue_video_packet(
                    packet.frame,
                    nvenc_output_is_key_frame(packet.pictureType),
                    packet_timestamp,
                    video_sample_duration_hns);
                if (FAILED(hr))
                    return hr;
                ++written_packets;
            }
        }
        catch (const std::exception& ex)
        {
            return fail_exception("NvEncoderD3D11 worker encode", ex,
                "submitted=" + std::to_string(submitted_frames) +
                ", written=" + std::to_string(written_packets));
        }

        return S_OK;
    }

    int64_t take_output_timestamp(int64_t encoder_timestamp_hns)
    {
        if (!pending_video_timestamps_hns.empty())
        {
            int64_t timestamp = pending_video_timestamps_hns.front();
            pending_video_timestamps_hns.pop_front();
            return timestamp;
        }

        if (encoder_timestamp_hns >= 0)
            return encoder_timestamp_hns;

        return static_cast<int64_t>(written_packets) * video_sample_duration_hns;
    }

    HRESULT submit_audio(const void* data, int32_t byte_count, int64_t timestamp_hns) override
    {
        if (!audio.enabled)
            return S_OK;
        if (!initialized)
            return S_OK;

        HRESULT hr = muxer.enqueue_audio(data, byte_count, timestamp_hns);
        if (FAILED(hr))
            return hr;

        ++audio_packets;
        return S_OK;
    }

    HRESULT stop_video_worker()
    {
        {
            std::lock_guard lock(video_queue_mutex);
            accepting_video = false;
            stopping_video = true;
        }
        video_queue_cv.notify_one();

        if (video_worker.joinable())
            video_worker.join();

        return video_worker_result.load();
    }

    HRESULT stop() override
    {
        if (stopped)
            return S_OK;

        stopped = true;
        HRESULT result = stop_video_worker();
        if (encoder)
        {
            try
            {
                std::vector<NvEncOutputFrame> packets;
                encoder->EndEncode(packets);
                for (const NvEncOutputFrame& packet : packets)
                {
                    if (packet.frame.empty())
                        continue;

                    int64_t packet_timestamp = take_output_timestamp(static_cast<int64_t>(packet.timeStamp));
                    HRESULT hr = muxer.enqueue_video_packet(
                        packet.frame,
                        nvenc_output_is_key_frame(packet.pictureType),
                        packet_timestamp,
                        video_sample_duration_hns);
                    if (FAILED(hr) && SUCCEEDED(result))
                        result = hr;
                    ++written_packets;
                }
                encoder->DestroyEncoder();
            }
            catch (const std::exception& ex)
            {
                if (SUCCEEDED(result))
                    result = fail_exception("NvEncoderD3D11 finalize", ex);
            }
        }

        HRESULT mux_hr = muxer.close();
        if (FAILED(mux_hr) && SUCCEEDED(result))
            result = mux_hr;

        encoder.reset();
        converter.reset();
        {
            std::lock_guard lock(input_mutex);
            nv12_pool.clear();
            next_nv12_slot = 0;
        }

        if (SUCCEEDED(result))
        {
            set_last_error("NativeRecorder finalized via NvEncoderD3D11 + libavformat. submitted=" +
                std::to_string(submitted_frames) +
                ", packets=" + std::to_string(written_packets) +
                ", audioPackets=" + std::to_string(audio_packets));
        }

        return result;
    }
};

struct AmfLibavRecorderBackend final : NativeRecorderBackend
{
    pr_video_config video{};
    pr_audio_config audio{};
    std::wstring output_path;
    SharedTextureNv12Converter converter;
    amf::AMFContextPtr context;
    amf::AMFComponentPtr encoder;
    AsyncLibavMp4Muxer muxer;
    bool initialized = false;
    bool stopped = false;
    bool factory_initialized = false;
    uint64_t submitted_frames = 0;
    uint64_t written_packets = 0;
    uint64_t audio_packets = 0;
    int64_t video_sample_duration_hns = 0;
    std::deque<int64_t> pending_video_timestamps_hns;
    struct AmfInputSlot
    {
        ComPtr<ID3D11Texture2D> texture;
        bool in_use = false;
    };
    std::vector<AmfInputSlot> nv12_pool;
    std::deque<size_t> pending_video_slots;
    size_t next_nv12_slot = 0;
    struct PendingVideoFrame
    {
        size_t slot_index = 0;
        int64_t timestamp_hns = 0;
        bool force_idr = false;
    };
    std::mutex input_mutex;
    std::mutex d3d_context_mutex;
    std::mutex video_queue_mutex;
    std::condition_variable video_queue_cv;
    std::deque<PendingVideoFrame> video_queue;
    std::thread video_worker;
    std::atomic<HRESULT> video_worker_result{S_OK};
    bool accepting_video = false;
    bool stopping_video = false;

    AmfLibavRecorderBackend(const pr_video_config& video_config, const pr_audio_config& audio_config, std::wstring output)
        : video(video_config), audio(audio_config), output_path(std::move(output))
    {
        converter.video = video;
        converter.required_vendor_id = kAmdVendorId;
        converter.required_vendor_name = "AMD";
    }

    ~AmfLibavRecorderBackend() override
    {
        stop();
        if (factory_initialized)
        {
            g_AMFFactory.Terminate();
            factory_initialized = false;
        }
    }

    const char* backend_name() const override
    {
        return "AMF+libavformat";
    }

    HRESULT initialize(ID3D11Device* source_device, DXGI_FORMAT source_format) override
    {
        if (initialized)
            return S_OK;
        if (source_device == nullptr)
            return E_POINTER;
        if (output_path.empty())
            return E_INVALIDARG;
        if (video.width <= 0 || video.height <= 0 || video.fps <= 0)
            return E_INVALIDARG;
        if (!is_supported_recording_codec(video.codec))
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (!amf_runtime_present())
        {
            set_last_error("AMD AMF runtime amfrt64.dll was not found.");
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        HRESULT hr = converter.initialize(source_device, source_format);
        if (FAILED(hr))
            return hr;

        const int encoded_width = converter.encoded_width();
        const int encoded_height = converter.encoded_height();
        video_sample_duration_hns = static_cast<int64_t>(make_sample_duration_hns(video.fps));

        AMF_RESULT amf_result = g_AMFFactory.Init();
        if (!amf_result_success(amf_result))
            return fail_amf("AMF factory init", amf_result);
        factory_initialized = true;

        amf_result = g_AMFFactory.GetFactory()->CreateContext(&context);
        if (!amf_result_success(amf_result))
            return fail_amf("AMF CreateContext", amf_result);

        amf_result = context->InitDX11(converter.device.Get());
        if (!amf_result_success(amf_result))
            return fail_amf("AMF InitDX11", amf_result, "adapter=" + converter.adapter_name);

        amf_result = g_AMFFactory.GetFactory()->CreateComponent(context, amf_codec_id(video.codec), &encoder);
        if (!amf_result_success(amf_result))
            return fail_amf("AMF CreateComponent(encoder)", amf_result, std::string("codec=") + codec_name(video.codec));

        hr = configure_encoder(encoded_width, encoded_height);
        if (FAILED(hr))
            return hr;

        amf_result = encoder->Init(amf::AMF_SURFACE_NV12, encoded_width, encoded_height);
        if (!amf_result_success(amf_result))
            return fail_amf("AMF encoder Init(NV12)", amf_result,
                "encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height));

        std::vector<uint8_t> sequence_params;
        hr = read_sequence_params(sequence_params);
        if (FAILED(hr))
            return hr;

        hr = muxer.open(output_path, video, audio, sequence_params);
        if (FAILED(hr))
            return hr;

        hr = ensure_nv12_pool();
        if (FAILED(hr))
            return hr;

        initialized = true;
        video_worker_result.store(S_OK);
        accepting_video = true;
        stopping_video = false;
        video_worker = std::thread([this] { video_worker_loop(); });

        std::string message = "NativeRecorder initialized: source AMD adapter=" + converter.adapter_name +
            ", luid=" + std::to_string(static_cast<uint32_t>(converter.adapter_luid.HighPart)) + ":" +
            std::to_string(converter.adapter_luid.LowPart) +
            ", sourceFormat=" + dxgi_format_to_string(source_format) +
            ", encoded=" + std::to_string(encoded_width) + "x" + std::to_string(encoded_height) +
            ", pad=" + std::to_string(encoded_width - video.width) + "x" + std::to_string(encoded_height - video.height) +
            ", vpSourceSupport=" + hex_uint32(converter.source_format_support) +
            ", vpNv12Support=" + hex_uint32(converter.nv12_format_support) +
            ", output=" + std::string(codec_name(video.codec)) + "/MP4 via AMF + libavformat.";
        set_last_error(message);
        return S_OK;
    }

    HRESULT configure_encoder(int encoded_width, int encoded_height)
    {
        const int fps = std::max(1, video.fps);
        const int64_t bitrate = video.bitrate_bps > 0 ? video.bitrate_bps : 12'000'000;
        AMF_RESULT result = AMF_OK;

        if (video.codec == PR_CODEC_H264)
        {
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 Usage)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 Profile)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, amf::AMFVariant(AMFConstructSize(encoded_width, encoded_height)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 FrameSize)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, amf::AMFVariant(AMFConstructRate(fps, 1)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 FrameRate)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, amf::AMFVariant(static_cast<amf_int64>(bitrate)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 TargetBitrate)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, amf::AMFVariant(static_cast<amf_int64>(bitrate)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 PeakBitrate)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 RateControl)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 QualityPreset)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES, amf::AMFVariant(static_cast<amf_int64>(0)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 BFrames)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, amf::AMFVariant(static_cast<amf_int64>(fps * 2)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 IDRPeriod)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, amf::AMFVariant(static_cast<amf_int64>(fps * 2)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(H264 HeaderInsertion)", result);
        }
        else
        {
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC Usage)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC Profile)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, amf::AMFVariant(AMFConstructSize(encoded_width, encoded_height)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC FrameSize)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, amf::AMFVariant(AMFConstructRate(fps, 1)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC FrameRate)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, amf::AMFVariant(static_cast<amf_int64>(bitrate)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC TargetBitrate)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, amf::AMFVariant(static_cast<amf_int64>(bitrate)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC PeakBitrate)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC RateControl)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC QualityPreset)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, amf::AMFVariant(static_cast<amf_int64>(fps * 2)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC GOP)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, amf::AMFVariant(static_cast<amf_int64>(1)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC IDR)", result);
            result = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED)));
            if (!amf_result_success(result)) return fail_amf("AMF SetProperty(HEVC HeaderInsertion)", result);
        }

        return S_OK;
    }

    HRESULT read_sequence_params(std::vector<uint8_t>& sequence_params)
    {
        amf::AMFVariant extra_data;
        const wchar_t* property = video.codec == PR_CODEC_HEVC
            ? AMF_VIDEO_ENCODER_HEVC_EXTRADATA
            : AMF_VIDEO_ENCODER_EXTRADATA;
        AMF_RESULT result = encoder->GetProperty(property, &extra_data);
        if (!amf_result_success(result))
            return fail_amf("AMF GetProperty(extradata)", result, std::string("codec=") + codec_name(video.codec));

        amf::AMFInterface* extra_interface = AMFVariantGetInterface(&extra_data);
        if (extra_interface == nullptr)
        {
            set_last_error("NativeRecorder AMF encoder returned empty extradata.");
            return E_FAIL;
        }

        amf::AMFBufferPtr buffer(extra_interface);
        if (buffer == nullptr || buffer->GetNative() == nullptr || buffer->GetSize() == 0)
        {
            set_last_error("NativeRecorder AMF encoder returned invalid extradata buffer.");
            return E_FAIL;
        }

        const auto* data = static_cast<const uint8_t*>(buffer->GetNative());
        sequence_params.assign(data, data + buffer->GetSize());
        return S_OK;
    }

    HRESULT ensure_nv12_pool()
    {
        if (!nv12_pool.empty())
            return S_OK;

        nv12_pool.reserve(kAmfNv12PoolSize);
        for (size_t i = 0; i < kAmfNv12PoolSize; ++i)
        {
            AmfInputSlot slot{};
            HRESULT hr = converter.create_nv12_texture(slot.texture);
            if (FAILED(hr))
                return hr;
            nv12_pool.push_back(std::move(slot));
        }

        next_nv12_slot = 0;
        return S_OK;
    }

    HRESULT acquire_nv12_slot(size_t& slot_index, ID3D11Texture2D** texture)
    {
        if (texture == nullptr)
            return E_POINTER;

        HRESULT hr = ensure_nv12_pool();
        if (FAILED(hr))
            return hr;

        std::lock_guard lock(input_mutex);
        for (size_t offset = 0; offset < nv12_pool.size(); ++offset)
        {
            size_t index = (next_nv12_slot + offset) % nv12_pool.size();
            if (nv12_pool[index].in_use)
                continue;

            nv12_pool[index].in_use = true;
            next_nv12_slot = (index + 1) % nv12_pool.size();
            slot_index = index;
            *texture = nv12_pool[index].texture.Get();
            return S_OK;
        }

        set_last_error("NativeRecorder AMF NV12 texture pool is full; dropping one frame.");
        return DXGI_ERROR_WAS_STILL_DRAWING;
    }

    void release_nv12_slot(size_t slot_index)
    {
        std::lock_guard lock(input_mutex);
        if (slot_index < nv12_pool.size())
            nv12_pool[slot_index].in_use = false;
    }

    HRESULT submit_shared_texture(ID3D11Device* source_device, HANDLE shared_handle, DXGI_FORMAT source_format, int64_t timestamp_hns) override
    {
        if (stopped)
            return E_ABORT;
        if (source_device == nullptr || shared_handle == nullptr)
            return E_POINTER;

        HRESULT hr = initialize(source_device, source_format);
        if (FAILED(hr))
            return hr;

        hr = video_worker_result.load();
        if (FAILED(hr))
            return hr;

        size_t slot_index = 0;
        ID3D11Texture2D* nv12_texture = nullptr;
        hr = acquire_nv12_slot(slot_index, &nv12_texture);
        if (FAILED(hr))
            return hr;

        {
            std::lock_guard context_lock(d3d_context_mutex);
            hr = converter.convert_shared_texture_to(shared_handle, source_format, nv12_texture);
        }
        if (FAILED(hr))
        {
            release_nv12_slot(slot_index);
            return hr;
        }

        const bool force_idr = submitted_frames == 0;
        hr = enqueue_video_frame(slot_index, std::max<int64_t>(0, timestamp_hns), force_idr);
        if (FAILED(hr))
        {
            release_nv12_slot(slot_index);
            return hr;
        }

        ++submitted_frames;
        return S_OK;
    }

    HRESULT enqueue_video_frame(size_t slot_index, int64_t timestamp_hns, bool force_idr)
    {
        std::unique_lock lock(video_queue_mutex);
        if (!accepting_video)
            return E_ABORT;
        if (video_queue.size() >= kMaxNativeVideoQueueItems)
        {
            set_last_error("NativeRecorder AMF input queue is full; dropping one frame.");
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }

        video_queue.push_back(PendingVideoFrame{slot_index, timestamp_hns, force_idr});
        lock.unlock();
        video_queue_cv.notify_one();
        return S_OK;
    }

    void video_worker_loop()
    {
        for (;;)
        {
            PendingVideoFrame frame{};
            {
                std::unique_lock lock(video_queue_mutex);
                video_queue_cv.wait(lock, [this] { return stopping_video || !video_queue.empty(); });
                if (video_queue.empty())
                {
                    if (stopping_video)
                        return;
                    continue;
                }

                frame = video_queue.front();
                video_queue.pop_front();
            }

            HRESULT hr = submit_queued_frame(frame);
            if (SUCCEEDED(hr))
                hr = drain_output(false);

            if (FAILED(hr))
            {
                video_worker_result.store(hr);
                std::vector<size_t> abandoned_slots;
                {
                    std::lock_guard lock(video_queue_mutex);
                    accepting_video = false;
                    stopping_video = true;
                    while (!video_queue.empty())
                    {
                        abandoned_slots.push_back(video_queue.front().slot_index);
                        video_queue.pop_front();
                    }
                }
                for (size_t slot : abandoned_slots)
                    release_nv12_slot(slot);
                video_queue_cv.notify_one();
                return;
            }
        }
    }

    HRESULT submit_queued_frame(const PendingVideoFrame& frame)
    {
        ComPtr<ID3D11Texture2D> nv12_texture;
        bool invalid_slot = false;
        {
            std::lock_guard lock(input_mutex);
            if (frame.slot_index >= nv12_pool.size() || !nv12_pool[frame.slot_index].texture)
                invalid_slot = true;
            else
                nv12_texture = nv12_pool[frame.slot_index].texture;
        }
        if (invalid_slot)
        {
            release_nv12_slot(frame.slot_index);
            return E_INVALIDARG;
        }

        amf::AMFSurfacePtr surface;
        AMF_RESULT result = context->CreateSurfaceFromDX11Native(nv12_texture.Get(), &surface, nullptr);
        if (!amf_result_success(result))
        {
            release_nv12_slot(frame.slot_index);
            return fail_amf("AMF CreateSurfaceFromDX11Native", result);
        }

        surface->SetPts(static_cast<amf_pts>(std::max<int64_t>(0, frame.timestamp_hns)));
        surface->SetDuration(static_cast<amf_pts>(video_sample_duration_hns));

        if (frame.force_idr)
        {
            if (video.codec == PR_CODEC_H264)
            {
                surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR)));
                surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, amf::AMFVariant(true));
                surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, amf::AMFVariant(true));
            }
            else
            {
                surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, amf::AMFVariant(static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR)));
                surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, amf::AMFVariant(true));
            }
        }

        result = encoder->SubmitInput(surface);
        if (result == AMF_INPUT_FULL)
        {
            HRESULT hr = drain_output(false);
            if (FAILED(hr))
            {
                release_nv12_slot(frame.slot_index);
                return hr;
            }
            result = encoder->SubmitInput(surface);
        }
        if (result == AMF_INPUT_FULL)
        {
            release_nv12_slot(frame.slot_index);
            set_last_error("NativeRecorder AMF input queue is full; dropping one frame.");
            return S_OK;
        }
        if (result != AMF_OK && result != AMF_NEED_MORE_INPUT)
        {
            release_nv12_slot(frame.slot_index);
            return fail_amf("AMF SubmitInput", result);
        }

        pending_video_timestamps_hns.push_back(std::max<int64_t>(0, frame.timestamp_hns));
        pending_video_slots.push_back(frame.slot_index);
        return S_OK;
    }

    HRESULT drain_output(bool flushing)
    {
        if (!encoder)
            return S_OK;

        int repeat_count = 0;
        while (true)
        {
            amf::AMFDataPtr data;
            AMF_RESULT result = encoder->QueryOutput(&data);
            if (result == AMF_REPEAT)
            {
                if (!flushing || repeat_count++ >= 1000)
                    return S_OK;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (result == AMF_EOF)
                return S_OK;
            if (!amf_result_success(result))
                return fail_amf("AMF QueryOutput", result);
            if (data == nullptr)
            {
                if (!flushing || repeat_count++ >= 1000)
                    return S_OK;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            amf::AMFBufferPtr buffer(data);
            if (buffer == nullptr || buffer->GetNative() == nullptr || buffer->GetSize() == 0)
                continue;

            const auto* bytes = static_cast<const uint8_t*>(buffer->GetNative());
            std::vector<uint8_t> packet(bytes, bytes + buffer->GetSize());
            bool key_frame = false;
            if (video.codec == PR_CODEC_H264)
            {
                amf_int64 output_type = AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P;
                buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &output_type);
                key_frame = output_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR ||
                    output_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
            }
            else
            {
                amf_int64 output_type = AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_P;
                buffer->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &output_type);
                key_frame = output_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR ||
                    output_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I;
            }

            int64_t timestamp = take_output_timestamp(static_cast<int64_t>(data->GetPts()));

            HRESULT hr = muxer.enqueue_video_packet(packet, key_frame, timestamp, video_sample_duration_hns);
            if (FAILED(hr))
                return hr;
            ++written_packets;
        }
    }

    int64_t take_output_timestamp(int64_t encoder_timestamp_hns)
    {
        if (!pending_video_timestamps_hns.empty())
        {
            int64_t timestamp = pending_video_timestamps_hns.front();
            pending_video_timestamps_hns.pop_front();
            if (!pending_video_slots.empty())
            {
                size_t slot_index = pending_video_slots.front();
                pending_video_slots.pop_front();
                release_nv12_slot(slot_index);
            }
            return timestamp;
        }

        if (encoder_timestamp_hns >= 0)
            return encoder_timestamp_hns;

        return static_cast<int64_t>(written_packets) * video_sample_duration_hns;
    }

    HRESULT submit_audio(const void* data, int32_t byte_count, int64_t timestamp_hns) override
    {
        if (!audio.enabled)
            return S_OK;
        if (!initialized)
            return S_OK;

        HRESULT hr = muxer.enqueue_audio(data, byte_count, timestamp_hns);
        if (FAILED(hr))
            return hr;

        ++audio_packets;
        return S_OK;
    }

    HRESULT stop_video_worker()
    {
        {
            std::lock_guard lock(video_queue_mutex);
            accepting_video = false;
            stopping_video = true;
        }
        video_queue_cv.notify_one();

        if (video_worker.joinable())
            video_worker.join();

        return video_worker_result.load();
    }

    HRESULT stop() override
    {
        if (stopped)
            return S_OK;

        stopped = true;
        HRESULT result = stop_video_worker();
        if (encoder)
        {
            AMF_RESULT amf_result = encoder->Drain();
            if (!amf_result_success(amf_result) && amf_result != AMF_INPUT_FULL)
                result = fail_amf("AMF encoder Drain", amf_result);

            if (SUCCEEDED(result))
                result = drain_output(true);

            encoder->Terminate();
            encoder.Release();
        }

        HRESULT mux_hr = muxer.close();
        if (FAILED(mux_hr) && SUCCEEDED(result))
            result = mux_hr;

        context.Release();
        pending_video_timestamps_hns.clear();
        pending_video_slots.clear();
        {
            std::lock_guard lock(input_mutex);
            nv12_pool.clear();
            next_nv12_slot = 0;
        }
        converter.reset();

        if (SUCCEEDED(result))
        {
            set_last_error("NativeRecorder finalized via AMF + libavformat. submitted=" +
                std::to_string(submitted_frames) +
                ", packets=" + std::to_string(written_packets) +
                ", audioPackets=" + std::to_string(audio_packets));
        }

        return result;
    }
};
