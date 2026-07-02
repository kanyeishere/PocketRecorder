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

#pragma once

#define THREAD_NAME_MAX 32
#define LINUX_THREAD_NAME_MAX 16

#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <iostream>
#include <thread>
#include <string>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cuda.h>
#include <cuda_runtime.h>
#include "NvEncoder/NvEncoderCuda.h"
#include "NvDecoder/NvDecoder.h"
#include "../Utils/FFmpegDemuxer.h"
#include "../Utils/NvCodecUtils.h"
#include "../Utils/Logger.h"
#include "../Utils/NvEncoderCLIOptions.h"

class FFmpegDemuxer;
class NvDecoder;
class NvEncoderCuda;

#define MIN_QUEUE_SIZE 3 //Higher value of queue size will give better perf

struct PerfPacket {
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int streamIndex;
};

struct TranscodeTask {
    uint8_t* decodedFrame;
    uint8_t* unlockDecFrame;
    int frameNum;
    bool endTask;
    PerfPacket videoPkt;
};

using NvEncCudaPtr = std::unique_ptr<NvEncoderCuda, std::function<void(NvEncoderCuda*)>>;

class AppTransPerf 
{
private:
    NvEncCudaPtr pEnc;
    FFmpegDemuxer& demuxer;
    NvDecoder& decoder;
    CUcontext cuContext;
    CUstream encStream;
    int queueSize;
    std::atomic<int> eos{ 0 };
    std::atomic<int> error{ 0 };
    std::atomic<int> processedFrames{ 0 };

    std::atomic<bool> startCuda{ false };
    std::atomic<bool> startEncode{ false };
    std::atomic<bool> startOutput{ false };

    std::vector<std::exception_ptr> vExceptionPtrs;

    ConcurrentQueue<TranscodeTask> decodeQueue;
    ConcurrentQueue<TranscodeTask> cudaProcessingQueue;
    ConcurrentQueue<TranscodeTask> encodeQueue;
    ConcurrentQueue<TranscodeTask> outputQueue;
    ConcurrentQueue<TranscodeTask> completionQueue;
    
    std::thread decodeThread;
    std::thread cudaThread;
    std::thread encodeThread;
    std::thread outputThread;

    inline bool isRunning() const { return !error.load(); }
    
    inline void WaitForStart(const std::atomic<bool>& startFlag) 
    {
        while (!startFlag.load() && isRunning()) 
        {
            std::this_thread::yield();
        }
    }
    
    inline TranscodeTask pop_front(ConcurrentQueue<TranscodeTask>& queue)
    {
        TranscodeTask task;
        while (!queue.try_pop_front(task) && isRunning())
        {
            std::this_thread::yield();
        }
        return task;
    }
    
    inline void push_back(ConcurrentQueue<TranscodeTask>& queue, const TranscodeTask& value) 
    {
        while (!queue.try_push_back(value) && isRunning())
        {
            std::this_thread::yield();
        }
    }
    
    inline void CheckForExceptions()
    {
        for (size_t i = 0; i < vExceptionPtrs.size(); ++i)
        {
            if (vExceptionPtrs[i])
            {
                std::rethrow_exception(vExceptionPtrs[i]);
            }
        }
    }
    
    inline void SetThreadName(const char* threadType) 
    {
        char threadName[THREAD_NAME_MAX];
        snprintf(threadName, THREAD_NAME_MAX, "%s", threadType);
        
    #ifdef _WIN32
        wchar_t wThreadName[THREAD_NAME_MAX];
        MultiByteToWideChar(CP_UTF8, 0, threadName, -1, wThreadName, THREAD_NAME_MAX);
        SetThreadDescription(GetCurrentThread(), wThreadName);
    #else
        char linuxThreadName[LINUX_THREAD_NAME_MAX];
        strncpy(linuxThreadName, threadName, LINUX_THREAD_NAME_MAX - 1);
        linuxThreadName[LINUX_THREAD_NAME_MAX - 1] = '\0';
        pthread_setname_np(pthread_self(), linuxThreadName);
    #endif
    }

    void ProcessDecodeTask(ConcurrentQueue<TranscodeTask>& decQueue, ConcurrentQueue<TranscodeTask>& cudaProcQueue);
    void ProcessCudaTask(ConcurrentQueue<TranscodeTask>& cudaProcQueue, ConcurrentQueue<TranscodeTask>& encQueue);
    void ProcessEncodeTask(ConcurrentQueue<TranscodeTask>& encQueue, ConcurrentQueue<TranscodeTask>& outputQueue);
    void ProcessOutputTask(ConcurrentQueue<TranscodeTask>& outputQueue, ConcurrentQueue<TranscodeTask>& completionQueue);

public: 
    AppTransPerf(NvEncCudaPtr& enc, FFmpegDemuxer& demux, NvDecoder& dec, CUcontext ctx);
    ~AppTransPerf();

    void CreateTranscodePipeline();
    int ExecutePipeline();
    int GetProcessedFrames() const { return processedFrames.load(); }
};
