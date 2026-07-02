/*
* Copyright 2017-2026 NVIDIA Corporation.  All rights reserved.
*
* Please refer to the NVIDIA end user license agreement (EULA) associated
* with this source code for terms and conditions that govern your use of
* this software. Any use, reproduction, disclosure, or distribution of
* this software and related documentation outside the terms of the EULA
* is strictly prohibited.
*
*/

/**
*  This sample application measures transcoding performance in FPS.
*  
*  Each transcoder instance spawns 4 threads:
*  - Decode thread: Decodes input frames
*  - CUDA processing thread: Copies frames to device memory
*  - Encode thread: Encodes frames
*  - Output thread: Counts processed frames (no file writing)
*
*  The application launches N transcoder instances for parallel processing
*  and measures aggregate throughput.
*/

#include "AppTransPerf.h"

simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();
void TransProc(CUcontext cuContext,
               NvDecoder* decoder,
               FFmpegDemuxer* demuxer,
               int* frameCount,
               NvEncoderInitParam* cliOptions,
               std::exception_ptr& vExceptionPtr)
{
    try {
        StopWatch w;
        w.Start();
        
        // Check if input is 422
        bool bInput422 = (demuxer->GetChromaFormat() == AV_PIX_FMT_YUV422P ||
                          demuxer->GetChromaFormat() == AV_PIX_FMT_YUV422P10LE ||
                          demuxer->GetChromaFormat() == AV_PIX_FMT_YUV422P12LE);

        // Check if input is 444
        bool bInput444 = (demuxer->GetChromaFormat() == AV_PIX_FMT_YUV444P ||
                          demuxer->GetChromaFormat() == AV_PIX_FMT_YUV444P10LE ||
                          demuxer->GetChromaFormat() == AV_PIX_FMT_YUV444P12LE);

        if (bInput422)
        {
            std::cout << "INFO: YUV422 is supported from Blackwell (GB20x) GPUs onwards." << std::endl;
            // Early reject for AV1 4:2:2 encode path
            if (cliOptions->IsCodecAV1())
            {
                std::cout << "ERROR: AV1 does not support 4:2:2 chroma format." << std::endl;
                return;
            }
        }
        else if (bInput444)
        {
            // Add warning for H.264 444 decode limitation
            if (demuxer->GetVideoCodec() == AV_CODEC_ID_H264)
            {
                std::cout << "WARNING: H.264 4:4:4 decode is not supported on this GPU." << std::endl;
            }

            // Early reject for AV1 4:4:4 encode path
            if (cliOptions->IsCodecAV1())
            {
                std::cout << "ERROR: AV1 does not support 4:4:4 chroma format." << std::endl;
                return;
            }
        }

        bool bOut10 = demuxer->GetBitDepth() > 8;
        NV_ENC_BUFFER_FORMAT eBufferFormat;
        
        if (bInput444)
        {
            eBufferFormat = bOut10 ? NV_ENC_BUFFER_FORMAT_YUV444_10BIT : NV_ENC_BUFFER_FORMAT_YUV444;
        }
        else if (bInput422)
        {
            eBufferFormat = bOut10 ? NV_ENC_BUFFER_FORMAT_P210 : NV_ENC_BUFFER_FORMAT_NV16;
        }
        else
        {
            eBufferFormat = bOut10 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
        }

        auto EncodeDeleteFunc = [](NvEncoderCuda* pEnc) {
            if (pEnc) {
                pEnc->DestroyEncoder();
                delete pEnc;
            }
        };
        NvEncCudaPtr pEnc(new NvEncoderCuda(cuContext, demuxer->GetWidth(), demuxer->GetHeight(),
            eBufferFormat, MIN_QUEUE_SIZE), 
            EncodeDeleteFunc);

        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        initializeParams.encodeConfig = &encodeConfig;
        pEnc->CreateDefaultEncoderParams(&initializeParams, cliOptions->GetEncodeGUID(), 
            cliOptions->GetPresetGUID(), cliOptions->GetTuningInfo());

        // Set chromaFormatIDC for 444 or 422 encoding
        if (bInput444)
        {
            if (cliOptions->IsCodecH264())
            {
                initializeParams.encodeConfig->encodeCodecConfig.h264Config.chromaFormatIDC = 3;
            }
            else if (cliOptions->IsCodecHEVC())
            {
                initializeParams.encodeConfig->encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
            }
        }
        else if (bInput422)
        {
            if (cliOptions->IsCodecH264())
            {
                initializeParams.encodeConfig->encodeCodecConfig.h264Config.chromaFormatIDC = 2;
            }
            else if (cliOptions->IsCodecHEVC())
            {
                initializeParams.encodeConfig->encodeCodecConfig.hevcConfig.chromaFormatIDC = 2;
            }
        }

        cliOptions->SetInitParams(&initializeParams, eBufferFormat);

        pEnc->CreateEncoder(&initializeParams);
        AppTransPerf transcoder(pEnc, *demuxer, *decoder, cuContext);
        transcoder.CreateTranscodePipeline();
        *frameCount = transcoder.ExecutePipeline();
        
        double elapsedTime = w.Stop();
        double fps = *frameCount / elapsedTime;
        std::cout << "Thread FPS = " << fps << std::endl;
    }
    catch (...) {
        vExceptionPtr = std::current_exception();
    }
}

AppTransPerf::AppTransPerf(NvEncCudaPtr& enc, FFmpegDemuxer& demux, NvDecoder& dec, CUcontext ctx) :
    pEnc(std::move(enc)),
    demuxer(demux),
    decoder(dec),
    cuContext(ctx),
    vExceptionPtrs(4, nullptr),
    decodeQueue(queueSize),
    cudaProcessingQueue(queueSize),
    encodeQueue(queueSize),
    outputQueue(queueSize),
    completionQueue(queueSize)
{
    queueSize = pEnc->GetNumBuffers();

    for (int i = 0; i < queueSize; i++) {
        TranscodeTask fillerTask = { 0 };
        push_back(completionQueue, fillerTask);
    }

    CUDA_DRVAPI_CALL(cuCtxPushCurrent(cuContext));
    ck(cuStreamCreate(&encStream, CU_STREAM_DEFAULT));
    pEnc->SetIOCudaStreams(&encStream, &encStream);
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
}

AppTransPerf::~AppTransPerf() {
    if (encStream) {
        try {
            CUDA_DRVAPI_CALL(cuCtxPushCurrent(cuContext));
            ck(cuStreamDestroy(encStream));
            CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
        }
        catch (...) {
            if (logger) {
                LOG(ERROR) << "Exception occurred while destroying CUDA stream in AppTransPerf destructor";
            }
        }
    }
}

void AppTransPerf::CreateTranscodePipeline() {
    decodeThread = std::thread(&AppTransPerf::ProcessDecodeTask, this, std::ref(decodeQueue), std::ref(cudaProcessingQueue));
    cudaThread = std::thread(&AppTransPerf::ProcessCudaTask, this, std::ref(cudaProcessingQueue), std::ref(encodeQueue));
    encodeThread = std::thread(&AppTransPerf::ProcessEncodeTask, this, std::ref(encodeQueue), std::ref(outputQueue));
    outputThread = std::thread(&AppTransPerf::ProcessOutputTask, this, std::ref(outputQueue), std::ref(completionQueue));
}

void AppTransPerf::ProcessDecodeTask(ConcurrentQueue<TranscodeTask>& decQueue, ConcurrentQueue<TranscodeTask>& cudaProcQueue)
{
    try {
        std::ostringstream threadName;
        threadName << "DecodeThread_" << std::this_thread::get_id();
        SetThreadName(threadName.str().c_str());

        uint8_t* pData;
        int nBytes, isVideoPacket = 0, streamIndex, nFrameReturned, frameCnt = 0;
        int64_t pts, dts, duration;

        do {
            demuxer.Demux(&pData, &nBytes, &pts, &dts, &duration, &isVideoPacket, &streamIndex);

            if (!isVideoPacket) continue;

            nFrameReturned = decoder.Decode(pData, nBytes, 0, pts);

            if (nFrameReturned) {
                for (int i = 0; i < nFrameReturned; i++) {
                    TranscodeTask task = pop_front(decQueue);

                    if (task.unlockDecFrame) {
                        decoder.UnlockFrame(&task.unlockDecFrame);
                        task.unlockDecFrame = nullptr;
                    }

                    uint8_t* pFrame = decoder.GetLockedFrame(&pts);
                    task.decodedFrame = pFrame;
                    task.frameNum = frameCnt;
                    task.unlockDecFrame = pFrame;
                    
                    task.videoPkt.pts = pts;
                    task.videoPkt.dts = pts;
                    task.videoPkt.duration = duration;
                    task.videoPkt.streamIndex = streamIndex;

                    push_back(cudaProcQueue, task);
                    startCuda.store(true);
                    frameCnt++;
                }
            }
        } while (nBytes && isRunning());

        if (isRunning()) {
            TranscodeTask task = pop_front(decQueue);
            if (task.unlockDecFrame) {
                decoder.UnlockFrame(&task.unlockDecFrame);
                task.unlockDecFrame = nullptr;
            }
            task.endTask = true;
            task.frameNum = frameCnt;
            push_back(cudaProcQueue, task);
        }
    }
    catch (...) {
        error = true;
        vExceptionPtrs[0] = std::current_exception();
    }
}

void AppTransPerf::ProcessCudaTask(ConcurrentQueue<TranscodeTask>& cudaProcQueue, ConcurrentQueue<TranscodeTask>& encQueue)
{
    try {
        std::ostringstream threadName;
        threadName << "CudaThread_" << std::this_thread::get_id();
        SetThreadName(threadName.str().c_str());
        WaitForStart(startCuda);
        ck(cuCtxSetCurrent((CUcontext)pEnc->GetDevice()));

        while (isRunning()) {
            TranscodeTask task = pop_front(cudaProcQueue);

            if (task.endTask) {
                push_back(encQueue, task);
                break;
            }

            const NvEncInputFrame* encoderInputFrame = pEnc->GetNextInputFrame(task.frameNum);

            NvEncoderCuda::CopyToDeviceFrame(cuContext,
                task.decodedFrame,
                decoder.GetDeviceFramePitch(),
                (CUdeviceptr)encoderInputFrame->inputPtr,
                encoderInputFrame->pitch,
                pEnc->GetEncodeWidth(),
                pEnc->GetEncodeHeight(),
                CU_MEMORYTYPE_DEVICE,
                encoderInputFrame->bufferFormat,
                encoderInputFrame->chromaOffsets,
                encoderInputFrame->numChromaPlanes,
                false, encStream);

            push_back(encQueue, task);
            startEncode.store(true);
        }
    }
    catch (...) {
        error = true;
        vExceptionPtrs[1] = std::current_exception();
    }
}

void AppTransPerf::ProcessEncodeTask(ConcurrentQueue<TranscodeTask>& encQueue, ConcurrentQueue<TranscodeTask>& outputQueue) 
{
    try {
        std::ostringstream threadName;
        threadName << "EncodeThread_" << std::this_thread::get_id();
        SetThreadName(threadName.str().c_str());
        WaitForStart(startEncode);

        while (isRunning()) {
            TranscodeTask task = pop_front(encQueue);

            if (task.endTask) {
                pEnc->EndEncode();
                push_back(outputQueue, task);
                eos = true;
                break;
            }

            NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
            picParams.inputTimeStamp = task.videoPkt.pts;
            pEnc->EncodeFrame(&picParams);

            push_back(outputQueue, task);
            startOutput.store(true);
        }
    }
    catch (...) {
        error = true;
        vExceptionPtrs[2] = std::current_exception();
    }
}

void AppTransPerf::ProcessOutputTask(ConcurrentQueue<TranscodeTask>& outputQueue, ConcurrentQueue<TranscodeTask>& completionQueue) 
{
    try {
        std::ostringstream threadName;
        threadName << "OutputThread_" << std::this_thread::get_id();
        SetThreadName(threadName.str().c_str());
        WaitForStart(startOutput);
        int delay = pEnc->GetMinOutputDelay();
        bool endTask = false;

        while (eos.load() == 0 && isRunning()) {
            if (outputQueue.size() > delay) {
                TranscodeTask task = pop_front(outputQueue);

                if (task.endTask) {
                    push_back(completionQueue, task);
                    endTask = true;
                    break;
                }

                NvEncOutputFrame outputFrame;
                pEnc->GetLockBitstream(task.frameNum, pEnc->GetOutputBfr(task.frameNum), outputFrame);
                pEnc->UnLockBitstream(task.frameNum, pEnc->GetOutputBfr(task.frameNum), true);

                processedFrames++;
                push_back(completionQueue, task);
            }
        }

        while (isRunning() && !endTask) {
            if (outputQueue.size() > 0) {
                TranscodeTask task = pop_front(outputQueue);

                if (task.endTask) {
                    push_back(completionQueue, task);
                    break;
                }

                NvEncOutputFrame outputFrame;
                pEnc->GetLockBitstream(task.frameNum, pEnc->GetOutputBfr(task.frameNum), outputFrame);
                pEnc->UnLockBitstream(task.frameNum, pEnc->GetOutputBfr(task.frameNum), true);

                processedFrames++;
                push_back(completionQueue, task);
            }
        }
    }
    catch (...) {
        error = true;
        vExceptionPtrs[3] = std::current_exception();
    }
}

int AppTransPerf::ExecutePipeline() 
{
    while (isRunning()) {
        TranscodeTask compTask = pop_front(completionQueue);
        if (!compTask.endTask) {
            uint8_t* unlockFrame = compTask.unlockDecFrame;
            compTask = { 0 };
            compTask.unlockDecFrame = unlockFrame;
            push_back(decodeQueue, compTask);
        }
        else {
            while (decodeQueue.size() > 0) {
                TranscodeTask task = pop_front(decodeQueue);
                if (task.unlockDecFrame) {
                    decoder.UnlockFrame(&task.unlockDecFrame);
                }
            }
            break;
        }
    }

    decodeThread.join();
    cudaThread.join();
    encodeThread.join();
    outputThread.join();

    CheckForExceptions();
    return processedFrames.load();
}

static void ShowBriefHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA Video Transcoder Performance Sample Application\n";
    oss << "==============================================\n\n";

    oss << "Usage: AppTransPerf -i <input_file> [options]\n\n";

    // Brief table of core arguments
    oss << "Common Arguments:\n";
    oss << std::left << std::setw(25) << "Argument"
        << std::setw(12) << "Type"
        << "Default Value\n";
    oss << std::string(50, '-') << "\n";

    oss << std::left << std::setw(25) << "-i <path>"
        << std::setw(12) << "Required"
        << "N/A\n";
    oss << std::left << std::setw(25) << "-gpu <n>"
        << std::setw(12) << "Optional"
        << "0\n";
    oss << std::left << std::setw(25) << "-thread <n>"
        << std::setw(12) << "Optional"
        << "2\n";
    oss << std::left << std::setw(25) << "-single"
        << std::setw(12) << "Optional"
        << "false\n";

    oss << "\nFor detailed help, use -A/--advanced-options\n";
    std::cout << oss.str();
    exit(0);
}

static void ShowHelpAdvanced()
{
    std::ostringstream oss;
    oss << "NVIDIA Video Transcoder Performance Sample Application\n";
    oss << "==============================================\n\n";

    oss << "Usage: AppTransPerf -i <input_file> [options]\n\n";

    // Detailed descriptions
    oss << "Basic Options:\n";
    oss << std::left << std::setw(25) << "-i <path>" << ": Input video file to transcode\n";
    oss << std::left << std::setw(25) << "-gpu <n>" << ": GPU device ordinal\n";
    oss << std::left << std::setw(25) << "-thread <n>" << ": Number of encoding threads\n";
    oss << std::left << std::setw(25) << "-single" << ": Use single context\n";
    oss << std::left << std::setw(25) << "-h/--help" << ": Print basic usage information\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print detailed usage information\n";

    // Important notes
    oss << "\nNotes:\n";
    oss << "------\n";
    oss << "* Multiple contexts are used by default for optimal performance\n";
    oss << "* Single context may result in suboptimal performance\n";
    oss << "* Each thread performs both decode and encode operations\n";
    oss << std::endl;

    // Encoder Parameters
    oss << "Encoder Parameters:\n";
    oss << NvEncoderInitParam().GetHelpMessage(false, false, true);

    std::cout << oss.str();
    exit(0);
}

void ShowHelpAndExit(const char *szBadOption = NULL)
{
    if (szBadOption)
    {
        std::ostringstream oss;
        oss << "Error parsing \"" << szBadOption << "\"\n";
        oss << "Use -h/--help for basic usage or -A/--advanced-options for detailed information\n";
        throw std::invalid_argument(oss.str());
    }
}

void ParseCommandLine(int argc, char *argv[], char *szInputFileName,
    int &iGpu, int &nThread, bool &bSingle, NvEncoderInitParam &initParam)
{
    std::ostringstream oss;

    if (argc == 1) {
        std::cout << "No Arguments provided! Please refer to the following for options:\n";
        ShowBriefHelp();
    }

    for (int i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help"))
        {
            ShowBriefHelp();
        }
        if (!_stricmp(argv[i], "-A") || !_stricmp(argv[i], "--advanced-options"))
        {
            ShowHelpAdvanced();
        }
        if (!_stricmp(argv[i], "-i"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-i");
            }
            sprintf(szInputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-gpu"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-gpu");
            }
            iGpu = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-thread"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-thread");
            }
            nThread = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-single"))
        {
            bSingle = true;
            continue;
        }
        // Regard as encoder parameter
        if (argv[i][0] != '-')
        {
            ShowHelpAndExit(argv[i]);
        }
        oss << argv[i] << " ";
        while (i + 1 < argc && argv[i + 1][0] != '-')
        {
            oss << argv[++i] << " ";
        }
    }
    initParam = NvEncoderInitParam(oss.str().c_str());
}

int main(int argc, char **argv)
{
    char szInFilePath[256] = "";
    int iGpu = 0;
    int nThread = 2;
    bool bSingle = false;
    
    try
    {
        NvEncoderInitParam encodeCLIOptions;
        ParseCommandLine(argc, argv, szInFilePath, iGpu, nThread, bSingle, encodeCLIOptions);

        CheckInputFile(szInFilePath);

        ck(cuInit(0));
        int nGpu = 0;
        ck(cuDeviceGetCount(&nGpu));
        if (iGpu < 0 || iGpu >= nGpu)
        {
            std::cout << "GPU ordinal out of range. Should be within [" << 0 << ", " << nGpu - 1 << "]" << std::endl;
            return 1;
        }
        CUdevice cuDevice = 0;
        ck(cuDeviceGet(&cuDevice, iGpu));
        char szDeviceName[80];
        ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
        std::cout << "GPU in use: " << szDeviceName << std::endl;

        std::vector<std::unique_ptr<FFmpegDemuxer>> vDemuxer;
        std::vector<std::unique_ptr<NvDecoder>> vpDec;

        std::vector<NvThread> vpThread;
        std::vector<std::exception_ptr> vExceptionPtrs(nThread);
        std::vector<int> vnFrameTrans(nThread);
        std::vector<CUcontext> vContexts;

        CUcontext currentContext = NULL;

        auto t0 = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < nThread; i++)
        {
            if (bSingle)
            {
                if (currentContext == nullptr) {
                    ck(NVCODEC_CUDA_CTX_CREATE(&currentContext, 0, cuDevice));
                    vContexts.push_back(currentContext);
                }
            }
            else
            {
                ck(NVCODEC_CUDA_CTX_CREATE(&currentContext, 0, cuDevice));
                vContexts.push_back(currentContext);
            }

            std::unique_ptr<FFmpegDemuxer> demuxer(new FFmpegDemuxer(szInFilePath));

            vDemuxer.push_back(std::move(demuxer));

            std::unique_ptr<NvDecoder> dec(new NvDecoder(currentContext, true, 
                FFmpeg2NvCodecId(vDemuxer[i]->GetVideoCodec()), false, true));

            vpDec.push_back(std::move(dec));

            vpThread.push_back(NvThread(std::thread(TransProc,
                currentContext,
                vpDec[i].get(),
                vDemuxer[i].get(),
                &vnFrameTrans[i],
                &encodeCLIOptions,
                std::ref(vExceptionPtrs[i]))));
        }

        for (int i = 0; i < nThread; i++) 
        {
            vpThread[i].join();
        }

        for (int i = 0; i < nThread; i++)
        {
            if (vExceptionPtrs[i])
            {
                std::rethrow_exception(vExceptionPtrs[i]);
            }
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(t1.time_since_epoch() - t0.time_since_epoch()).count();

        int nFrameTransTotal = 0;
        for (int i = 0; i < nThread; i++)
        {
            nFrameTransTotal += vnFrameTrans[i];
        }

        std::cout << "nFrameTransTotal=" << nFrameTransTotal << ", time=" << msec << " millisec, FPS=" << (nFrameTransTotal * 1000 / msec) << std::endl;
        
        for (int i = 0; i < nThread; i++) {
            vpDec[i].reset(nullptr);
        }
        
        for (auto context : vContexts) {
            try {
                ck(cuCtxDestroy(context));
            }
            catch (...) {
            }
        }
    }
    catch (const std::exception& ex)
    {
        std::cout << ex.what();
        exit(1);
    }
    return 0;
}
