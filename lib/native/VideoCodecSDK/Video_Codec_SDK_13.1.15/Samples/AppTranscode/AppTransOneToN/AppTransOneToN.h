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

// Thread name buffer size constants
#define THREAD_NAME_MAX 32
#define LINUX_THREAD_NAME_MAX 16

#include <cuda_runtime.h>
#include <stdio.h>
#include <iostream>
#include <thread>
#include <algorithm>
#include <chrono>
#include <string.h>
#include <memory>
#include <numeric>
#include <atomic>
#include <fstream>
#include <vector>
#include <unordered_map>
#include "NvEncoder/NvEncoderCuda.h"
#include "NvDecoder/NvDecoder.h"
#include "../Utils/NvEncoderCLIOptions.h"
#include "../Utils/NvCodecUtils.h"
#include "../Utils/FFmpegDemuxer.h"
#include <cassert>

class FFmpegDemuxer;
class FFmpegMuxer;
class NvDecoder;
class NvEncoderCuda;

#define MIN_QUEUE_SIZE 8 //Higher value of queue size will give better perf

using NvEncCudaPtr = std::unique_ptr<NvEncoderCuda, std::function<void(NvEncoderCuda*)>>;

struct Packet {
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int streamIndex;
};

struct TransOneToNTask {
    uint8_t* decodedFrame;
    uint8_t* unlockDecFrame;
    int frameNum;
    bool endTask;
    Packet videoPkt;
};

class AppTransOneToN 
{
private:
    FFmpegDemuxer& demuxer;
    NvDecoder& decoder;
    std::vector<NvEncCudaPtr>& vEncoders;
    CUcontext cuContext;
    bool bOut10;
    bool bInput422;
    bool bInput444;
    
    std::vector<std::vector<std::exception_ptr>>& vExceptionPtrs;
    
    char *szOutFileNamePrefix = NULL;
    char* szOutFileNameSuffix = NULL;
    
    int queueSize;
    
    std::vector<std::atomic<bool>> startCuda;
    std::vector<std::atomic<bool>> startEncode;
    std::vector<std::atomic<bool>> startGetOutput;
    std::vector<std::atomic<int>> eos;
    std::atomic<int> error{ 0 };

    std::vector<CUstream> inpStreams;
    
    std::vector<StopWatch> sessionStopWatches;
    
    std::vector<std::ofstream> outputFiles;

    std::unique_ptr<ConcurrentQueue<TransOneToNTask>> decodeQueue;
    std::vector<std::unique_ptr<ConcurrentQueue<TransOneToNTask>>> cudaProcessingQueues;
    std::vector<std::unique_ptr<ConcurrentQueue<TransOneToNTask>>> encodeQueues;
    std::vector<std::unique_ptr<ConcurrentQueue<TransOneToNTask>>> outputQueues;
    std::vector<std::unique_ptr<ConcurrentQueue<TransOneToNTask>>> completionQueues;

    std::thread decodeThread;
    std::vector<std::thread> cudaThreads;
    std::vector<std::thread> encodeThreads;
    std::vector<std::thread> outputThreads;

    // Inline helper functions
    inline bool isRunning() const { return !error.load(); }
    
    inline void WaitForStart(const std::atomic<bool>& startFlag) 
    {
        while (!startFlag.load() && isRunning()) {
            std::this_thread::yield();
        }
    }
    
    inline TransOneToNTask pop_front(ConcurrentQueue<TransOneToNTask>& queue)
    {
        TransOneToNTask task;
        while (!queue.try_pop_front(task) && isRunning())
        {
            std::this_thread::yield();
        }
        return task;
    }
    
    inline void push_back(ConcurrentQueue<TransOneToNTask>& queue, const TransOneToNTask& value) 
    {
        while (!queue.try_push_back(value) && isRunning())
        {
            std::this_thread::yield();
        }
    }
    
    inline void push_back(std::unique_ptr<ConcurrentQueue<TransOneToNTask>>& queue, const TransOneToNTask& value) 
    {
        while (!queue->try_push_back(value) && isRunning())
        {
            std::this_thread::yield();
        }
    }
    
    inline void CheckForExceptions()
    {
        for (int encId = 0; encId < vEncoders.size(); encId++) 
        {
            for (int threadId = 0; threadId < vExceptionPtrs[encId].size(); threadId++) 
            {
                if (vExceptionPtrs[encId][threadId]) 
                {
                    std::cout << "Exception occurred in encoder " << encId << ", thread " << threadId << std::endl;
                    std::rethrow_exception(vExceptionPtrs[encId][threadId]);
                }
            }
        }
    }
    
    inline void SetThreadName(const char* threadType, int encId = -1) 
    {
        char threadName[THREAD_NAME_MAX];
        if (encId >= 0) {
            snprintf(threadName, THREAD_NAME_MAX, "%s_%d", threadType, encId);
        } else {
            snprintf(threadName, THREAD_NAME_MAX, "%s", threadType);
        }
        
    #ifdef _WIN32
        wchar_t wThreadName[THREAD_NAME_MAX];
        MultiByteToWideChar(CP_UTF8, 0, threadName, -1, wThreadName, THREAD_NAME_MAX);
        SetThreadDescription(GetCurrentThread(), wThreadName);
    #else
        // Linux pthread_setname_np has 16-char limit (15 chars + null terminator)
        char linuxThreadName[LINUX_THREAD_NAME_MAX];
        strncpy(linuxThreadName, threadName, LINUX_THREAD_NAME_MAX - 1);
        linuxThreadName[LINUX_THREAD_NAME_MAX - 1] = '\0';  // Ensure null termination
        pthread_setname_np(pthread_self(), linuxThreadName);
    #endif
    }

    void ProcessDecodeTask(ConcurrentQueue<TransOneToNTask>& decQueue, std::vector<std::unique_ptr<ConcurrentQueue<TransOneToNTask>>>& cudaProcQueues);
    void ProcessCudaTask(ConcurrentQueue<TransOneToNTask>& cudaProcQueue, ConcurrentQueue<TransOneToNTask>& encQueue, int encId);
    void ProcessEncodeTask(ConcurrentQueue<TransOneToNTask>& encQueue, ConcurrentQueue<TransOneToNTask>& outputQueue, int encId);
    void ProcessOutputTask(ConcurrentQueue<TransOneToNTask>& outputQueue, ConcurrentQueue<TransOneToNTask>& compQueue, int encId);
    void WriteOutput(TransOneToNTask& task, int encId);

public: 
    AppTransOneToN(FFmpegDemuxer& demux, NvDecoder& dec, CUcontext ctx, 
                      bool out10, bool input422, bool input444, char* prefix, char* suffix,
                      std::vector<NvEncCudaPtr>& encoders,
                      std::vector<std::vector<std::exception_ptr>>& exceptionPtrs);
    ~AppTransOneToN();

    void CreateTranscodePipeline();
    void ExecutePipeline(int& totFrames);
};

