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

#include <fstream>
#include <iostream>
#include <cuda.h>
#include <memory>
#include <iomanip>
#include <functional>
#include <stdint.h>
#include "NvEncoder/NvEncoderCuda.h"
#include "../Utils/Logger.h"
#include "../Utils/NvEncoderCLIOptions.h"
#include "../Utils/NvCodecUtils.h"
#include "../Common/AppEncUtils.h"

#define DUMP_HINTS 0

#define INVALID_HINT_REFIDX 31U

#define H264_MB_SIZE 16U
#define HEVC_CTB_SIZE 32U
#define AV1_SB_SIZE 64U

simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

struct ExternalMEHintInfo
{
    uint32_t nFrameIdx;
    std::string strExternalMEHintFile;
    uint16_t meHintRefPicDist[2];
    NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE meHintCountsPerBlock[2];
};

class AppEncExternalME
{
public:
    AppEncExternalME();
    void Run(int argc, char *argv[]);

private:
    int m_iGPU;
    CUdevice m_cuDevice;
    CUcontext m_cuContext;

    NvEncoderCuda *m_pEncoder;
    NvEncoderInitParam m_stInitParam;
    NV_ENC_INITIALIZE_PARAMS m_stEncInitializeParams;
    NV_ENC_CONFIG m_stEncConfig;
    NV_ENC_PIC_PARAMS m_stPicParams;

    uint32_t m_nWidth;
    uint32_t m_nHeight;
    uint32_t m_nFrame;
    uint32_t m_nCurrentFrameIdx;
    NV_ENC_BUFFER_FORMAT m_eFormat;
    bool m_bEnableExternalMEHints;
    uint32_t m_nMaxMEHintCountsPerBlock;
    uint32_t m_nMaxMEHintBufferSize;
    void* m_pExternalMEHintBuffer;
    std::streamsize m_nExternalMEHintBufferSize;
    std::vector<ExternalMEHintInfo> m_vExternalMEHintInfo;

    std::string m_strInFilePath;
    std::string m_strOutFilePath;
    std::string m_strExternalMEHintConfigFilePath;
    std::ifstream *m_fpInput;
    std::ofstream *m_fpOutput;
    std::ifstream *m_fpHintConfigFile;
    std::ifstream *m_fpExternalMEHint;

    void Initialize();
    void DeInitialize();
    void ParseCommandLine(int argc, char *argv[]);
    void ShowEncoderBriefHelp();
    void ShowEncoderDetailedHelp();
    void ShowHelpAndExit(const char *szBadOption = NULL);
    void InitializeEncodeParams();
    void Encode();
    void ParseExternalMEHintConfigFile();
    void InitializeExternalMEHintsParams(NV_ENC_INITIALIZE_PARAMS &pInitParams);
    uint32_t GetBlockSize();
    void AllocateAppExternalMEHintBuffer();
    void SetupPicExternalMEHintParams();
    void SetupInvalidHints();
    void ReadPicMEHintsFile(std::string &strExternalMEHintFilePath);
    void DumpExternalMEHints();
};

AppEncExternalME::AppEncExternalME():
    m_iGPU(0),
    m_pEncoder(nullptr),
    m_nWidth(0),
    m_nHeight(0),
    m_nFrame(0),
    m_nCurrentFrameIdx(0),
    m_eFormat(NV_ENC_BUFFER_FORMAT_IYUV),
    m_bEnableExternalMEHints(false),
    m_nMaxMEHintCountsPerBlock(0),
    m_nMaxMEHintBufferSize(0),
    m_pExternalMEHintBuffer(nullptr),
    m_nExternalMEHintBufferSize(0),
    m_fpInput(nullptr),
    m_fpOutput(nullptr),
    m_fpHintConfigFile(nullptr),
    m_fpExternalMEHint(nullptr)
{
}

void AppEncExternalME::Initialize()
{
    ValidateResolution(m_nWidth, m_nHeight);

    if (m_strOutFilePath.empty())
    {
        m_strOutFilePath = m_stInitParam.IsCodecH264() ? "out.h264" :
            m_stInitParam.IsCodecHEVC() ? "out.hevc" : "out.av1";
    }

    m_fpInput = new std::ifstream(m_strInFilePath, std::ifstream::in | std::ifstream::binary);
    if (!m_fpInput || !(m_fpInput->is_open()))
    {
        std::ostringstream err;
        err << "Unable to open input file: " << m_strInFilePath << std::endl;
        throw std::invalid_argument(err.str());
    }

    m_fpOutput = new std::ofstream(m_strOutFilePath, std::ios::out | std::ios::binary);
    if (!m_fpOutput || !(m_fpOutput->is_open()))
    {
        std::ostringstream err;
        err << "Unable to open output file: " << m_strOutFilePath << std::endl;
        throw std::invalid_argument(err.str());
    }
    if (m_bEnableExternalMEHints)
    {
        m_fpHintConfigFile = new std::ifstream(m_strExternalMEHintConfigFilePath);
        if (!m_fpHintConfigFile || !(m_fpHintConfigFile->is_open()))
        {
            std::ostringstream err;
            err << "Unable to open externalMEHintConfigFile: " << m_strExternalMEHintConfigFilePath << std::endl;
            throw std::invalid_argument(err.str());
        }
    }

    if (m_eFormat == NV_ENC_BUFFER_FORMAT_UNDEFINED)
    {
        m_eFormat = NV_ENC_BUFFER_FORMAT_IYUV;
    }

    ck(cuInit(0));
    int iGPU = 0;
    ck(cuDeviceGetCount(&iGPU));
    if (m_iGPU < 0 || m_iGPU >= iGPU)
    {
        std::ostringstream err;
        err << "GPU ordinal out of range. Should be within [" << 0 << ", " << iGPU - 1 << "]" << std::endl;
        throw std::invalid_argument(err.str());
    }

    ck(cuDeviceGet(&m_cuDevice, m_iGPU));
    char szDeviceName[80];
    ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), m_cuDevice));
    std::cout << "GPU in use: " << szDeviceName << std::endl;
    ck(NVCODEC_CUDA_CTX_CREATE(&m_cuContext, 0, m_cuDevice));

    m_pEncoder = new NvEncoderCuda(m_cuContext, m_nWidth, m_nHeight, m_eFormat, 0);
    memset(&m_stEncInitializeParams, 0, sizeof(m_stEncInitializeParams));
    m_stEncInitializeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    memset(&m_stEncConfig, 0, sizeof(m_stEncConfig));
    m_stEncConfig.version = NV_ENC_CONFIG_VER;
    m_stEncInitializeParams.encodeConfig = &m_stEncConfig;
    memset(&m_stPicParams, 0, sizeof(m_stPicParams));
    m_stPicParams.version = NV_ENC_PIC_PARAMS_VER;
}

void AppEncExternalME::DeInitialize()
{
    if (m_pEncoder)
    {
        m_pEncoder->DestroyEncoder();
    }

    if (m_fpOutput)
    {
        m_fpOutput->close();
    }

    if (m_fpInput)
    {
        m_fpInput->close();
    }

    if (m_fpHintConfigFile)
    {
        m_fpHintConfigFile->close();
    }

    if (m_pExternalMEHintBuffer)
    {
        if (m_stInitParam.IsCodecAV1())
        {
            delete[](NVENC_EXTERNAL_ME_SB_HINT *) m_pExternalMEHintBuffer;
        }
        else
        {
            delete[](NVENC_EXTERNAL_ME_HINT *) m_pExternalMEHintBuffer;
        }
    }

    if (m_fpExternalMEHint)
    {
        m_fpExternalMEHint->close();
    }
    if (m_cuContext)
    {
        ck(cuCtxDestroy(m_cuContext));
    }

}

void AppEncExternalME::InitializeExternalMEHintsParams(NV_ENC_INITIALIZE_PARAMS &pInitParams)
{
    pInitParams.enableExternalMEHints = 1;
    uint32_t nPredictorCount = pInitParams.encodeConfig->frameIntervalP > 1 ? 2 : 1;
    for (uint32_t i = 0; i < nPredictorCount; i++)
    {
        pInitParams.maxMEHintCountsPerBlock[i].numCandsPerBlk16x16 = m_nMaxMEHintCountsPerBlock;
        pInitParams.maxMEHintCountsPerBlock[i].numCandsPerBlk16x8 = m_nMaxMEHintCountsPerBlock;
        pInitParams.maxMEHintCountsPerBlock[i].numCandsPerBlk8x16 = m_nMaxMEHintCountsPerBlock;
        pInitParams.maxMEHintCountsPerBlock[i].numCandsPerBlk8x8 = m_nMaxMEHintCountsPerBlock;
        pInitParams.maxMEHintCountsPerBlock[i].numCandsPerSb = m_nMaxMEHintCountsPerBlock;
    }
}

void AppEncExternalME::AllocateAppExternalMEHintBuffer()
{
    uint32_t nBlockSize = GetBlockSize();
    uint32_t nWidthInBlocks = (m_nWidth + nBlockSize - 1) / nBlockSize;
    uint32_t nHeightInBlocks = (m_nHeight + nBlockSize - 1) / nBlockSize;
    uint32_t nPredictorCount = m_stEncConfig.frameIntervalP > 1 ? 2 : 1;

    if (m_stInitParam.IsCodecAV1())
    {
        m_nMaxMEHintBufferSize = m_nMaxMEHintCountsPerBlock * nWidthInBlocks * nHeightInBlocks;
        m_pExternalMEHintBuffer = (void*) new NVENC_EXTERNAL_ME_SB_HINT[m_nMaxMEHintBufferSize];
        m_nMaxMEHintBufferSize *= sizeof(NVENC_EXTERNAL_ME_SB_HINT);
    }
    else
    {
        // There can be at most 9 hints per block: 1 16x16, 2 8x16, 2 16x8 and 4 8x8
        uint32_t maxMEHints = 9 * m_nMaxMEHintCountsPerBlock;
        m_nMaxMEHintBufferSize = maxMEHints * nWidthInBlocks * nWidthInBlocks * nPredictorCount;
        m_pExternalMEHintBuffer = (void*) new NVENC_EXTERNAL_ME_HINT[m_nMaxMEHintBufferSize];
        m_nMaxMEHintBufferSize *= sizeof(NVENC_EXTERNAL_ME_HINT);
    }
    if (!m_pExternalMEHintBuffer)
    {
        std::ostringstream err;
        err << "Allocation of buffer for external ME hints failed" << std::endl;
        throw std::invalid_argument(err.str());
    }
}

void AppEncExternalME::ParseExternalMEHintConfigFile()
{
    std::stringstream strm;
    std::string strLine, strKey, strSeperator, strVal;

    while (std::getline(*m_fpHintConfigFile, strLine))
    {
        strm.clear();
        strm.str(strLine);
        strm >> strKey >> strSeperator >> strVal;
        if (strKey == "frameNumber")
        {
            ExternalMEHintInfo stExternalMEHintInfo = { 0 };
            stExternalMEHintInfo.nFrameIdx = std::stoi(strVal);
            stExternalMEHintInfo.meHintRefPicDist[0] = 1;
            stExternalMEHintInfo.meHintRefPicDist[1] = 1;
            while (getline(*m_fpHintConfigFile, strLine))
            {
                strm.clear();
                strm.str(strLine);
                strm >> strKey >> strSeperator >> strVal;
                if (strKey == "meHintCountsPerBlock[0].numCandsPerBlk16x16")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[0].numCandsPerBlk16x16 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[1].numCandsPerBlk16x16")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[1].numCandsPerBlk16x16 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[0].numCandsPerBlk16x8")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[0].numCandsPerBlk16x8 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[1].numCandsPerBlk16x8")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[1].numCandsPerBlk16x8 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[0].numCandsPerBlk8x16")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[0].numCandsPerBlk8x16 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[1].numCandsPerBlk8x16")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[1].numCandsPerBlk8x16 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[0].numCandsPerBlk8x8")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[0].numCandsPerBlk8x8 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[1].numCandsPerBlk8x8")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[1].numCandsPerBlk8x8 = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[0].numCandsPerSb")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[0].numCandsPerSb = std::stoi(strVal);
                }
                else if (strKey == "meHintCountsPerBlock[1].numCandsPerSb")
                {
                    stExternalMEHintInfo.meHintCountsPerBlock[1].numCandsPerSb = std::stoi(strVal);
                }
                else if (strKey == "meHintRefPicDist[0]")
                {
                    stExternalMEHintInfo.meHintRefPicDist[0] = std::stoi(strVal);
                }
                else if (strKey == "meHintRefPicDist[1]")
                {
                    stExternalMEHintInfo.meHintRefPicDist[1] = std::stoi(strVal);
                }
                else if (strKey == "externalMEHintFile")
                {
                    stExternalMEHintInfo.strExternalMEHintFile = strVal;
                    break;
                }
                else
                {
                    std::ostringstream err;
                    err << "Invalid key " << strKey << " in externalMEHintConfigFile" << std::endl;
                    throw std::invalid_argument(err.str());
                }
            }
            m_vExternalMEHintInfo.push_back(stExternalMEHintInfo);
        }
    }
}

void AppEncExternalME::ReadPicMEHintsFile(std::string &strExternalMEHintFilePath)
{
    m_fpExternalMEHint = new std::ifstream(strExternalMEHintFilePath, std::ios::binary | std::ios::ate);
    if (!m_fpExternalMEHint || !(m_fpExternalMEHint->is_open()))
    {
        std::ostringstream err;
        err << "Unable to open externalMEHintFile: " << strExternalMEHintFilePath << std::endl;
        throw std::invalid_argument(err.str());
    }
    std::streamsize nFileSize = m_fpExternalMEHint->tellg();
    if (nFileSize > m_nMaxMEHintBufferSize)
    {
        nFileSize = m_nMaxMEHintBufferSize;
    }
    m_fpExternalMEHint->seekg(0, std::ios::beg);
    m_fpExternalMEHint->read((char*)m_pExternalMEHintBuffer, nFileSize);
    if (m_fpExternalMEHint->fail())
    {
        std::ostringstream err;
        err << "Reading from " << strExternalMEHintFilePath << " failed." << std::endl;
        throw std::invalid_argument(err.str());
    }
    m_nExternalMEHintBufferSize = m_fpExternalMEHint->gcount();
    m_fpExternalMEHint->close();
    m_fpExternalMEHint = nullptr;
}

void AppEncExternalME::DumpExternalMEHints()
{
    std::string dumpFilename = "Frame" + std::to_string(m_nCurrentFrameIdx) + "HintsDump.txt";
    std::ofstream dumpFile(dumpFilename);
    uint32_t nHintStructSize = m_stInitParam.IsCodecAV1() ? sizeof(NVENC_EXTERNAL_ME_SB_HINT) : sizeof(NVENC_EXTERNAL_ME_HINT);
    uint32_t nTotalHints = static_cast<uint32_t>(m_nExternalMEHintBufferSize / nHintStructSize);

    if (m_stInitParam.IsCodecAV1())
    {
        NVENC_EXTERNAL_ME_SB_HINT* pCurrentHint = (NVENC_EXTERNAL_ME_SB_HINT*)m_pExternalMEHintBuffer;
        for (uint32_t nHintIdx = 0; nHintIdx < nTotalHints; ++nHintIdx)
        {
            dumpFile << std::endl << "============== Idx = " << nHintIdx << " ==============" << std::endl;
            dumpFile << "refidx : " << (pCurrentHint->refidx & 0x1F) << std::endl;
            dumpFile << "direction : " << (pCurrentHint->direction & 0x1) << std::endl;
            dumpFile << "bi : " << (pCurrentHint->bi & 0x1) << std::endl;
            dumpFile << "partition_type : " << (pCurrentHint->partition_type & 0x7) << std::endl;
            dumpFile << "x8 : " << (pCurrentHint->x8 & 0x7) << std::endl;
            dumpFile << "last_of_cu : " << (pCurrentHint->last_of_cu & 0x1) << std::endl;
            dumpFile << "last_of_sb : " << (pCurrentHint->last_of_sb & 0x1) << std::endl;
            dumpFile << "reserved0 : " << pCurrentHint->reserved0 << std::endl;
            dumpFile << "mvx : " << (pCurrentHint->mvx >> 2) << std::endl;
            dumpFile << "cu_size : " << (pCurrentHint->cu_size & 0x3) << std::endl;
            dumpFile << "mvy : " << (pCurrentHint->mvy >> 2) << std::endl;
            dumpFile << "y8 : " << (pCurrentHint->y8 & 0x7) << std::endl;
            dumpFile << "reserved1 : " << pCurrentHint->reserved1 << std::endl;
            ++pCurrentHint;
        }
    }
    else
    {
        NVENC_EXTERNAL_ME_HINT* pCurrentHint = (NVENC_EXTERNAL_ME_HINT*)m_pExternalMEHintBuffer;
        for (uint32_t nHintIdx = 0; nHintIdx < nTotalHints; ++nHintIdx)
        {
            dumpFile << std::endl << "============== Idx = " << nHintIdx << " ==============" << std::endl;
            dumpFile << "mvx : " << pCurrentHint->mvx << std::endl;
            dumpFile << "mvy : " << pCurrentHint->mvy << std::endl;
            dumpFile << "refidx : " << (pCurrentHint->refidx & 0x1F) << std::endl;
            dumpFile << "dir : " << (pCurrentHint->dir & 0x1) << std::endl;
            dumpFile << "partType : " << (pCurrentHint->partType & 0x3) << std::endl;
            dumpFile << "lastofPart : " << (pCurrentHint->lastofPart & 0x1) << std::endl;
            dumpFile << "lastOfMB : " << (pCurrentHint->lastOfMB & 0x1) << std::endl;
            ++pCurrentHint;
        }
    }
    dumpFile.close();
}

void AppEncExternalME::SetupInvalidHints()
{
    // Pass a single invalid hint per block in L0 direction

    memset(&(m_stPicParams.meHintCountsPerBlock[0]), 0, sizeof(NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE));
    memset(&(m_stPicParams.meHintCountsPerBlock[1]), 0, sizeof(NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE));

    uint32_t nBlockSize = GetBlockSize();
    uint32_t nWidthInBlocks = (m_nWidth + nBlockSize - 1) / nBlockSize;
    uint32_t nHeightInBlocks = (m_nHeight + nBlockSize - 1) / nBlockSize;
    uint32_t nTotalHints = nWidthInBlocks * nHeightInBlocks;

    if (m_stInitParam.IsCodecAV1())
    {
        m_stPicParams.meHintCountsPerBlock[0].numCandsPerSb = 1;
    }
    else
    {
        m_stPicParams.meHintCountsPerBlock[0].numCandsPerBlk16x16 = 1;
    }

    if (m_stInitParam.IsCodecAV1())
    {
        NVENC_EXTERNAL_ME_SB_HINT* pCurrentHint = (NVENC_EXTERNAL_ME_SB_HINT*)m_pExternalMEHintBuffer;
        for (uint32_t nHintIdx = 0; nHintIdx < nTotalHints; ++nHintIdx)
        {
            pCurrentHint->refidx = INVALID_HINT_REFIDX;
            pCurrentHint->last_of_cu = 1;
            pCurrentHint->last_of_sb = 1;
            ++pCurrentHint;
        }
        m_stPicParams.meExternalSbHints = (NVENC_EXTERNAL_ME_SB_HINT*)m_pExternalMEHintBuffer;
        m_stPicParams.meSbHintsCount = nTotalHints;
    }
    else
    {
        NVENC_EXTERNAL_ME_HINT* pCurrentHint = (NVENC_EXTERNAL_ME_HINT*)m_pExternalMEHintBuffer;
        for (uint32_t nHintIdx = 0; nHintIdx < nTotalHints; ++nHintIdx)
        {
            pCurrentHint->refidx = INVALID_HINT_REFIDX;
            pCurrentHint->lastofPart = 1;
            pCurrentHint->lastOfMB   = 1;
            ++pCurrentHint;
        }
        m_stPicParams.meExternalHints = (NVENC_EXTERNAL_ME_HINT*)m_pExternalMEHintBuffer;
    }
}

void AppEncExternalME::SetupPicExternalMEHintParams()
{
    m_stPicParams.meExternalHints = nullptr;
    m_stPicParams.meExternalSbHints = nullptr;
    m_stPicParams.meSbHintsCount = 0;
    uint32_t nPredictorCount = m_stEncConfig.frameIntervalP > 1 ? 2 : 1;
    bool bHintFileFound = false;

    for (auto externalMEHintInfo : m_vExternalMEHintInfo)
    {
        if (externalMEHintInfo.nFrameIdx == m_nCurrentFrameIdx)
        {
            for (uint32_t i = 0; i < nPredictorCount; i++)
            {
                m_stPicParams.meHintCountsPerBlock[i] = externalMEHintInfo.meHintCountsPerBlock[i];
            }
            if (m_stInitParam.IsCodecH264())
            {
                m_stPicParams.meHintRefPicDist[0] = externalMEHintInfo.meHintRefPicDist[0];
                m_stPicParams.meHintRefPicDist[1] = externalMEHintInfo.meHintRefPicDist[1];
            }
            ReadPicMEHintsFile(externalMEHintInfo.strExternalMEHintFile);
            if (m_stInitParam.IsCodecAV1())
            {
                m_stPicParams.meExternalSbHints = (NVENC_EXTERNAL_ME_SB_HINT*)m_pExternalMEHintBuffer;
                m_stPicParams.meSbHintsCount = static_cast<uint32_t>(m_nExternalMEHintBufferSize / sizeof(NVENC_EXTERNAL_ME_SB_HINT));
            }
            else
            {
                m_stPicParams.meExternalHints = (NVENC_EXTERNAL_ME_HINT*)m_pExternalMEHintBuffer;
            }

#ifdef DUMP_HINTS
            DumpExternalMEHints();
#endif
            bHintFileFound = true;
            break;
        }
    }

    // External ME hints enabled, but user haven't provided hint file for this frame
    // Pass invalid hints for this frame to avoid error
    if (!bHintFileFound)
    {
        std::cout << "No hint file found for frame " << m_nCurrentFrameIdx << ", passing invalid hints" << std::endl;
        SetupInvalidHints();
    }
}

uint32_t AppEncExternalME::GetBlockSize()
{
    uint32_t nBlockSize = H264_MB_SIZE;
    if (m_stInitParam.IsCodecAV1())
    {
        nBlockSize = AV1_SB_SIZE;
    }
    else if (m_stInitParam.IsCodecHEVC())
    {
        nBlockSize = HEVC_CTB_SIZE;
    }
    return nBlockSize;
}

void AppEncExternalME::InitializeEncodeParams()
{
    m_pEncoder->CreateDefaultEncoderParams(&m_stEncInitializeParams, m_stInitParam.GetEncodeGUID(), m_stInitParam.GetPresetGUID(),
        m_stInitParam.GetTuningInfo() == NV_ENC_TUNING_INFO_LOW_LATENCY ? NV_ENC_TUNING_INFO_LOW_LATENCY : NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

    m_stEncConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
    // will be changed if -bf option is specified through CLI
    m_stEncConfig.frameIntervalP = 1;
    if (m_stInitParam.IsCodecH264())
    {
        m_stEncConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    }
    else if (m_stInitParam.IsCodecHEVC())
    {
        m_stEncConfig.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    }
    else
    {
        m_stEncConfig.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    }

    m_stInitParam.SetInitParams(&m_stEncInitializeParams, m_eFormat);

    if (m_bEnableExternalMEHints)
    {
        InitializeExternalMEHintsParams(m_stEncInitializeParams);
        ParseExternalMEHintConfigFile();
        AllocateAppExternalMEHintBuffer();
    }
}

void AppEncExternalME::Encode()
{
    InitializeEncodeParams();
    m_pEncoder->CreateEncoder(&m_stEncInitializeParams);

    std::streamsize  nRead = 0;
    uint32_t nFrameSize = m_pEncoder->GetFrameSize();
    std::unique_ptr<uint8_t[]> pHostFrame(new uint8_t[nFrameSize]);

    uint32_t nFrame = 0;
    do
    {
        std::vector<NvEncOutputFrame> vPacket;
        nRead = m_fpInput->read(reinterpret_cast<char*>(pHostFrame.get()), nFrameSize).gcount();
        // Read a full frame
        if (nRead == nFrameSize)
        {
            const NvEncInputFrame* pEncoderInputFrame = m_pEncoder->GetNextInputFrame();
            NvEncoderCuda::CopyToDeviceFrame(m_cuContext,
                pHostFrame.get(),
                0,
                (CUdeviceptr)pEncoderInputFrame->inputPtr,
                (int)pEncoderInputFrame->pitch,
                m_pEncoder->GetEncodeWidth(),
                m_pEncoder->GetEncodeHeight(),
                CU_MEMORYTYPE_HOST,
                pEncoderInputFrame->bufferFormat,
                pEncoderInputFrame->chromaOffsets,
                pEncoderInputFrame->numChromaPlanes);

            if (m_bEnableExternalMEHints)
            {
                SetupPicExternalMEHintParams();
            }
            m_pEncoder->EncodeFrame(vPacket, &m_stPicParams);
        }
        // no more frames left
        else
        {
            m_pEncoder->EndEncode(vPacket);
        }

        // Encode and LockBitstream calls completed, write to output file
        nFrame += (int)vPacket.size();
        for (NvEncOutputFrame &packet : vPacket)
        {
            m_fpOutput->write(reinterpret_cast<char*>(packet.frame.data()), packet.frame.size());
        }
        m_nCurrentFrameIdx++;
    } while (nRead == nFrameSize);

    std::cout << "Total frames encoded: " << nFrame << std::endl << "Saved in file " << m_strOutFilePath << std::endl;
}

void AppEncExternalME::ShowEncoderBriefHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA Video External Motion Estimation Hints Sample Application\n";
    oss << "=======================================================\n\n";

    oss << "Usage: AppEncExternalMEHint -i <input_file> [options]\n\n";

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
        << "codec-based (out.h264/hevc/av1)\n";
    oss << std::left << std::setw(25) << "-s <WxH>"
        << std::setw(12) << "Required"
        << "N/A\n";
    oss << std::left << std::setw(25) << "-if <format>"
        << std::setw(12) << "Optional"
        << "iyuv\n";
    oss << std::left << std::setw(25) << "-gpu <n>"
        << std::setw(12) << "Optional"
        << "0\n";
    oss << std::left << std::setw(25) << "-enableExternalMEHint"
        << std::setw(12) << "Optional"
        << "0\n";

    oss << "\nFor detailed help, use -A/--advanced-options\n";
    oss << "To view encoder capabilities:\n";
    oss << "  -ec        : Print encoder capabilities summary\n";
    oss << "  -ec-detail : Print detailed encoder capabilities\n";
    std::cout << oss.str();
    exit(0);
}

void AppEncExternalME::ShowEncoderDetailedHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA Video External Motion Estimation Hints Sample Application - Detailed Help\n";
    oss << "=================================================================\n\n";

    oss << "Usage: AppEncExternalMEHint -i <input_file> [options]\n\n";

    // Full table of all arguments
    oss << "All Arguments:\n";
    oss << std::left << std::setw(25) << "Argument"
        << std::setw(12) << "Type"
        << std::setw(20) << "Default Value"
        << "Example\n";
    oss << std::string(80, '-') << "\n";

    // Required arguments
    oss << std::left << std::setw(25) << "-i <path>"
        << std::setw(12) << "Required"
        << std::setw(20) << "N/A"
        << "-i input.yuv\n";
    oss << std::left << std::setw(25) << "-s <WxH>"
        << std::setw(12) << "Required"
        << std::setw(20) << "N/A"
        << "-s 1920x1080\n";

    // Optional arguments
    oss << std::left << std::setw(25) << "-o <path>"
        << std::setw(12) << "Optional"
        << std::setw(20) << "codec-based"
        << "-o output.h264\n";
    oss << std::left << std::setw(25) << "-if <format>"
        << std::setw(12) << "Optional"
        << std::setw(20) << "iyuv"
        << "-if yuv444\n";
    oss << std::left << std::setw(25) << "-gpu <n>"
        << std::setw(12) << "Optional"
        << std::setw(20) << "0"
        << "-gpu 1\n";
    oss << std::left << std::setw(25) << "-enableExternalMEHint"
        << std::setw(12) << "Optional"
        << std::setw(20) << "0"
        << "-enableExternalMEHint 1\n";
    oss << std::left << std::setw(25) << "-maxMEHintCountsPerBlock"
        << std::setw(12) << "Optional"
        << std::setw(20) << "1"
        << "-maxMEHintCountsPerBlock 2\n";
    oss << std::left << std::setw(25) << "-externalMEHintConfigFile"
        << std::setw(12) << "Optional"
        << std::setw(20) << "N/A"
        << "-externalMEHintConfigFile hints.cfg\n";

    // Detailed descriptions
    oss << "\nDetailed Descriptions:\n";
    oss << "-------------------\n";
    oss << std::left << std::setw(25) << "-i" << ": Input file path\n";
    oss << std::left << std::setw(25) << "-o" << ": Output encoded file path\n";
    oss << std::left << std::setw(25) << "-s" << ": Input resolution in WxH format\n";
    oss << std::left << std::setw(25) << "-if" << ": Input format (iyuv/nv12)\n";
    oss << std::left << std::setw(25) << "-gpu" << ": Ordinal of GPU to use\n";
    oss << std::left << std::setw(25) << "-enableExternalMEHint" << ": Enable external ME hints (0/1)\n";
    oss << std::left << std::setw(25) << "-maxMEHintCountsPerBlock" << ": Maximum hints per block\n";
    oss << std::left << std::setw(25) << "-externalMEHintConfigFile" << ": Path to ME hints config file\n";
    oss << std::left << std::setw(25) << "-h/--help" << ": Print basic usage information\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print detailed usage information\n";
    oss << std::left << std::setw(25) << "-ec" << ": Print encoder capabilities summary\n";
    oss << std::left << std::setw(25) << "-ec-detail" << ": Print detailed encoder capabilities\n";

    // Important notes
    oss << "\nNotes:\n";
    oss << "------\n";
    oss << "* This sample demonstrates external motion estimation hints\n";
    oss << "* Supports providing motion vector hints to the encoder\n";
    oss << "* Hints can be provided per block (16x16, 16x8, 8x16, 8x8)\n";
    oss << "* Supports H.264, HEVC, and AV1 encoding with external ME hints\n";
    oss << "* Requires a properly formatted hint configuration file\n";
    oss << std::endl;

    oss << NvEncoderInitParam().GetHelpMessage(false, false, true, false, false, false, false, false) << std::endl;
    oss << "\nTo view encoder capabilities:\n";
    oss << "  -ec        : Print encoder capabilities summary\n";
    oss << "  -ec-detail : Print detailed encoder capabilities\n";
    std::cout << oss.str();
    exit(0);
}

void AppEncExternalME::ShowHelpAndExit(const char *szBadOption)
{
    if (szBadOption)
    {
        std::ostringstream oss;
        oss << "Error parsing \"" << szBadOption << "\"\n";
        oss << "Use -h/--help for basic usage or -A/--advanced-options for detailed information\n";
        throw std::invalid_argument(oss.str());
    }
}

void AppEncExternalME::ParseCommandLine(int argc, char* argv[])
{
    std::ostringstream oss;
    int i, nWidth = 0, nHeight = 0;

    if (argc == 1) {
        std::cout << "No Arguments provided! Please refer to the following for options:\n";
        ShowEncoderBriefHelp();
    }

    for (i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help"))
        {
            ShowEncoderBriefHelp();
        }
        if (!_stricmp(argv[i], "-A") || !_stricmp(argv[i], "--advanced-options"))
        {
            ShowEncoderDetailedHelp();
        }
        if (!_stricmp(argv[i], "-ec-detail"))
        {
            ShowEncoderCapabilityDetailed();
        }
        if (!_stricmp(argv[i], "-ec"))
        {
            ShowEncoderCapability();
        }
        if (!_stricmp(argv[i], "-i"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-i");
            }
            m_strInFilePath = argv[i];
            continue;
        }
        if (!_stricmp(argv[i], "-o"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-o");
            }
            m_strOutFilePath = argv[i];
            continue;
        }
        if (!_stricmp(argv[i], "-s"))
        {
            if (++i == argc || 2 != sscanf(argv[i], "%dx%d", &nWidth, &nHeight))
            {
                ShowHelpAndExit("-s");
            }
            m_nWidth = nWidth;
            m_nHeight = nHeight;
            continue;
        }

        std::vector<std::string> vszFileFormatName = { "iyuv", "nv12" };

        NV_ENC_BUFFER_FORMAT aFormat[] =
        {
            NV_ENC_BUFFER_FORMAT_IYUV,
            NV_ENC_BUFFER_FORMAT_NV12,
        };

        if (!_stricmp(argv[i], "-if"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-if");
            }
            auto it = std::find(vszFileFormatName.begin(), vszFileFormatName.end(), argv[i]);
            if (it == vszFileFormatName.end())
            {
                ShowHelpAndExit("-if");
            }
            m_eFormat = aFormat[it - vszFileFormatName.begin()];
            continue;
        }
        if (!_stricmp(argv[i], "-gpu")) {
            if (++i == argc) {
                ShowHelpAndExit("-gpu");
            }
            m_iGPU = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-frame")) {
            if (++i == argc) {
                ShowHelpAndExit("-frame");
            }
            m_nFrame = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-enableExternalMEHint")) {
            if (++i == argc) {
                ShowHelpAndExit("-enableExternalMEHint");
            }
            m_bEnableExternalMEHints = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-maxMEHintCountsPerBlock")) {
            if (++i == argc) {
                ShowHelpAndExit("-maxMEHintCountsPerBlock");
            }
            m_nMaxMEHintCountsPerBlock = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-externalMEHintConfigFile"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit("-externalMEHintConfigFile");
            }
            m_strExternalMEHintConfigFilePath = argv[i];
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
    m_stInitParam = NvEncoderInitParam(oss.str().c_str(), nullptr, true);
}

void AppEncExternalME::Run(int argc, char *argv[])
{
    try
    {
        ParseCommandLine(argc, argv);
        Initialize();
        Encode();
        DeInitialize();
    }
    catch (const std::exception &e)
    {
        DeInitialize();
        std::cout << e.what();
        exit(1);
    }
}

int main(int argc, char **argv)
{
    AppEncExternalME appEncExternalME;
    appEncExternalME.Run(argc, argv);
    return 0;
}
