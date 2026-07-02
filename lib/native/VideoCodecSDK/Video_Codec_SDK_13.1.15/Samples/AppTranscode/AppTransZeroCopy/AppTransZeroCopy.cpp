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

#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include "AppTransZeroCopy.h"

simplelogger::Logger* logger = simplelogger::LoggerFactory::CreateConsoleLogger();

static void ShowBriefHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA Zero-Copy Video Transcoder Sample Application\n";
    oss << "=====================================================\n\n";
    
    oss << "Usage: AppTransZeroCopy -i <input_file> [options]\n\n";

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
    oss << std::left << std::setw(25) << "-gpu <n>" 
        << std::setw(12) << "Optional"
        << "0\n";
    oss << std::left << std::setw(25) << "-thread <n>" 
        << std::setw(12) << "Optional"
        << "1\n";
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
    oss << "NVIDIA Zero-Copy Video Transcoder Sample Application\n";
    oss << "=====================================================\n\n";
    
    oss << "Usage: AppTransZeroCopy -i <input_file> [options]\n\n";

    oss << "Basic Options:\n";
    oss << std::left << std::setw(25) << "-i <path>" << ": Input video file to transcode\n";
    oss << std::left << std::setw(25) << "-o <path>" << ": Output file (.mp4/.mov for containers)\n";
    oss << std::left << std::setw(25) << "-ob <depth>" << ": Output bit depth (8 or 10)\n";
    oss << std::left << std::setw(25) << "-gpu <n>" << ": GPU device ordinal\n";
    oss << std::left << std::setw(25) << "-thread <n>" << ": Number of transcoding sessions (perf mode)\n";
    oss << std::left << std::setw(25) << "-single" << ": Use single CUDA context\n";
    oss << std::left << std::setw(25) << "-v" << ": Verbose output\n";
    oss << std::left << std::setw(25) << "-h/--help" << ": Print basic usage information\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print detailed usage information\n";

    oss << "\nNotes:\n";
    oss << "------\n";
    oss << "* Zero-copy: decoder outputs directly to CUDA arrays registered with encoder\n";
    oss << "* Supports container output: MP4, MOV, MKV, WebM (with audio passthrough)\n";
    oss << "* Limitations: no bit-depth conversion, no lookahead\n";
    oss << std::endl;

    oss << "Encoder Parameters:\n";
    oss << NvEncoderInitParam().GetHelpMessage(false, false, true);
    
    std::cout << oss.str();
    exit(0);
}

void ShowHelpAndExit(const char* szBadOption = nullptr)
{
    if (szBadOption)
    {
        std::ostringstream oss;
        oss << "Error parsing \"" << szBadOption << "\"\n";
        oss << "Use -h/--help for basic usage or -A/--advanced-options for detailed information\n";
        throw std::invalid_argument(oss.str());
    }
}

void PrintUsage()
{
    ShowBriefHelp();
}

bool ParseCommandLine(int argc, char* argv[], AppConfig& config)
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
            strncpy(config.szInputFile, argv[i], sizeof(config.szInputFile) - 1);
            continue;
        }
        if (!_stricmp(argv[i], "-o"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-o");
            }
            strncpy(config.szOutputFile, argv[i], sizeof(config.szOutputFile) - 1);
            continue;
        }
        if (!_stricmp(argv[i], "-ob"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-ob");
            }
            config.nOutBitDepth = atoi(argv[i]);
            if (config.nOutBitDepth != 8 && config.nOutBitDepth != 10) 
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
            config.iGpu = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-v"))
        {
            config.verbose = true;
            continue;
        }
        if (!_stricmp(argv[i], "-thread"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-thread");
            }
            config.nThread = atoi(argv[i]);
            if (config.nThread < 1)
            {
                std::cerr << "Error: -thread must be >= 1" << std::endl;
                return false;
            }
            config.bPerfMode = true;
            continue;
        }
        if (!_stricmp(argv[i], "-single"))
        {
            config.bSingleContext = true;
            continue;
        }
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
    
    config.encoderParams = NvEncoderInitParam(oss.str().c_str());

    if (config.szInputFile[0] == '\0')
    {
        std::cerr << "Error: Input file is required (-i)" << std::endl;
        return false;
    }

    if (config.szOutputFile[0] == '\0')
    {
        if (config.encoderParams.IsCodecHEVC())
            strcpy(config.szOutputFile, "out.hevc");
        else if (config.encoderParams.IsCodecAV1())
            strcpy(config.szOutputFile, "out.av1");
        else
            strcpy(config.szOutputFile, "out.h264");
    }

    return true;
}

static const char* GetCodecName(cudaVideoCodec codec)
{
    switch (codec)
    {
    case cudaVideoCodec_H264:     return "H.264";
    case cudaVideoCodec_H264_SVC: return "H.264 SVC";
    case cudaVideoCodec_H264_MVC: return "H.264 MVC";
    case cudaVideoCodec_HEVC:     return "HEVC";
    case cudaVideoCodec_VP9:      return "VP9";
    case cudaVideoCodec_AV1:      return "AV1";
    case cudaVideoCodec_MPEG1:    return "MPEG1";
    case cudaVideoCodec_MPEG2:    return "MPEG2";
    case cudaVideoCodec_MPEG4:    return "MPEG4";
    case cudaVideoCodec_VC1:      return "VC1";
    case cudaVideoCodec_VP8:      return "VP8";
    case cudaVideoCodec_JPEG:     return "JPEG";
    default:                      return "Unknown";
    }
}

ZeroCopyTranscoder::ZeroCopyTranscoder(CUcontext cuContext, FFmpegDemuxer& demuxer,
                                       const AppConfig& config)
    : m_cuContext(cuContext)
    , m_cuStream(nullptr)
    , m_demuxer(demuxer)
    , m_nWidth(demuxer.GetWidth())
    , m_nHeight(demuxer.GetHeight())
    , m_decCodec(FFmpeg2NvCodecId(demuxer.GetVideoCodec()))
    , m_encCodecGuid(config.encoderParams.GetEncodeGUID())
    , m_mediaFormat(MEDIA_FORMAT_ELEMENTARY)
    , m_chromaFormatIDC(1)
    , m_queueSize(0)
    , m_vExceptionPtrs(4, nullptr)
    , m_bUseIVFContainer(false)
    , m_bWriteIVFFileHeader(false)
    , m_frameDtsCounter(0)
    , m_frameDuration(0)
    , m_nFrameCount(0)
    , m_bVerbose(config.verbose)
{
    bool bOut10 = config.nOutBitDepth ? config.nOutBitDepth > 8 : demuxer.GetBitDepth() > 8;
    
    AVPixelFormat chromaFmt = demuxer.GetChromaFormat();
    bool bInput422 = (chromaFmt == AV_PIX_FMT_YUV422P ||
                      chromaFmt == AV_PIX_FMT_YUV422P10LE ||
                      chromaFmt == AV_PIX_FMT_YUV422P12LE);
    bool bInput444 = (chromaFmt == AV_PIX_FMT_YUV444P ||
                      chromaFmt == AV_PIX_FMT_YUV444P10LE ||
                      chromaFmt == AV_PIX_FMT_YUV444P12LE);
    
    if (bInput444)
    {
        std::cerr << "\nERROR: YUV444 chroma format is not supported in zero-copy mode.\n"
                  << "       Please use a 4:2:0 or 4:2:2 input instead.\n" << std::endl;
        throw std::runtime_error("Unsupported chroma format: YUV444");
    }
    if (bInput422)
    {
        m_chromaFormatIDC = 2;
        m_bufferFormat = bOut10 ? NV_ENC_BUFFER_FORMAT_P210 : NV_ENC_BUFFER_FORMAT_NV16;
    }
    else
    {
        m_chromaFormatIDC = 1;
        m_bufferFormat = bOut10 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    }
    
    char* extension = strrchr(const_cast<char*>(config.szOutputFile), '.');
    if (extension)
    {
        if (_stricmp(extension, ".mov") == 0)
            m_mediaFormat = MEDIA_FORMAT_MOV;
        else if (_stricmp(extension, ".mp4") == 0)
            m_mediaFormat = MEDIA_FORMAT_MP4;
        else if (_stricmp(extension, ".webm") == 0)
            m_mediaFormat = MEDIA_FORMAT_WEBM;
        else if (_stricmp(extension, ".mkv") == 0)
            m_mediaFormat = MEDIA_FORMAT_MKV;
        else
            m_mediaFormat = MEDIA_FORMAT_ELEMENTARY;
    }
    
    if (config.encoderParams.IsCodecAV1() && bInput422)
    {
        std::cout << "ERROR: AV1 does not support 4:2:2 chroma format." << std::endl;
        throw std::runtime_error("Unsupported chroma format for AV1");
    }
    
    if (bInput422)
    {
        std::cout << "INFO: YUV422 is supported from Blackwell (GB20x) GPUs onwards." << std::endl;
    }

    ck(cuCtxPushCurrent(m_cuContext));
    ck(cuStreamCreate(&m_cuStream, CU_STREAM_DEFAULT));

    InitializeDecoder();
    InitializeEncoder(config);
    AllocateSharedBuffers();
    RegisterBuffersWithCodecs();
    
    m_queueSize = static_cast<uint32_t>(m_bufferPool.GetCudaArrays().size());
    
    m_encodeSubmitQueue.setSize(m_queueSize);
    m_outputQueue.setSize(m_queueSize);
    m_completionQueue.setSize(m_queueSize);
    
    ck(cuCtxPopCurrent(nullptr));
}

ZeroCopyTranscoder::~ZeroCopyTranscoder()
{
    if (m_fpOut && m_fpOut->is_open())
    {
        m_fpOut->close();
    }
    
    if (m_cuContext)
    {
        cuCtxPushCurrent(m_cuContext);
        
        cuCtxSynchronize();
        
        if (m_pEncoder)
        {
            m_pEncoder.reset();
        }
        
        m_bufferPool.Release();
        
        if (m_pDecoder)
        {
            m_pDecoder.reset();
        }
        
        if (m_cuStream)
        {
            cuStreamDestroy(m_cuStream);
            m_cuStream = nullptr;
        }
        
        CUcontext dummy;
        cuCtxPopCurrent(&dummy);
    }
}

void ZeroCopyTranscoder::InitializeDecoder()
{
    unsigned int alignedHeight;
    if (m_encCodecGuid == NV_ENC_CODEC_AV1_GUID)
        alignedHeight = (m_nHeight + 63) & ~63;
    else if (m_encCodecGuid == NV_ENC_CODEC_HEVC_GUID)
        alignedHeight = (m_nHeight + 31) & ~31;
    else
        alignedHeight = (m_nHeight + 15) & ~15;
    
    m_pDecoder.reset(new NvDecoderZeroCopy(
        m_cuContext, m_decCodec, m_cuStream, m_nWidth, alignedHeight, false));
}

void ZeroCopyTranscoder::InitializeEncoder(const AppConfig& config)
{
    m_pEncoder.reset(new NvEncoderZeroCopy(
        m_cuContext, m_nWidth, m_nHeight, m_bufferFormat, m_encCodecGuid, 3));
    
    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG encConfig = { NV_ENC_CONFIG_VER };
    initParams.encodeConfig = &encConfig;
    
    m_pEncoder->GetDefaultEncoderParams(&initParams, 
                                         config.encoderParams.GetPresetGUID(),
                                         config.encoderParams.GetTuningInfo());
    
    if (m_chromaFormatIDC == 2)
    {
        if (config.encoderParams.IsCodecH264())
        {
            encConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 2;
        }
        else if (config.encoderParams.IsCodecHEVC())
        {
            encConfig.encodeCodecConfig.hevcConfig.chromaFormatIDC = 2;
        }
    }
    
    config.encoderParams.SetInitParams(&initParams, m_bufferFormat);
    
    if (config.encoderParams.GetTuningInfo() == NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY)
    {
        std::cerr << "\nERROR: Ultra High Quality (UHQ) tuning is not supported in zero-copy mode.\n"
                  << "       UHQ requires lookahead which conflicts with synchronous output retrieval.\n"
                  << "       Please use -tuninginfo hq, lowlatency, or ultralowlatency instead.\n" << std::endl;
        throw std::runtime_error("Unsupported encoder configuration: UHQ tuning");
    }
    
    if (encConfig.rcParams.enableLookahead)
    {
        std::cerr << "\nERROR: Lookahead is not supported in zero-copy mode.\n"
                  << "       Lookahead requires encoder buffering which conflicts with synchronous output.\n"
                  << "       Please remove -lookahead option or set -lookahead 0.\n" << std::endl;
        throw std::runtime_error("Unsupported encoder configuration: lookahead enabled");
    }
    
    bool inputIs10Bit = m_demuxer.GetBitDepth() > 8;
    bool outputIs10Bit = (m_bufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ||
                          m_bufferFormat == NV_ENC_BUFFER_FORMAT_P210);
    if (inputIs10Bit != outputIs10Bit)
    {
        std::cerr << "\nERROR: Bit depth conversion is not supported in zero-copy mode.\n"
                  << "       Input is " << (inputIs10Bit ? "10-bit" : "8-bit") 
                  << " but output is " << (outputIs10Bit ? "10-bit" : "8-bit") << ".\n"
                  << "       Zero-copy requires matching bit depth between input and output.\n" << std::endl;
        throw std::runtime_error("Unsupported encoder configuration: bit depth conversion");
    }
    
    m_pEncoder->CreateEncoder(&initParams);
    
    m_pEncoder->SetIOCudaStreams(&m_cuStream, &m_cuStream);
}

void ZeroCopyTranscoder::AllocateSharedBuffers()
{
    uint32_t encoderBuffers = m_pEncoder->GetRequiredBufferCount();
    
    uint32_t decoderEstimate;
    switch (m_decCodec)
    {
    case cudaVideoCodec_H264:
    case cudaVideoCodec_H264_SVC:
    case cudaVideoCodec_H264_MVC:
        decoderEstimate = 20;
        break;
    case cudaVideoCodec_HEVC:
        decoderEstimate = 20;
        break;
    case cudaVideoCodec_AV1:
        decoderEstimate = 18;
        break;
    case cudaVideoCodec_VP9:
        decoderEstimate = 12;
        break;
    default:
        decoderEstimate = 16;
        break;
    }
    
    uint32_t numBuffers = (std::max)(encoderBuffers, decoderEstimate) + 4;
    
    cudaVideoSurfaceFormat decFormat = ZeroCopyBufferPool::EncFormatToDecFormat(m_bufferFormat);
    
    bool success = m_bufferPool.Allocate(
        m_cuContext, numBuffers, m_nWidth, m_nHeight,
        decFormat, m_bufferFormat, m_encCodecGuid);
    
    if (!success)
    {
        throw std::runtime_error("Failed to allocate zero-copy buffer pool");
    }
}

void ZeroCopyTranscoder::RegisterBuffersWithCodecs()
{
    std::vector<CUarray> cudaArrays = m_bufferPool.GetCudaArrays();
    
    m_pDecoder->SetExternalOutputArrays(cudaArrays);
    
    m_pEncoder->RegisterInputArrays(
        cudaArrays,
        m_bufferPool.GetAlignedWidth(),
        m_bufferPool.GetAlignedHeight(),
        m_bufferPool.GetPitch());
}

void ZeroCopyTranscoder::Transcode(const char* szOutputFile)
{
    AVCodecID codecID = m_encCodecGuid == NV_ENC_CODEC_H264_GUID ? AV_CODEC_ID_H264 :
                        m_encCodecGuid == NV_ENC_CODEC_HEVC_GUID ? AV_CODEC_ID_HEVC :
                        AV_CODEC_ID_AV1;
    
    if (m_mediaFormat != MEDIA_FORMAT_ELEMENTARY)
    {
        std::vector<unsigned char> vSeqParams;
        m_pEncoder->GetSequenceParams(vSeqParams);
        
        m_pMuxer.reset(new FFmpegMuxer(szOutputFile, m_mediaFormat, 
                                        m_demuxer.GetAVFormatContext(),
                                        codecID, m_nWidth, m_nHeight,
                                        vSeqParams.data(), vSeqParams.size()));
    }
    else
    {
        m_fpOut.reset(new std::ofstream(szOutputFile, std::ios::out | std::ios::binary));
        if (!m_fpOut->is_open())
        {
            std::ostringstream err;
            err << "Unable to open output file: " << szOutputFile << std::endl;
            throw std::invalid_argument(err.str());
        }
    }

    m_bUseIVFContainer = (m_encCodecGuid == NV_ENC_CODEC_AV1_GUID) && 
                         (m_mediaFormat == MEDIA_FORMAT_ELEMENTARY);
    m_bWriteIVFFileHeader = m_bUseIVFContainer;
    
    m_frameDtsCounter = 0;
    m_frameDuration = 0;
    m_nFrameCount = 0;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    int totFrames = 0;
    CreateTranscodePipeline();
    ExecutePipeline(totFrames);

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    
    std::cout << "Total processing time = " << elapsedSec << " seconds, FPS=" 
              << (m_nFrameCount / elapsedSec) << " (#totFrames=" << m_nFrameCount << ")" << std::endl;
}

void ZeroCopyTranscoder::CreateTranscodePipeline()
{
    uint32_t numTokens = m_pEncoder->GetRequiredBufferCount();
    for (uint32_t i = 0; i < numTokens; i++)
    {
        ZeroCopyTask fillerTask;
        fillerTask.arrayIdx = -1;
        fillerTask.endTask = false;
        PushBack(m_completionQueue, fillerTask);
    }
    
    m_decodeThread = std::thread(&ZeroCopyTranscoder::ProcessDecodeTask, this,
                                  std::ref(m_decodeQueue),
                                  std::ref(m_encodeSubmitQueue));
    
    m_encodeThread = std::thread(&ZeroCopyTranscoder::ProcessEncodeTask, this,
                                  std::ref(m_encodeSubmitQueue),
                                  std::ref(m_outputQueue));
    
    m_outputThread = std::thread(&ZeroCopyTranscoder::ProcessOutputTask, this,
                                  std::ref(m_outputQueue), std::ref(m_completionQueue));
}

void ZeroCopyTranscoder::ExecutePipeline(int& totFrames)
{
    while (isRunning())
    {
        ZeroCopyTask compTask = PopFront(m_completionQueue);
        
        if (!compTask.endTask)
        {
            ZeroCopyTask permitTask;
            permitTask.arrayIdx = compTask.arrayIdx;
            permitTask.endTask = false;
            PushBack(m_decodeQueue, permitTask);
        }
        else
        {
            totFrames = m_nFrameCount;
            break;
        }
    }
    
    if (m_decodeThread.joinable()) m_decodeThread.join();
    if (m_encodeThread.joinable()) m_encodeThread.join();
    if (m_outputThread.joinable()) m_outputThread.join();

    CheckForExceptions();
}

void ZeroCopyTranscoder::ProcessDecodeTask(ConcurrentQueue<ZeroCopyTask>& decQueue,
                                           ConcurrentQueue<ZeroCopyTask>& encQueue)
{
    try
    {
        SetThreadName("ZC_Decode");

        ck(cuCtxSetCurrent(m_cuContext));

        uint8_t* pData = nullptr;
        int nBytes = 0;
        int64_t pts = 0, dts = 0, duration = 0;
        int isVideoPacket = 0, streamIndex = 0;
        int frameCnt = 0;
        std::vector<NonVideoPacket> nonVidPkt;

        do
        {
            if (m_mediaFormat != MEDIA_FORMAT_ELEMENTARY)
            {
                m_demuxer.Demux(&pData, &nBytes, &pts, &dts, &duration, &isVideoPacket, &streamIndex);
                
                if (!isVideoPacket && nBytes > 0)
                {
                    NonVideoPacket pkt;
                    pkt.data.resize(nBytes);
                    std::copy(pData, pData + nBytes, pkt.data.begin());
                    pkt.pts = pts;
                    pkt.dts = dts;
                    pkt.duration = duration;
                    pkt.streamIndex = streamIndex;
                    pkt.nBytes = nBytes;
                    nonVidPkt.push_back(pkt);
                    continue;
                }
            }
            else
            {
                m_demuxer.DemuxVideo(&pData, &nBytes, &pts);
                dts = pts;
                duration = 0;
                streamIndex = 0;
            }

            int nFrameReturned = m_pDecoder->Decode(pData, nBytes, 0, pts);

            for (int i = 0; i < nFrameReturned; i++)
            {
                ZeroCopyTask token = PopFront(decQueue);

                ZeroCopyDisplayInfo dispInfo;
                int arrayIdx = m_pDecoder->GetDecodedFrame(&dispInfo);
                
                if (arrayIdx < 0)
                {
                    PushBack(decQueue, token);
                    continue;
                }
                
                ZeroCopyTask task;
                if (!nonVidPkt.empty())
                {
                    task.nonVidPkt = std::move(nonVidPkt);
                    nonVidPkt.clear();
                }

                task.arrayIdx = arrayIdx;
                task.frameNum = frameCnt;
                task.pts = dispInfo.timestamp;
                task.dts = dts;
                task.duration = duration;
                task.streamIndex = streamIndex;
                task.endTask = false;
                task.isFlush = false;

                PushBack(encQueue, task);
                m_startEncode.store(true);

                frameCnt++;
            }
        } while (nBytes && isRunning());

        while (isRunning())
        {
            int nFrameReturned = m_pDecoder->Decode(nullptr, 0, 0, 0);
            if (nFrameReturned == 0)
                break;
            
            for (int i = 0; i < nFrameReturned; i++)
            {
                ZeroCopyTask token = PopFront(decQueue);
                
                if (token.arrayIdx >= 0)
                {
                    m_pDecoder->UnlockFrame(token.arrayIdx);
                }
                
                ZeroCopyDisplayInfo dispInfo;
                int arrayIdx = m_pDecoder->GetDecodedFrame(&dispInfo);
                
                if (arrayIdx < 0)
                {
                    PushBack(decQueue, token);
                    continue;
                }

                ZeroCopyTask task;
                task.arrayIdx = arrayIdx;
                task.frameNum = frameCnt;
                task.pts = dispInfo.timestamp;
                task.endTask = false;
                task.isFlush = false;

                PushBack(encQueue, task);
                frameCnt++;
            }
        }

        if (isRunning())
        {
            ZeroCopyTask token = PopFront(decQueue);
            if (token.arrayIdx >= 0)
            {
                m_pDecoder->UnlockFrame(token.arrayIdx);
            }
            
            ZeroCopyTask task;
            task.endTask = true;
            task.frameNum = frameCnt;
            task.isFlush = true;
            if (!nonVidPkt.empty())
            {
                task.nonVidPkt = std::move(nonVidPkt);
            }
            PushBack(encQueue, task);
        }
    }
    catch (...)
    {
        m_error = 1;
        m_vExceptionPtrs[0] = std::current_exception();
    }
}

void ZeroCopyTranscoder::ProcessEncodeTask(ConcurrentQueue<ZeroCopyTask>& encQueue,
                                           ConcurrentQueue<ZeroCopyTask>& outQueue)
{
    try
    {
        SetThreadName("ZC_Encode");
        WaitForStart(m_startEncode);

        ck(cuCtxSetCurrent(m_cuContext));

        bool bHasBFrames = m_pEncoder->HasBFrames();

        while (isRunning())
        {
            ZeroCopyTask task = PopFront(encQueue);

            if (task.endTask)
            {
                m_pEncoder->EndEncode();
                
                PushBack(outQueue, task);
                m_eos = 1;
                break;
            }

            NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
            if (m_mediaFormat != MEDIA_FORMAT_ELEMENTARY)
            {
                picParams.inputTimeStamp = task.pts;
            }
            
            m_pEncoder->EncodeFrame(task.arrayIdx, &picParams);

            if (bHasBFrames)
            {
                // When B-frames are configured, the driver's preprocessing step
                // has already copied the input surface to an internal buffer.
                // Release the shared CUarray immediately so the decoder can
                // reuse it - the encoder works from its internal copy.
                m_pEncoder->UnmapInputSurface(static_cast<uint32_t>(task.arrayIdx));
                m_pDecoder->UnlockFrame(task.arrayIdx);
                task.arrayIdx = -1;
            }            
            PushBack(outQueue, task);
            m_startOutput.store(true);
        }
    }
    catch (...)
    {
        m_error = 1;
        m_vExceptionPtrs[1] = std::current_exception();
    }
}

void ZeroCopyTranscoder::ProcessOutputTask(ConcurrentQueue<ZeroCopyTask>& outQueue,
                                           ConcurrentQueue<ZeroCopyTask>& compQueue)
{
    try
    {
        SetThreadName("ZC_Output");
        WaitForStart(m_startOutput);

        ck(cuCtxSetCurrent(m_cuContext));

        uint32_t delay = m_pEncoder->GetMinOutputDelay();
        bool endTaskReceived = false;

        while (m_eos.load() == 0 && isRunning())
        {
            if (outQueue.size() > delay)
            {
                ZeroCopyTask task = PopFront(outQueue);

                if (task.endTask)
                {
                    PushBack(compQueue, task);
                    endTaskReceived = true;
                    break;
                }

                ZeroCopyOutputFrame outputFrame;
                m_pEncoder->GetLockBitstream(task.frameNum, 
                                             m_pEncoder->GetOutputBfr(task.frameNum), 
                                             outputFrame);
                
                task.bitstream = std::move(outputFrame.bitstream);
                task.pictureType = outputFrame.pictureType;
                task.outputTimestamp = outputFrame.timestamp;
                
                WriteOutput(task);
                
                m_pEncoder->UnLockBitstream(task.frameNum, 
                                            m_pEncoder->GetOutputBfr(task.frameNum));
                
                if (task.arrayIdx >= 0)
                {
                    m_pEncoder->UnmapInputSurface(static_cast<uint32_t>(task.arrayIdx));
                    m_pDecoder->UnlockFrame(task.arrayIdx);
                    task.arrayIdx = -1;
                }
                
                PushBack(compQueue, task);
                m_nFrameCount++;
            }
            else
            {
                std::this_thread::yield();
            }
        }

        while (isRunning() && !endTaskReceived)
        {
            if (outQueue.size() > 0)
            {
                ZeroCopyTask task = PopFront(outQueue);

                if (task.endTask)
                {
                    PushBack(compQueue, task);
                    break;
                }

                ZeroCopyOutputFrame outputFrame;
                m_pEncoder->GetLockBitstream(task.frameNum, 
                                             m_pEncoder->GetOutputBfr(task.frameNum), 
                                             outputFrame);
                
                task.bitstream = std::move(outputFrame.bitstream);
                task.pictureType = outputFrame.pictureType;
                task.outputTimestamp = outputFrame.timestamp;
                
                WriteOutput(task);
                
                m_pEncoder->UnLockBitstream(task.frameNum, 
                                            m_pEncoder->GetOutputBfr(task.frameNum));
                
                if (task.arrayIdx >= 0)
                {
                    m_pEncoder->UnmapInputSurface(static_cast<uint32_t>(task.arrayIdx));
                    m_pDecoder->UnlockFrame(task.arrayIdx);
                    task.arrayIdx = -1;
                }
                
                PushBack(compQueue, task);
                m_nFrameCount++;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }
    catch (...)
    {
        m_error = 1;
        m_vExceptionPtrs[2] = std::current_exception();
    }
}

void ZeroCopyTranscoder::WriteOutput(ZeroCopyTask& task)
{
    if (!task.nonVidPkt.empty() && m_mediaFormat != MEDIA_FORMAT_ELEMENTARY)
    {
        for (const auto& pkt : task.nonVidPkt)
        {
            m_pMuxer->Mux(const_cast<uint8_t*>(pkt.data.data()),
                          static_cast<unsigned int>(pkt.nBytes),
                          pkt.pts, pkt.dts, pkt.duration, pkt.streamIndex);
        }
        task.nonVidPkt.clear();
    }

    if (task.bitstream.empty())
    {
        return;
    }

    if (m_mediaFormat != MEDIA_FORMAT_ELEMENTARY)
    {
        bool isKeyFrame = (task.pictureType == NV_ENC_PIC_TYPE_IDR);
        int64_t frameDts = m_frameDtsCounter;
        
        if (m_frameDuration == 0 && task.duration > 0)
        {
            m_frameDuration = task.duration;
        }
        m_frameDtsCounter += (m_frameDuration > 0) ? m_frameDuration : 1;
        
        m_pMuxer->Mux(const_cast<uint8_t*>(task.bitstream.data()),
                      static_cast<unsigned int>(task.bitstream.size()),
                      task.outputTimestamp, frameDts, task.duration,
                      task.streamIndex, isKeyFrame, 0);
    }
    else
    {
        if (m_bUseIVFContainer)
        {
            IVFUtils ivfUtils;
            std::vector<uint8_t> ivfPacket;
            if (m_bWriteIVFFileHeader)
            {
                ivfUtils.WriteFileHeader(ivfPacket, MAKE_FOURCC('A', 'V', '0', '1'),
                                          m_nWidth, m_nHeight, 30, 1, 0xFFFF);
                m_bWriteIVFFileHeader = false;
            }
            ivfUtils.WriteFrameHeader(ivfPacket, task.bitstream.size(), m_nFrameCount);
            m_fpOut->write(reinterpret_cast<const char*>(ivfPacket.data()), ivfPacket.size());
        }
        
        m_fpOut->write(reinterpret_cast<const char*>(task.bitstream.data()),
                       task.bitstream.size());
    }
    
    if (m_bVerbose && m_nFrameCount % 100 == 0)
    {
        std::cout << "\r  Transcoded " << m_nFrameCount << " frames..." << std::flush;
    }
}

void TranscodeSessionProc(
    CUcontext cuContext,
    const char* szInputFile,
    const AppConfig* pConfig,
    int sessionId,
    int* pFrameCount,
    double* pThreadFps,
    std::exception_ptr& exceptionPtr)
{
    try
    {
        CK_CUDA(cuCtxSetCurrent(cuContext));
        
        std::unique_ptr<FFmpegDemuxer> pDemuxer(new FFmpegDemuxer(szInputFile));
        
        AppConfig localConfig = *pConfig;
        
        std::unique_ptr<ZeroCopyTranscoder> pTranscoder(
            new ZeroCopyTranscoder(cuContext, *pDemuxer, localConfig));
        
        char szSessionOutput[512];
#ifdef _WIN32
        snprintf(szSessionOutput, sizeof(szSessionOutput), "NUL");
#else
        snprintf(szSessionOutput, sizeof(szSessionOutput), "/dev/null");
#endif
        
        auto tStart = std::chrono::high_resolution_clock::now();
        pTranscoder->Transcode(szSessionOutput);
        auto tEnd = std::chrono::high_resolution_clock::now();
        
        *pFrameCount = pTranscoder->GetFrameCount();
        
        auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();
        *pThreadFps = (msec > 0) ? (*pFrameCount * 1000.0 / msec) : 0;
        
        std::cout << "Thread FPS = " << *pThreadFps << std::endl;
        
        pTranscoder.reset();
        pDemuxer.reset();
    }
    catch (...)
    {
        exceptionPtr = std::current_exception();
    }
}

int main(int argc, char* argv[])
{
    AppConfig config;
    
    if (!ParseCommandLine(argc, argv, config))
    {
        return 1;
    }

    try
    {
        ck(cuInit(0));
        
        int nGpu = 0;
        ck(cuDeviceGetCount(&nGpu));
        
        if (config.iGpu < 0 || config.iGpu >= nGpu)
        {
            std::cout << "GPU ordinal out of range. Should be within [" << 0 << ", " << nGpu - 1 << "]" << std::endl;
            return 1;
        }

        CUdevice cuDevice = 0;
        ck(cuDeviceGet(&cuDevice, config.iGpu));
        
        char szDeviceName[80];
        ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
        std::cout << "GPU in use: " << szDeviceName << std::endl;

        if (config.bPerfMode)
        {
            std::vector<CUcontext> vContexts;
            std::vector<std::thread> vThreads;
            std::vector<int> vFrameCounts(config.nThread, 0);
            std::vector<double> vThreadFps(config.nThread, 0);
            std::vector<std::exception_ptr> vExceptionPtrs(config.nThread, nullptr);
            
            CUcontext sharedContext = nullptr;
            
            auto t0 = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < config.nThread; i++)
            {
                CUcontext ctx;
                if (config.bSingleContext)
                {
                    if (sharedContext == nullptr)
                    {
                        ck(NVCODEC_CUDA_CTX_CREATE(&sharedContext, 0, cuDevice));
                        vContexts.push_back(sharedContext);
                    }
                    ctx = sharedContext;
                }
                else
                {
                    ck(NVCODEC_CUDA_CTX_CREATE(&ctx, 0, cuDevice));
                    vContexts.push_back(ctx);
                }
                
                vThreads.emplace_back(TranscodeSessionProc,
                                      ctx,
                                      config.szInputFile,
                                      &config,
                                      i,
                                      &vFrameCounts[i],
                                      &vThreadFps[i],
                                      std::ref(vExceptionPtrs[i]));
            }
            
            for (auto& t : vThreads)
            {
                t.join();
            }
            
            auto t1 = std::chrono::high_resolution_clock::now();
            auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            
            for (int i = 0; i < config.nThread; i++)
            {
                if (vExceptionPtrs[i])
                {
                    std::rethrow_exception(vExceptionPtrs[i]);
                }
            }
            
            int nFrameTransTotal = 0;
            for (int i = 0; i < config.nThread; i++)
            {
                nFrameTransTotal += vFrameCounts[i];
            }
            
            std::cout << "nFrameTransTotal=" << nFrameTransTotal << ", time=" << msec 
                      << " millisec, FPS=" << (nFrameTransTotal * 1000 / msec) << std::endl;
            
            for (auto ctx : vContexts)
            {
                cuCtxDestroy(ctx);
            }
        }
        else
        {
            CUcontext cuContext = nullptr;
            ck(NVCODEC_CUDA_CTX_CREATE(&cuContext, 0, cuDevice));

            FFmpegDemuxer demuxer(config.szInputFile);

            bool bOut10 = config.nOutBitDepth ? config.nOutBitDepth > 8 : demuxer.GetBitDepth() > 8;

            {
                ZeroCopyTranscoder transcoder(cuContext, demuxer, config);

                transcoder.Transcode(config.szOutputFile);

                std::cout << "Saved in file " << config.szOutputFile << " of " << (bOut10 ? 10 : 8) << " bit depth" << std::endl;
            }

            ck(cuCtxDestroy(cuContext));
        }

        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cout << ex.what();
        exit(1);
    }
    return 0;
}
