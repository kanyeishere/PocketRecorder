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

#ifdef _WIN32
#include <windows.h>
#endif

#include <cuda.h>
#include <memory>
#include <vector>
#include <fstream>
#include <thread>
#include <atomic>
#include <exception>

#include "nvEncodeAPI.h"
#include "nvcuvid.h"
#include "FFmpegDemuxer.h"
#include "FFmpegMuxer.h"
#include "NvCodecUtils.h"
#include "Logger.h"
#include "NvEncoderCLIOptions.h"
#include "NvDecoder/NvDecoderZeroCopy.h"
#include "NvEncoder/NvEncoderZeroCopy.h"
#include "ZeroCopyUtils.h"

#define ZC_THREAD_NAME_MAX 32
#define ZC_LINUX_THREAD_NAME_MAX 16

#ifndef _WIN32
#include <pthread.h>
#endif

#define CK_CUDA(call) \
    do { \
        CUresult err = call; \
        if (err != CUDA_SUCCESS) { \
            const char* szErrName = nullptr; \
            cuGetErrorName(err, &szErrName); \
            std::cerr << "CUDA error: " << (szErrName ? szErrName : "Unknown") \
                      << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            exit(1); \
        } \
    } while (0)

struct AppConfig
{
    char szInputFile[256] = "";
    char szOutputFile[256] = "";
    int nOutBitDepth = 0;
    int iGpu = 0;
    mutable NvEncoderInitParam encoderParams;
    bool verbose = false;
    int nThread = 1;
    bool bSingleContext = false;
    bool bPerfMode = false;
};

struct NonVideoPacket
{
    std::vector<uint8_t> data;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int streamIndex;
    int nBytes;
};

struct ZeroCopyTask
{
    int arrayIdx;
    int frameNum;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int streamIndex;
    bool endTask;
    bool isFlush;
    std::vector<NonVideoPacket> nonVidPkt;
    std::vector<uint8_t> bitstream;
    NV_ENC_PIC_TYPE pictureType;
    int64_t outputTimestamp;

    ZeroCopyTask()
        : arrayIdx(-1)
        , frameNum(0)
        , pts(0)
        , dts(0)
        , duration(0)
        , streamIndex(0)
        , endTask(false)
        , isFlush(false)
        , pictureType(NV_ENC_PIC_TYPE_P)
        , outputTimestamp(0)
    {}
};

class ZeroCopyTranscoder
{
public:
    ZeroCopyTranscoder(CUcontext cuContext, FFmpegDemuxer& demuxer, const AppConfig& config);
    ~ZeroCopyTranscoder();
    void Transcode(const char* szOutputFile);
    uint32_t GetFrameCount() const { return m_nFrameCount; }

private:
    void InitializeDecoder();
    void InitializeEncoder(const AppConfig& config);
    void AllocateSharedBuffers();
    void RegisterBuffersWithCodecs();
    void CreateTranscodePipeline();
    void ExecutePipeline(int& totFrames);
    void ProcessDecodeTask(ConcurrentQueue<ZeroCopyTask>& decQueue, ConcurrentQueue<ZeroCopyTask>& encQueue);
    void ProcessEncodeTask(ConcurrentQueue<ZeroCopyTask>& encQueue, ConcurrentQueue<ZeroCopyTask>& outQueue);
    void ProcessOutputTask(ConcurrentQueue<ZeroCopyTask>& outQueue, ConcurrentQueue<ZeroCopyTask>& compQueue);
    void WriteOutput(ZeroCopyTask& task);
    inline bool isRunning() const { return !m_error.load(); }

    inline void WaitForStart(const std::atomic<bool>& startFlag)
    {
        while (!startFlag.load() && isRunning())
        {
            std::this_thread::yield();
        }
    }
    
    inline ZeroCopyTask PopFront(ConcurrentQueue<ZeroCopyTask>& queue)
    {
        ZeroCopyTask task;
        while (!queue.try_pop_front(task) && isRunning())
        {
            std::this_thread::yield();
        }
        return task;
    }
    
    inline void PushBack(ConcurrentQueue<ZeroCopyTask>& queue, const ZeroCopyTask& value)
    {
        while (!queue.try_push_back(value) && isRunning())
        {
            std::this_thread::yield();
        }
    }
    
    inline void CheckForExceptions()
    {
        for (size_t i = 0; i < m_vExceptionPtrs.size(); ++i)
        {
            if (m_vExceptionPtrs[i])
            {
                std::rethrow_exception(m_vExceptionPtrs[i]);
            }
        }
    }
    
    inline void SetThreadName(const char* threadType)
    {
        char threadName[ZC_THREAD_NAME_MAX];
        snprintf(threadName, ZC_THREAD_NAME_MAX, "%s", threadType);
        
#ifdef _WIN32
        wchar_t wThreadName[ZC_THREAD_NAME_MAX];
        MultiByteToWideChar(CP_UTF8, 0, threadName, -1, wThreadName, ZC_THREAD_NAME_MAX);
        SetThreadDescription(GetCurrentThread(), wThreadName);
#else
        char linuxThreadName[ZC_LINUX_THREAD_NAME_MAX];
        strncpy(linuxThreadName, threadName, ZC_LINUX_THREAD_NAME_MAX - 1);
        linuxThreadName[ZC_LINUX_THREAD_NAME_MAX - 1] = '\0';
        pthread_setname_np(pthread_self(), linuxThreadName);
#endif
    }

private:
    CUcontext m_cuContext;
    CUstream m_cuStream;
    FFmpegDemuxer& m_demuxer;
    std::unique_ptr<NvDecoderZeroCopy> m_pDecoder;
    std::unique_ptr<NvEncoderZeroCopy> m_pEncoder;
    ZeroCopyBufferPool m_bufferPool;
    std::unique_ptr<FFmpegMuxer> m_pMuxer;
    std::unique_ptr<std::ofstream> m_fpOut;
    MEDIA_FORMAT m_mediaFormat;
    uint32_t m_nWidth;
    uint32_t m_nHeight;
    cudaVideoCodec m_decCodec;
    GUID m_encCodecGuid;
    NV_ENC_BUFFER_FORMAT m_bufferFormat;
    int m_chromaFormatIDC;
    std::thread m_decodeThread;
    std::thread m_encodeThread;
    std::thread m_outputThread;
    uint32_t m_queueSize;
    ConcurrentQueue<ZeroCopyTask> m_decodeQueue;
    ConcurrentQueue<ZeroCopyTask> m_encodeSubmitQueue;
    ConcurrentQueue<ZeroCopyTask> m_outputQueue;
    ConcurrentQueue<ZeroCopyTask> m_completionQueue;
    std::atomic<bool> m_startEncode{false};
    std::atomic<bool> m_startOutput{false};
    std::atomic<int> m_eos{0};
    std::atomic<int> m_error{0};
    std::vector<std::exception_ptr> m_vExceptionPtrs;
    bool m_bUseIVFContainer;
    bool m_bWriteIVFFileHeader;
    int64_t m_frameDtsCounter;
    int64_t m_frameDuration;
    uint32_t m_nFrameCount;
    bool m_bVerbose;
};

void PrintUsage();
bool ParseCommandLine(int argc, char* argv[], AppConfig& config);

