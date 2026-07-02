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
*  This sample application demonstrates transcoding of an input video stream
*  using multiple threads for each of the transcoding tasks: decode, cuda
*  processing, encode and writing of output to the file.
*
*  If requested by the user, the bit-depth of the decoded content will be
*  converted to the target bit-depth before encoding. The only supported
*  conversions are from 8 bit to 10 bit (per component) and vice versa.
*/

#include <cuda.h>
#include <cuda_runtime.h>
#include <iostream>
#include <memory>
#include <functional>
#include <iomanip>
#include "AppTrans.h"

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

static void ShowBriefHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA Video Transcoder Sample Application\n";
    oss << "====================================\n\n";
    
    oss << "Usage: AppTrans -i <input_file> [options]\n\n";

    // Brief table of core arguments
    oss << "Common Arguments:\n";
    oss << std::left << std::setw(25) << "Argument" 
        << std::setw(12) << "Type"
        << "Default Value\n";
    oss << std::string(50, '-') << "\n";

    oss << std::left << std::setw(25) << "-i <path>" 
        << std::setw(12) << "Required"
        << "N/A\n";
    oss << std::left << std::setw(25) << "-o <path>" 
        << std::setw(12) << "Optional"
        << "codec-based\n";
    oss << std::left << std::setw(25) << "-ob <depth>" 
        << std::setw(12) << "Optional"
        << "Input depth\n";
    oss << std::left << std::setw(25) << "-gpu <n>" 
        << std::setw(12) << "Optional"
        << "0\n";

    oss << "\nFor detailed help, use -A/--advanced-options\n";
    std::cout << oss.str();
    exit(0);
}

static void ShowHelpAdvanced()
{
    std::ostringstream oss;
    oss << "NVIDIA Video Transcoder Sample Application\n";
    oss << "====================================\n\n";
    
    oss << "Usage: AppTrans -i <input_file> [options]\n\n";

    // Detailed descriptions
    oss << "Basic Options:\n";
    oss << std::left << std::setw(25) << "-i <path>" << ": Input video file to transcode\n";
    oss << std::left << std::setw(25) << "-o <path>" << ": Output file (.mp4/.mov for containers)\n";
    oss << std::left << std::setw(25) << "-ob <depth>" << ": Output bit depth (8 or 10)\n";
    oss << std::left << std::setw(25) << "-gpu <n>" << ": GPU device ordinal\n";
    oss << std::left << std::setw(25) << "-h/--help" << ": Print basic usage information\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print detailed usage information\n";

    // Important notes
    oss << "\nNotes:\n";
    oss << "------\n";
    oss << "* Supports bit-depth conversion between 8-bit and 10-bit\n";
    oss << "* Output format is elementary stream if container not specified\n";
    oss << "* Default output filename is out.<codec> if not specified\n";
    oss << std::endl;

    // Encoder Parameters
    oss << "Encoder Parameters:\n";
    oss << NvEncoderInitParam().GetHelpMessage(false, false, true);
    
    std::cout << oss.str();
    exit(0);
}

AppTrans::AppTrans(NvEncCudaPtr& enc, FFmpegDemuxer& demux, std::unique_ptr<FFmpegMuxer> mux,
    MEDIA_FORMAT format, NvDecoder& dec, CUcontext ctx, bool out10,
    const char* szOutFilePath) :
    pEnc(std::move(enc)),
    demuxer(demux),
    muxer(std::move(mux)),
    mediaFormat(format),
    decoder(dec),
    cuContext(ctx),
    bOut10(out10),
    queueSize(pEnc->GetNumBuffers()),
    vExceptionPtrs(4, nullptr),  // 4 threads: decode, cuda, encode, output
    decodeQueue(queueSize),
    cudaProcessingQueue(queueSize),
    encodeQueue(queueSize),
    outputQueue(queueSize),
    completionQueue(queueSize)
{
    if (mediaFormat == MEDIA_FORMAT_ELEMENTARY)
    {
        fpOut = std::unique_ptr<std::ofstream>(new std::ofstream(szOutFilePath, std::ios::out | std::ios::binary));
        if (!fpOut || !fpOut->is_open())
        {
            std::ostringstream err;
            err << "Unable to open output file: " << szOutFilePath << std::endl;
            throw std::invalid_argument(err.str());
        }
    }

    // Initialize completion queue with filler tasks
    for (unsigned int i = 0; i < pEnc->GetNumBuffers(); i++)
    {
        TranscodeTask fillerTask = { 0 };
        push_back(completionQueue, fillerTask);
    }

    CUDA_DRVAPI_CALL(cuCtxPushCurrent(cuContext));
    ck(cuStreamCreate(&encStream, CU_STREAM_DEFAULT));
    pEnc->SetIOCudaStreams(&encStream, &encStream);
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
}

AppTrans::~AppTrans()
{
    if (fpOut && fpOut->is_open())
    {
        fpOut->close();
    }

    if (encStream)
    {
        CUDA_DRVAPI_CALL(cuCtxPushCurrent(cuContext));
        ck(cuStreamDestroy(encStream));
        CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
    }
}

void AppTrans::CreateTranscodePipeline()
{
    decodeThread = std::thread(&AppTrans::ProcessDecodeTask, this, std::ref(decodeQueue), std::ref(cudaProcessingQueue));
    cudaThread = std::thread(&AppTrans::ProcessCudaTask, this, std::ref(cudaProcessingQueue), std::ref(encodeQueue));
    encodeThread = std::thread(&AppTrans::ProcessEncodeTask, this, std::ref(encodeQueue), std::ref(outputQueue));
    outputThread = std::thread(&AppTrans::ProcessOutputTask, this, std::ref(outputQueue), std::ref(completionQueue));
}

void AppTrans::ProcessDecodeTask(ConcurrentQueue<TranscodeTask>& decQueue, ConcurrentQueue<TranscodeTask>& cudaProcQueue)
{
    try
    {
        SetThreadName("DecodeThread");

        uint8_t* pData;
        int nBytes, isVideoPacket = 0, streamIndex, nFrameReturned, frameCnt = 0;
        int64_t pts, dts, duration;
        std::vector<Packet> nonVidPkt;

        do {
            demuxer.Demux(&pData, &nBytes, &pts, &dts, &duration, &isVideoPacket, &streamIndex);

            if (!isVideoPacket)
            {
                if (mediaFormat != MEDIA_FORMAT_ELEMENTARY)
                {
                    Packet pkt;
                    pkt.data.resize(nBytes);
                    std::copy(pData, pData + nBytes, pkt.data.begin());
                    pkt.pts = pts;
                    pkt.dts = dts;
                    pkt.duration = duration;
                    pkt.nBytes = nBytes;
                    pkt.streamIndex = streamIndex;
                    nonVidPkt.push_back(pkt);
                }
                continue;
            }
            nFrameReturned = decoder.Decode(pData, nBytes, 0, pts);

            if (nFrameReturned)
            {
                for (int i = 0; i < nFrameReturned; i++)
                {
                    TranscodeTask task = pop_front(decQueue);

                    if (!nonVidPkt.empty())
                    {
                        task.nonVidPkt = nonVidPkt;
                        nonVidPkt.clear();
                    }

                    if (task.unlockDecFrame)
                    {
                        decoder.UnlockFrame(&task.unlockDecFrame);
                        task.unlockDecFrame = nullptr;
                    }
                    uint8_t* pFrame = decoder.GetLockedFrame(&pts);
                    task.decodedFrame = pFrame;
                    task.frameNum = frameCnt;
                    task.unlockDecFrame = pFrame;

                    task.videoPkt.duration = duration;
                    task.videoPkt.pts = pts;
                    task.videoPkt.dts = pts;
                    task.videoPkt.streamIndex = streamIndex;

                    push_back(cudaProcQueue, task);
                    startCuda.store(true);

                    frameCnt++;
                }
            }
        } while (nBytes && isRunning());

        if (isRunning())
        {
            TranscodeTask task = pop_front(decQueue);
            if (task.unlockDecFrame)
            {
                decoder.UnlockFrame(&task.unlockDecFrame);
                task.unlockDecFrame = nullptr;
            }

            task.endTask = true;
            task.frameNum = frameCnt;
            if (!nonVidPkt.empty())
            {
                task.nonVidPkt = nonVidPkt;
                nonVidPkt.clear();
            }
            push_back(cudaProcQueue, task);
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[0] = std::current_exception();
    }
}

void AppTrans::ProcessCudaTask(ConcurrentQueue<TranscodeTask>& cudaProcQueue, ConcurrentQueue<TranscodeTask>& encQueue)
{
    try
    {
        SetThreadName("CudaThread");
        WaitForStart(startCuda);

        ck(cuCtxSetCurrent((CUcontext)pEnc->GetDevice()));

        while (isRunning())
        {
            TranscodeTask task = pop_front(cudaProcQueue);

            if (task.endTask)
            {
                push_back(encQueue, task);
                break;
            }

            const NvEncInputFrame* encoderInputFrame = pEnc->GetNextInputFrame(task.frameNum);

            if ((bOut10 && decoder.GetBitDepth() > 8) || (!bOut10 && decoder.GetBitDepth() == 8))
            {
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
            }
            else
            {
                if (bOut10)
                {
                    ConvertUInt8ToUInt16(task.decodedFrame, (uint16_t*)encoderInputFrame->inputPtr,
                        decoder.GetDeviceFramePitch(), encoderInputFrame->pitch,
                        pEnc->GetEncodeWidth(),
                        pEnc->GetEncodeHeight() + ((pEnc->GetEncodeHeight() + 1) / 2), encStream);
                }
                else
                {
                    ConvertUInt16ToUInt8((uint16_t*)task.decodedFrame, (uint8_t*)encoderInputFrame->inputPtr,
                        decoder.GetDeviceFramePitch(), encoderInputFrame->pitch,
                        pEnc->GetEncodeWidth(),
                        pEnc->GetEncodeHeight() + ((pEnc->GetEncodeHeight() + 1) / 2), encStream);
                }
            }

            push_back(encQueue, task);
            startEncode.store(true);
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[1] = std::current_exception();
    }
}

void AppTrans::ProcessEncodeTask(ConcurrentQueue<TranscodeTask>& encQueue, ConcurrentQueue<TranscodeTask>& outputQueue)
{
    try
    {
        SetThreadName("EncodeThread");
        WaitForStart(startEncode);

        while (isRunning())
        {
            TranscodeTask task = pop_front(encQueue);

            if (task.endTask)
            {
                pEnc->EndEncode();
                push_back(outputQueue, task);
                eos = true;
                break;
            }

            NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
            if (mediaFormat != MEDIA_FORMAT_ELEMENTARY)
                picParams.inputTimeStamp = task.videoPkt.pts;
            NVENCSTATUS nvStatus = pEnc->EncodeFrame(&picParams);

            push_back(outputQueue, task);
            startGetOutput.store(true);
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[2] = std::current_exception();
    }
}

void AppTrans::ProcessOutputTask(ConcurrentQueue<TranscodeTask>& outputQueue, ConcurrentQueue<TranscodeTask>& completionQueue)
{
    try
    {
        SetThreadName("OutputThread");
        WaitForStart(startGetOutput);

        int delay = pEnc->GetMinOutputDelay();
        bool endTask = false;

        while (eos.load() == 0 && isRunning())
        {
            if (outputQueue.size() > delay)
            {
                TranscodeTask task = pop_front(outputQueue);

                if (task.endTask)
                {
                    push_back(completionQueue, task);
                    endTask = true;
                    break;
                }

                WriteOutput(task);
                push_back(completionQueue, task);
            }
        }

        while (isRunning() && !endTask)
        {
            if (outputQueue.size() > 0)
            {
                TranscodeTask task = pop_front(outputQueue);

                if (task.endTask)
                {
                    push_back(completionQueue, task);
                    break;
                }

                WriteOutput(task);
                push_back(completionQueue, task);
            }
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[3] = std::current_exception();
    }
}

void AppTrans::WriteOutput(TranscodeTask& task)
{
    // Write non-video packets first (if any)
    if (task.nonVidPkt.size() && mediaFormat != MEDIA_FORMAT_ELEMENTARY)
    {
        for (const auto& pkt : task.nonVidPkt)
        {
            muxer->Mux(const_cast<uint8_t*>(pkt.data.data()),
                static_cast<unsigned int>(pkt.nBytes),
                pkt.pts, pkt.dts, pkt.duration, pkt.streamIndex);
        }
        task.nonVidPkt.clear();
    }

    // Write video packet
    NvEncOutputFrame outputFrame;
    pEnc->GetLockBitstream(task.frameNum, pEnc->GetOutputBfr(task.frameNum), outputFrame);

    if (mediaFormat == MEDIA_FORMAT_ELEMENTARY)
    {
        fpOut->write(reinterpret_cast<const char*>(outputFrame.frame.data()),
            static_cast<unsigned int>(outputFrame.frame.size()));
    }
    else
    {
        muxer->Mux(reinterpret_cast<unsigned char*>(const_cast<uint8_t*>(outputFrame.frame.data())),
            static_cast<unsigned int>(outputFrame.frame.size()),
            outputFrame.timeStamp,
            task.videoPkt.dts,
            task.videoPkt.duration,
            task.videoPkt.streamIndex,
            outputFrame.pictureType == NV_ENC_PIC_TYPE_IDR,
            pEnc->GetNumBFrames());
    }

    pEnc->UnLockBitstream(task.frameNum, pEnc->GetOutputBfr(task.frameNum), true);
}

void AppTrans::ExecutePipeline(int& totFrames)
{
    while (isRunning())
    {
        TranscodeTask compTask = pop_front(completionQueue);

        if (!compTask.endTask)
        {
            uint8_t* unlockFrame = compTask.unlockDecFrame;
            compTask = { 0 };
            compTask.unlockDecFrame = unlockFrame;

            push_back(decodeQueue, compTask);
        }
        else
        {
            while (decodeQueue.size() > 0)
            {
                TranscodeTask task = pop_front(decodeQueue);

                if (task.unlockDecFrame)
                {
                    decoder.UnlockFrame(&task.unlockDecFrame);
                }
            }
            totFrames = compTask.frameNum;
            break;
        }
    }

    decodeThread.join();
    cudaThread.join();
    encodeThread.join();
    outputThread.join();

    CheckForExceptions();
}

void ShowHelpAndExit(const char* szBadOption = NULL)
{
    if (szBadOption) 
    {
        std::ostringstream oss;
        oss << "Error parsing \"" << szBadOption << "\"\n";
        oss << "Use -h/--help for basic usage or -A/--advanced-options for detailed information\n";
        throw std::invalid_argument(oss.str());
    }
}

void ParseCommandLine(int argc, char* argv[], char* szInputFileName, char* szOutputFileName, int& nOutBitDepth, int& iGpu, NvEncoderInitParam& initParam)
{
    std::ostringstream oss;
    int i;

    if (argc == 1) {
        std::cout << "No Arguments provided! Please refer to the following for options:\n";
        ShowBriefHelp();
    }

    for (i = 1; i < argc; i++)
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
        if (!_stricmp(argv[i], "-o"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-o");
            }
            sprintf(szOutputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-ob"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-ob");
            }
            nOutBitDepth = atoi(argv[i]);
            if (nOutBitDepth != 8 && nOutBitDepth != 10) 
            {
                ShowHelpAndExit("-ob");
            }
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

int main(int argc, char** argv) {
    char szInFilePath[260] = "";
    char szOutFilePath[260] = "";
    int nOutBitDepth = 0;
    int iGpu = 0;
    try
    {
        using NvEncCudaPtr = std::unique_ptr<NvEncoderCuda, std::function<void(NvEncoderCuda*)>>;
        auto EncodeDeleteFunc = [](NvEncoderCuda* pEnc)
        {
            if (pEnc)
            {
                pEnc->DestroyEncoder();
                delete pEnc;
            }
        };

        NvEncCudaPtr pEnc(nullptr, EncodeDeleteFunc);

        NvEncoderInitParam encodeCLIOptions;
        ParseCommandLine(argc, argv, szInFilePath, szOutFilePath, nOutBitDepth, iGpu, encodeCLIOptions);

        CheckInputFile(szInFilePath);

        if (!*szOutFilePath) {
            sprintf(szOutFilePath, encodeCLIOptions.IsCodecH264() ? "out.h264" : encodeCLIOptions.IsCodecHEVC() ? "out.hevc" : "out.av1");
        }

        std::ifstream fpIn(szInFilePath, std::ifstream::in | std::ifstream::binary);
        if (!fpIn)
        {
            std::ostringstream err;
            err << "Unable to open input file: " << szInFilePath << std::endl;
            throw std::invalid_argument(err.str());
        }

        ck(cuInit(0));
        int nGpu = 0;
        ck(cuDeviceGetCount(&nGpu));
        if (iGpu < 0 || iGpu >= nGpu) {
            std::cout << "GPU ordinal out of range. Should be within [" << 0 << ", " << nGpu - 1 << "]" << std::endl;
            return 1;
        }
        CUdevice cuDevice = 0;
        ck(cuDeviceGet(&cuDevice, iGpu));
        char szDeviceName[80];
        ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
        std::cout << "GPU in use: " << szDeviceName << std::endl;
        CUcontext cuContext = NULL;
        ck(NVCODEC_CUDA_CTX_CREATE(&cuContext, 0, cuDevice));

        FFmpegDemuxer demuxer(szInFilePath);
        std::unique_ptr<FFmpegMuxer> muxer;
        MEDIA_FORMAT mediaFormat = muxer->GetMediaFormat(szOutFilePath);

        // Check if input is 422
        bool bInput422 = (demuxer.GetChromaFormat() == AV_PIX_FMT_YUV422P ||
                          demuxer.GetChromaFormat() == AV_PIX_FMT_YUV422P10LE ||
                          demuxer.GetChromaFormat() == AV_PIX_FMT_YUV422P12LE);

        // Check if input is 444
        bool bInput444 = (demuxer.GetChromaFormat() == AV_PIX_FMT_YUV444P ||
                          demuxer.GetChromaFormat() == AV_PIX_FMT_YUV444P10LE ||
                          demuxer.GetChromaFormat() == AV_PIX_FMT_YUV444P12LE);

        if (bInput422)
        {
            std::cout << "INFO: YUV422 is supported from Blackwell (GB20x) GPUs onwards." << std::endl;
            // Early reject for AV1 4:2:2 encode path
            if (encodeCLIOptions.IsCodecAV1())
            {
                std::cout << "ERROR: AV1 does not support 4:2:2 chroma format." << std::endl;
                return 1;
            }
        }
        else if (bInput444)
        {
            // Add warning for H.264 444 decode limitation
            if (demuxer.GetVideoCodec() == AV_CODEC_ID_H264)
            {
                std::cout << "WARNING: H.264 4:4:4 decode is not supported on this GPU." << std::endl;
            }

            // Early reject for AV1 4:4:4 encode path
            if (encodeCLIOptions.IsCodecAV1())
            {
                std::cout << "ERROR: AV1 does not support 4:4:4 chroma format." << std::endl;
                return 1;
            }
        }

        NvDecoder dec(cuContext, true, FFmpeg2NvCodecId(demuxer.GetVideoCodec()), false, true);

        NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
        NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
        AVCodecID codecID = encodeCLIOptions.IsCodecH264() ? AV_CODEC_ID_H264 : encodeCLIOptions.IsCodecHEVC() ? AV_CODEC_ID_HEVC : AV_CODEC_ID_AV1;

        StopWatch processingTime;
        processingTime.Start();

        bool bOut10 = nOutBitDepth ? nOutBitDepth > 8 : demuxer.GetBitDepth() > 8;
        bool bUseIVFContainer = (mediaFormat != MEDIA_FORMAT_ELEMENTARY) ? false : true;
        bool bRepeatSequenceHeader = ((mediaFormat != MEDIA_FORMAT_ELEMENTARY) && (codecID == AV_CODEC_ID_AV1)) ? true : false;

        // Determine buffer format based on input chroma format
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

        pEnc.reset(new NvEncoderCuda(cuContext, demuxer.GetWidth(), demuxer.GetHeight(), 
            eBufferFormat, MIN_QUEUE_SIZE, false, false, bUseIVFContainer, bRepeatSequenceHeader));

            initializeParams.encodeConfig = &encodeConfig;
            pEnc->CreateDefaultEncoderParams(&initializeParams, encodeCLIOptions.GetEncodeGUID(), encodeCLIOptions.GetPresetGUID(), encodeCLIOptions.GetTuningInfo());
            
            // Set chromaFormatIDC for 444 or 422 encoding
            if (bInput444)
            {
                if (encodeCLIOptions.IsCodecH264())
                {
                    initializeParams.encodeConfig->encodeCodecConfig.h264Config.chromaFormatIDC = 3;
                }
                else if (encodeCLIOptions.IsCodecHEVC())
                {
                    initializeParams.encodeConfig->encodeCodecConfig.hevcConfig.chromaFormatIDC = 3;
                }
            }
            else if (bInput422)
            {
                if (encodeCLIOptions.IsCodecH264())
                {
                    initializeParams.encodeConfig->encodeCodecConfig.h264Config.chromaFormatIDC = 2;
                }
                else if (encodeCLIOptions.IsCodecHEVC())
                {
                    initializeParams.encodeConfig->encodeCodecConfig.hevcConfig.chromaFormatIDC = 2;
                }
            }
            
            encodeCLIOptions.SetInitParams(&initializeParams, eBufferFormat);
            pEnc->CreateEncoder(&initializeParams);

        if (mediaFormat != MEDIA_FORMAT_ELEMENTARY) {
            std::vector<unsigned char> vSeqParams;
            pEnc->GetSequenceParams(vSeqParams); 
            muxer = std::unique_ptr<FFmpegMuxer>(new FFmpegMuxer(szOutFilePath, mediaFormat, demuxer.GetAVFormatContext(),
                        codecID, demuxer.GetWidth(), demuxer.GetHeight(), vSeqParams.data(), vSeqParams.size()));
        }

        AppTrans transcoder(pEnc, demuxer, std::move(muxer), mediaFormat, dec, cuContext, bOut10, szOutFilePath);
        int totFrames = 0;

        transcoder.CreateTranscodePipeline();
        transcoder.ExecutePipeline(totFrames);

        double pT = processingTime.Stop();
        std::cout << "Total processing time = " << pT << " seconds, FPS=" << totFrames / pT << " (#totFrames=" << totFrames << ")" << std::endl;

        fpIn.close();
        std::cout << "Saved in file " << szOutFilePath << " of " << (bOut10 ? 10 : 8) << " bit depth" << std::endl;

        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cout << ex.what();
        exit(1);
    }
    return 0;
}
