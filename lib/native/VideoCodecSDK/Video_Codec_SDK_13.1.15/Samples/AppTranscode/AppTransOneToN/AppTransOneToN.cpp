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
*  This sample application demonstrates 1:N transcoding of a single input
*  stream using threads for each of the transcode tasks: decode, cuda processing,
*  encoding, getting the output bitstream.
*  Decoding of frames from the input stream takes place on the single decoder thread.
*  'N' threads are spawned for each of the steps after decoding: cuda processing, encoding
*  and getting the encoded output. A different
*  resolution can be specified for each output stream and the decoded frames
*  will be scaled as required. If no output resolutions are specified, this
*  application will generate two streams: one of 1280x720 and the other of
*  800x480.
*/

#include "AppTransOneToN.h"
#include <cuda_runtime.h>
#include <stdio.h>
#include <iostream>
#include <thread>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string.h>
#include <memory>
#include <unordered_map>
#include <iomanip>

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

TextureObject GetOrCreateTextureObject(uint8_t* currentFrame, int pitch, int width, int height,
    std::unordered_map<uint8_t*, TextureObject>& textureMap, bool isP016, bool is422, bool is444)
{
    auto it = textureMap.find(currentFrame);
    if (it == textureMap.end())
    {
        std::pair<std::unordered_map<uint8_t*, TextureObject>::iterator, bool> result =
            textureMap.insert(std::make_pair(currentFrame, 
                is444 ? CreateTextureObjects444(currentFrame, pitch, width, height, isP016) :
                is422 ? CreateTextureObjects422(currentFrame, pitch, width, height, isP016) :
                        CreateTextureObjects(currentFrame, pitch, width, height, isP016)));
        return result.first->second;
    }
    return it->second;
}

AppTransOneToN::AppTransOneToN(FFmpegDemuxer& demux, NvDecoder& dec, CUcontext ctx,
    bool out10, bool input422, bool input444, char* prefix, char* suffix,
    std::vector<NvEncCudaPtr>& encoders,
    std::vector<std::vector<std::exception_ptr>>& exceptionPtrs) :
    demuxer(demux),
    decoder(dec),
    cuContext(ctx),
    bOut10(out10),
    bInput422(input422),
    bInput444(input444),
    vEncoders(encoders),
    vExceptionPtrs(exceptionPtrs),
    szOutFileNamePrefix(prefix),
    szOutFileNameSuffix(suffix),
    queueSize(encoders[0]->GetNumBuffers()),
    cudaProcessingQueues(encoders.size()),
    encodeQueues(encoders.size()),
    outputQueues(encoders.size()),
    completionQueues(encoders.size()),
    startCuda(encoders.size()),
    startEncode(encoders.size()),
    startGetOutput(encoders.size()),
    eos(encoders.size()),
    inpStreams(encoders.size()),
    sessionStopWatches(encoders.size()),
    outputFiles(encoders.size())
{
    // Assert that all encoders have the same number of buffers
    for (int i = 1; i < vEncoders.size(); ++i)
    {
        assert(vEncoders[i]->GetNumBuffers() == queueSize && "All encoders must have the same number of buffers");
    }

    // Initialize decode queue
    decodeQueue = std::unique_ptr<ConcurrentQueue<TransOneToNTask>>(new ConcurrentQueue<TransOneToNTask>(queueSize));

    // Initialize queue vectors
    for (int i = 0; i < vEncoders.size(); i++)
    {
        cudaProcessingQueues[i] = std::unique_ptr<ConcurrentQueue<TransOneToNTask>>(new ConcurrentQueue<TransOneToNTask>(queueSize));
        encodeQueues[i] = std::unique_ptr<ConcurrentQueue<TransOneToNTask>>(new ConcurrentQueue<TransOneToNTask>(queueSize));
        outputQueues[i] = std::unique_ptr<ConcurrentQueue<TransOneToNTask>>(new ConcurrentQueue<TransOneToNTask>(queueSize));
        completionQueues[i] = std::unique_ptr<ConcurrentQueue<TransOneToNTask>>(new ConcurrentQueue<TransOneToNTask>(queueSize));

        // Initialize completion queue with filler tasks
        for (int j = 0; j < queueSize; j++)
        {
            TransOneToNTask fillerTask = { 0 };
            fillerTask.frameNum = -1;
            push_back(completionQueues[i], fillerTask);
        }
    }

    CUDA_DRVAPI_CALL(cuCtxPushCurrent(cuContext));
    for (int i = 0; i < vEncoders.size(); ++i)
    {
        ck(cuStreamCreate(&inpStreams[i], CU_STREAM_DEFAULT));
        vEncoders[i]->SetIOCudaStreams(&inpStreams[i], &inpStreams[i]);

        char szOutFilePath[260];
        sprintf(szOutFilePath, "%s_%dx%d_%d.%s", szOutFileNamePrefix, vEncoders[i]->GetEncodeWidth(), vEncoders[i]->GetEncodeHeight(), i, szOutFileNameSuffix);
        outputFiles[i].open(szOutFilePath, std::ios::out | std::ios::binary);
        if (!outputFiles[i].is_open())
        {
            std::cout << "Unable to open output file: " << szOutFilePath << std::endl;
            throw std::runtime_error("Failed to open output file");
        }
    }
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
}

AppTransOneToN::~AppTransOneToN()
{
    for (auto& stream : inpStreams)
    {
        cudaStreamDestroy(stream);
    }
    for (auto& file : outputFiles)
    {
        if (file.is_open())
        {
            file.close();
        }
    }
}

void AppTransOneToN::CreateTranscodePipeline()
{
    // Single decode thread that provides input to all encoders
    decodeThread = std::thread(&AppTransOneToN::ProcessDecodeTask, this, std::ref(*decodeQueue), std::ref(cudaProcessingQueues));

    // Create multiple threads for 'N' encoders
    for (int i = 0; i < vEncoders.size(); i++)
    {
        cudaThreads.emplace_back(&AppTransOneToN::ProcessCudaTask, this, std::ref(*cudaProcessingQueues[i]), std::ref(*encodeQueues[i]), i);
        encodeThreads.emplace_back(&AppTransOneToN::ProcessEncodeTask, this, std::ref(*encodeQueues[i]), std::ref(*outputQueues[i]), i);
        outputThreads.emplace_back(&AppTransOneToN::ProcessOutputTask, this, std::ref(*outputQueues[i]), std::ref(*completionQueues[i]), i);
    }
}

void AppTransOneToN::ProcessDecodeTask(ConcurrentQueue<TransOneToNTask>& decQueue, std::vector<std::unique_ptr<ConcurrentQueue<TransOneToNTask>>>& cudaProcQueues)
{
    try
    {
        SetThreadName("DecodeThread");

        for (int i = 0; i < vEncoders.size(); i++)
        {
            sessionStopWatches[i].Start();
        }

        uint8_t* pData;
        int nBytes, isVideoPacket, streamIndex, nFrameReturned, frameCnt = 0;
        int64_t pts, dts, duration;

        do {
            demuxer.Demux(&pData, &nBytes, &pts, &dts, &duration, &isVideoPacket, &streamIndex);

            nFrameReturned = decoder.Decode(pData, nBytes, 0, pts);

            for (int i = 0; i < nFrameReturned; i++)
            {
                TransOneToNTask task = pop_front(decQueue);

                if (task.unlockDecFrame)
                {
                    decoder.UnlockFrame(&task.unlockDecFrame);
                }

                uint8_t* pFrame = decoder.GetLockedFrame(&pts);

                task.decodedFrame = pFrame;
                task.unlockDecFrame = pFrame;
                task.frameNum = frameCnt;
                task.videoPkt.duration = duration;
                task.videoPkt.pts = pts;
                task.videoPkt.dts = pts;
                task.videoPkt.streamIndex = streamIndex;

                for (int encId = 0; encId < vEncoders.size(); encId++)
                {
                    push_back(cudaProcQueues[encId], task);
                    startCuda[encId] = true;
                }
                frameCnt++;
            }
        } while (nBytes && isRunning());

        if (isRunning())
        {
            TransOneToNTask task = pop_front(decQueue);
            task.endTask = true;
            task.frameNum = frameCnt;
            for (int encId = 0; encId < vEncoders.size(); encId++)
            {
                push_back(cudaProcQueues[encId], task);
            }
        }

    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[0][0] = std::current_exception();
    }
}

void AppTransOneToN::ProcessCudaTask(ConcurrentQueue<TransOneToNTask>& cudaProcQueue, ConcurrentQueue<TransOneToNTask>& encQueue, int encId)
{
    std::unordered_map<uint8_t*, TextureObject> textureMap;
    try
    {
        SetThreadName("CudaThread", encId);

        ck(cuCtxSetCurrent((CUcontext)vEncoders[encId]->GetDevice()));

        WaitForStart(startCuda[encId]);

        while (isRunning())
        {
            TransOneToNTask task = pop_front(cudaProcQueue);

            if (task.endTask)
            {
                push_back(encQueue, task);
                break;
            }

            const NvEncInputFrame* encoderInputFrame = vEncoders[encId]->GetNextInputFrame(task.frameNum);
            
            if (bInput444)
            {
                // 444 format
                if (bOut10)
                {
                    TextureObject texObj = GetOrCreateTextureObject(task.decodedFrame, decoder.GetDeviceFramePitch(), demuxer.GetWidth(), demuxer.GetHeight(), textureMap, true, false, true);

                    ResizeYUV444_10BitWithTexture((unsigned char*)encoderInputFrame->inputPtr, (int)encoderInputFrame->pitch,
                        vEncoders[encId]->GetEncodeWidth(), vEncoders[encId]->GetEncodeHeight(),
                        texObj, demuxer.GetWidth(), demuxer.GetHeight(), nullptr, nullptr, inpStreams[encId]);
                }
                else
                {
                    TextureObject texObj = GetOrCreateTextureObject(task.decodedFrame, decoder.GetDeviceFramePitch(), demuxer.GetWidth(), demuxer.GetHeight(), textureMap, false, false, true);

                    ResizeYUV444WithTexture((unsigned char*)encoderInputFrame->inputPtr, (int)encoderInputFrame->pitch,
                        vEncoders[encId]->GetEncodeWidth(), vEncoders[encId]->GetEncodeHeight(),
                        texObj, demuxer.GetWidth(), demuxer.GetHeight(), nullptr, nullptr, inpStreams[encId]);
                }
            }
            else if (bInput422)
            {
                // 422 format
                if (bOut10)
                {
                    TextureObject texObj = GetOrCreateTextureObject(task.decodedFrame, decoder.GetDeviceFramePitch(), demuxer.GetWidth(), demuxer.GetHeight(), textureMap, true, true, false);

                    ResizeP210WithTexture((unsigned char*)encoderInputFrame->inputPtr, (int)encoderInputFrame->pitch,
                        vEncoders[encId]->GetEncodeWidth(), vEncoders[encId]->GetEncodeHeight(),
                        texObj, demuxer.GetWidth(), demuxer.GetHeight(), nullptr, inpStreams[encId]);
                }
                else
                {
                    TextureObject texObj = GetOrCreateTextureObject(task.decodedFrame, decoder.GetDeviceFramePitch(), demuxer.GetWidth(), demuxer.GetHeight(), textureMap, false, true, false);

                    ResizeNv16WithTexture((unsigned char*)encoderInputFrame->inputPtr, (int)encoderInputFrame->pitch,
                        vEncoders[encId]->GetEncodeWidth(), vEncoders[encId]->GetEncodeHeight(),
                        texObj, demuxer.GetWidth(), demuxer.GetHeight(), nullptr, inpStreams[encId]);
                }
            }
            else
            {
                // 420 format
                if (bOut10)
                {
                    TextureObject texObj = GetOrCreateTextureObject(task.decodedFrame, decoder.GetDeviceFramePitch(), demuxer.GetWidth(), demuxer.GetHeight(), textureMap, true, false, false);

                    ResizeP016WithTexture((unsigned char*)encoderInputFrame->inputPtr, (int)encoderInputFrame->pitch,
                        vEncoders[encId]->GetEncodeWidth(), vEncoders[encId]->GetEncodeHeight(),
                        texObj, demuxer.GetWidth(), demuxer.GetHeight(), nullptr, inpStreams[encId]);
                }
                else
                {
                    TextureObject texObj = GetOrCreateTextureObject(task.decodedFrame, decoder.GetDeviceFramePitch(), demuxer.GetWidth(), demuxer.GetHeight(), textureMap, false, false, false);

                    ResizeNv12WithTexture((unsigned char*)encoderInputFrame->inputPtr, (int)encoderInputFrame->pitch,
                        vEncoders[encId]->GetEncodeWidth(), vEncoders[encId]->GetEncodeHeight(),
                        texObj, demuxer.GetWidth(), demuxer.GetHeight(), nullptr, inpStreams[encId]);
                }
            }

            push_back(encQueue, task);
            startEncode[encId].store(true);
        }

        for (auto& pair : textureMap)
        {
            DestroyTextureObjects(pair.second);
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[encId][2] = std::current_exception();

        for (auto& pair : textureMap)
        {
            DestroyTextureObjects(pair.second);
        }
    }
}

void AppTransOneToN::ProcessEncodeTask(ConcurrentQueue<TransOneToNTask>& encQueue, ConcurrentQueue<TransOneToNTask>& outputQueue, int encId)
{
    try
    {
        SetThreadName("EncodeThread", encId);
        WaitForStart(startEncode[encId]);

        while (isRunning())
        {
            TransOneToNTask task = pop_front(encQueue);

            if (task.endTask)
            {
                vEncoders[encId]->EndEncode();
                push_back(outputQueue, task);
                eos[encId] = true;
                break;
            }

            NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
            vEncoders[encId]->EncodeFrame(&picParams);

            push_back(outputQueue, task);

            startGetOutput[encId].store(true);
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[encId][3] = std::current_exception();
    }
}

void AppTransOneToN::ProcessOutputTask(ConcurrentQueue<TransOneToNTask>& outputQueue, ConcurrentQueue<TransOneToNTask>& compQueue, int encId)
{
    try
    {
        SetThreadName("OutputThread", encId);
        WaitForStart(startGetOutput[encId]);

        TransOneToNTask task;
        int delay = vEncoders[encId]->GetMinOutputDelay();
        bool endTask = false;

        while (!eos[encId].load() && isRunning())
        {
            if (outputQueue.size() > delay)
            {
                task = pop_front(outputQueue);

                if (task.endTask)
                {
                    push_back(compQueue, task);
                    double sessionTime = sessionStopWatches[encId].Stop();
                    double fps = (task.frameNum > 0) ? task.frameNum / sessionTime : 0.0;
                    std::cout << "Encoder " << encId << " Session FPS=" << fps << " (frames=" << task.frameNum << ", time=" << sessionTime << "s)" << std::endl;
                    endTask = true;
                    break;
                }

                WriteOutput(task, encId);
                push_back(compQueue, task);
            }
        }

        while (isRunning() && !endTask)
        {
            if (outputQueue.size() > 0)
            {
                task = pop_front(outputQueue);
                if (task.endTask)
                {
                    push_back(compQueue, task);
                    double sessionTime = sessionStopWatches[encId].Stop();
                    double fps = (task.frameNum > 0) ? task.frameNum / sessionTime : 0.0;
                    std::cout << "Encoder " << encId << " Session FPS=" << fps << " (frames=" << task.frameNum << ", time=" << sessionTime << "s)" << std::endl;
                    break;
                }
                WriteOutput(task, encId);
                push_back(compQueue, task);
            }
        }
    }
    catch (...)
    {
        error = true;
        vExceptionPtrs[encId][4] = std::current_exception();
    }
}

void AppTransOneToN::WriteOutput(TransOneToNTask& task, int encId)
{
    std::ofstream& fpOut = outputFiles[encId];

    NvEncOutputFrame outputFrame;
    vEncoders[encId]->GetLockBitstream(task.frameNum, vEncoders[encId]->GetOutputBfr(task.frameNum), outputFrame);
    fpOut.write(reinterpret_cast<const char*>(outputFrame.frame.data()), static_cast<unsigned int>(outputFrame.frame.size()));
    vEncoders[encId]->UnLockBitstream(task.frameNum, vEncoders[encId]->GetOutputBfr(task.frameNum), true);
}

void AppTransOneToN::ExecutePipeline(int& totFrames)
{
    std::vector<TransOneToNTask> compTask(vEncoders.size());

    while (isRunning())
    {
        for (int encId = 0; encId < vEncoders.size(); encId++)
        {
            compTask[encId] = pop_front(*completionQueues[encId]);
        }

        if (!compTask[0].endTask)
        {
            int frameNum_ref = compTask[0].frameNum;
            for (int i = 1; i < vEncoders.size(); ++i)
            {
                assert(compTask[i].frameNum == frameNum_ref && "All compTask[i].frameNum should be the same");
            }

            TransOneToNTask decTask = { 0 };
            decTask.unlockDecFrame = compTask[0].unlockDecFrame;
            push_back(decodeQueue, decTask);
        }
        else
        {
            // Clean up any remaining frames in the decode queue
            while (decodeQueue->size() > 0)
            {
                TransOneToNTask task = pop_front(*decodeQueue);

                if (task.unlockDecFrame)
                {
                    decoder.UnlockFrame(&task.unlockDecFrame);
                }
            }

            totFrames = compTask[0].frameNum;
            break;
        }
    }

    decodeThread.join();

    for (auto& thread : cudaThreads)
    {
        thread.join();
    }

    for (auto& thread : encodeThreads)
    {
        thread.join();
    }

    for (auto& thread : outputThreads)
    {
        thread.join();
    }

    CheckForExceptions();
}

static void ShowBriefHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA 1:N Video Transcoder Sample Application\n";
    oss << "========================================\n\n";
    
    oss << "Usage: AppTransOneToN -i <input_file> [options]\n\n";

    // Brief table of core arguments
    oss << "Common Arguments:\n";
    oss << std::left << std::setw(25) << "Argument" 
        << std::setw(12) << "Type"
        << "Default Value\n";
    oss << std::string(50, '-') << "\n";

    oss << std::left << std::setw(25) << "-i <path>" 
        << std::setw(12) << "Required"
        << "N/A\n";
    oss << std::left << std::setw(25) << "-o <prefix>" 
        << std::setw(12) << "Optional"
        << "out\n";
    oss << std::left << std::setw(25) << "-r <WxH>" 
        << std::setw(12) << "Optional"
        << "1280x720,800x480\n";
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
    oss << "NVIDIA 1:N Video Transcoder Sample Application\n";
    oss << "========================================\n\n";
    
    oss << "Usage: AppTransOneToN -i <input_file> [options]\n\n";

    // Detailed descriptions
    oss << "Basic Options:\n";
    oss << std::left << std::setw(25) << "-i <path>" << ": Input video file to transcode\n";
    oss << std::left << std::setw(25) << "-o <prefix>" << ": Output filename prefix\n";
    oss << std::left << std::setw(25) << "-r <WxH>" << ": Output resolutions (e.g. 1280x720)\n";
    oss << std::left << std::setw(25) << "-gpu <n>" << ": GPU device ordinal\n";
    oss << std::left << std::setw(25) << "-h/--help" << ": Print basic usage information\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print detailed usage information\n";

    // Important notes
    oss << "\nNotes:\n";
    oss << "------\n";
    oss << "* Multiple output resolutions can be specified (e.g. -r 1280x720 800x480)\n";
    oss << "* Default resolutions: 1280x720 and 800x480\n";
    oss << "* Each output stream is encoded in a separate thread\n";
    oss << std::endl;

    // Encoder Parameters
    oss << "Encoder Parameters:\n";
    oss << NvEncoderInitParam().GetHelpMessage(false, false, true, false, false, false, false, true);
    
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

void ParseCommandLine(int argc, char* argv[], char* szInputFileName, char* szOutputFileName,
    std::vector<int2>& vResolution, int& iGpu, NvEncoderInitParam& initParam)
{
    std::ostringstream oss;

    if (argc == 1) {
        std::cout << "No Arguments provided! Please refer to the following for options:\n";
        ShowBriefHelp();
    }

    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help")) {
            ShowBriefHelp();
        }
        if (!_stricmp(argv[i], "-A") || !_stricmp(argv[i], "--advanced-options")) {
            ShowHelpAdvanced();
        }
        if (!_stricmp(argv[i], "-i")) {
            if (++i == argc) {
                ShowHelpAndExit("-i");
            }
            sprintf(szInputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-o")) {
            if (++i == argc) {
                ShowHelpAndExit("-o");
            }
            sprintf(szOutputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-r")) {
            int w, h;
            if (++i == argc || 2 != sscanf(argv[i], "%dx%d", &w, &h)) {
                ShowHelpAndExit("-r");
            }
            vResolution.push_back(make_int2(w, h));
            while (++i != argc && 2 == sscanf(argv[i], "%dx%d", &w, &h)) {
                vResolution.push_back(make_int2(w, h));
            }
            if (i != argc) {
                i--;
            }
            continue;
        }
        if (!_stricmp(argv[i], "-gpu")) {
            if (++i == argc) {
                ShowHelpAndExit("-gpu");
            }
            iGpu = atoi(argv[i]);
            continue;
        }
        // Regard as encoder parameter
        if (argv[i][0] != '-') {
            ShowHelpAndExit(argv[i]);
        }
        oss << argv[i] << " ";
        while (i + 1 < argc && argv[i + 1][0] != '-') {
            oss << argv[++i] << " ";
        }
    }
    initParam = NvEncoderInitParam(oss.str().c_str());
    // fill default values
    if (vResolution.empty()) {
        vResolution.push_back(make_int2(1280, 720));
        vResolution.push_back(make_int2(800, 480));
    }
}

int main(int argc, char* argv[])
{
    int iGpu = 0;
    char szInFilePath[260] = "";
    char szOutFileNamePrefix[260] = "out";
    std::vector<int2> vResolution;
    std::vector<std::vector<std::exception_ptr>> vExceptionPtrs;
    try
    {
        auto EncodeDeleteFunc = [](NvEncoderCuda* pEnc)
        {
            if (pEnc)
            {
                pEnc->DestroyEncoder();
                delete pEnc;
            }
        };
        std::vector<NvEncCudaPtr> vEncoders;

        NvEncoderInitParam encodeCLIOptions;
        ParseCommandLine(argc, argv, szInFilePath, szOutFileNamePrefix, vResolution, iGpu, encodeCLIOptions);

        CheckInputFile(szInFilePath);

        std::cout
            << "Input file             : " << szInFilePath << std::endl
            << "Output file name pefix : " << szOutFileNamePrefix << std::endl
            << "Output resolutions     : ";
        for (int2 xy : vResolution) {
            std::cout << xy.x << "x" << xy.y << " ";
        }
        std::cout << std::endl
            << "GPU ordinal        : " << iGpu << std::endl;

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
        std::cout << "GPU in use         : " << szDeviceName << std::endl;
        CUcontext cuContext = NULL;
        auto t0 = std::chrono::high_resolution_clock::now();
        ck(NVCODEC_CUDA_CTX_CREATE(&cuContext, 0, cuDevice));

        FFmpegDemuxer demuxer(szInFilePath);

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

        encodeCLIOptions.setTransOneToN(true);
        int nEnc = (int)vResolution.size();
        vExceptionPtrs.resize(nEnc, std::vector<std::exception_ptr>(5, nullptr));

        // Determine buffer format based on input chroma format
        NV_ENC_BUFFER_FORMAT eBufferFormat;
        if (bInput444)
        {
            eBufferFormat = (demuxer.GetBitDepth() > 8) ? NV_ENC_BUFFER_FORMAT_YUV444_10BIT : NV_ENC_BUFFER_FORMAT_YUV444;
        }
        else if (bInput422)
        {
            eBufferFormat = (demuxer.GetBitDepth() > 8) ? NV_ENC_BUFFER_FORMAT_P210 : NV_ENC_BUFFER_FORMAT_NV16;
        }
        else
        {
            eBufferFormat = (demuxer.GetBitDepth() == 8) ? NV_ENC_BUFFER_FORMAT_NV12 : NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
        }

        for (int i = 0; i < nEnc; i++)
        {
            NvEncCudaPtr encPtr(new NvEncoderCuda(cuContext, vResolution[i].x, vResolution[i].y,
                eBufferFormat, MIN_QUEUE_SIZE), EncodeDeleteFunc);
            vEncoders.push_back(std::move(encPtr));

            NV_ENC_INITIALIZE_PARAMS initializeParams = { NV_ENC_INITIALIZE_PARAMS_VER };
            NV_ENC_CONFIG encodeConfig = { NV_ENC_CONFIG_VER };
            initializeParams.encodeConfig = &encodeConfig;
            vEncoders[i]->CreateDefaultEncoderParams(&initializeParams, encodeCLIOptions.GetEncodeGUID(), encodeCLIOptions.GetPresetGUID(), encodeCLIOptions.GetTuningInfo());

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

            vEncoders[i]->CreateEncoder(&initializeParams);
        }

        NvDecoder dec(cuContext, true, FFmpeg2NvCodecId(demuxer.GetVideoCodec()), false, true);

        AppTransOneToN oneToNTranscoder(demuxer, dec, cuContext, (demuxer.GetBitDepth() > 8 ? true : false), bInput422, bInput444, szOutFileNamePrefix,
            encodeCLIOptions.IsCodecH264() ? (char*)"h264" : encodeCLIOptions.IsCodecHEVC() ? (char*)"hevc" : (char*)"av1",
            vEncoders, vExceptionPtrs);

        int nFrameTrans = 0;
        oneToNTranscoder.CreateTranscodePipeline();
        oneToNTranscoder.ExecutePipeline(nFrameTrans);
        
        auto t1 = std::chrono::high_resolution_clock::now();
        auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(t1.time_since_epoch() - t0.time_since_epoch()).count();
        std::cout << "Frames transcoded: " << nFrameTrans << " x " << nEnc << ", FPS=" << ((nFrameTrans * nEnc * 1000) / msec) << std::endl;
    }
    catch (const std::exception& ex)
    {
        std::cout << ex.what();
        exit(1);
    }
    return 0;
}

