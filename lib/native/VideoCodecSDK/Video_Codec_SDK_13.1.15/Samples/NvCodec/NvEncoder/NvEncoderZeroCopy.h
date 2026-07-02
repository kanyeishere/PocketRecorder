/*
 * Copyright (c) 2010-2026 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the software, and to permit persons to whom the
 * software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <vector>
#include <mutex>
#include <stdint.h>
#include <string>
#include <sstream>
#include <cuda.h>
#include "nvEncodeAPI.h"

class NvEncoderZeroCopyException : public std::exception
{
public:
    NvEncoderZeroCopyException(const std::string& errorStr, NVENCSTATUS errorCode)
        : m_errorString(errorStr), m_errorCode(errorCode) {}

    virtual ~NvEncoderZeroCopyException() throw() {}
    virtual const char* what() const throw() { return m_errorString.c_str(); }
    NVENCSTATUS getErrorCode() const { return m_errorCode; }
    const std::string& getErrorString() const { return m_errorString; }

    static NvEncoderZeroCopyException makeException(const std::string& errorStr, NVENCSTATUS errorCode,
        const std::string& functionName, const std::string& fileName, int lineNo);

private:
    std::string m_errorString;
    NVENCSTATUS m_errorCode;
};

inline NvEncoderZeroCopyException NvEncoderZeroCopyException::makeException(
    const std::string& errorStr, NVENCSTATUS errorCode,
    const std::string& functionName, const std::string& fileName, int lineNo)
{
    std::ostringstream errorLog;
    errorLog << functionName << " : " << errorStr << " at " << fileName << ":" << lineNo << std::endl;
    return NvEncoderZeroCopyException(errorLog.str(), errorCode);
}

#define NVENC_ZC_THROW_ERROR(errorStr, errorCode) \
    do { \
        throw NvEncoderZeroCopyException::makeException(errorStr, errorCode, __FUNCTION__, __FILE__, __LINE__); \
    } while (0)

#define NVENC_ZC_API_CALL(nvencAPI) \
    do { \
        NVENCSTATUS errorCode = nvencAPI; \
        if (errorCode != NV_ENC_SUCCESS) { \
            std::ostringstream errorLog; \
            errorLog << #nvencAPI << " returned error " << errorCode; \
            throw NvEncoderZeroCopyException::makeException(errorLog.str(), errorCode, __FUNCTION__, __FILE__, __LINE__); \
        } \
    } while (0)

#define CUDA_ENC_ZC_CALL(cuAPI) \
    do { \
        CUresult err = cuAPI; \
        if (err != CUDA_SUCCESS) { \
            const char *szErrName = NULL; \
            cuGetErrorName(err, &szErrName); \
            std::ostringstream errorLog; \
            errorLog << #cuAPI << " returned error " << (szErrName ? szErrName : "Unknown"); \
            throw NvEncoderZeroCopyException::makeException(errorLog.str(), NV_ENC_ERR_GENERIC, __FUNCTION__, __FILE__, __LINE__); \
        } \
    } while (0)

struct ZeroCopyInputSurface
{
    CUarray cudaArray;
    NV_ENC_REGISTERED_PTR registeredPtr;
    NV_ENC_INPUT_PTR mappedPtr;
    unsigned int width;
    unsigned int height;
    unsigned int bufHeight;
    unsigned int pitch;
    NV_ENC_BUFFER_FORMAT bufferFormat;
    bool isMapped;
};

struct ZeroCopyOutputFrame
{
    std::vector<uint8_t> bitstream;
    NV_ENC_PIC_TYPE pictureType;
    uint64_t timestamp;
    uint32_t frameIdx;
};

class NvEncoderZeroCopy
{
public:
    NvEncoderZeroCopy(CUcontext cuContext, uint32_t nWidth, uint32_t nHeight,
                      NV_ENC_BUFFER_FORMAT eBufferFormat, GUID codecGuid,
                      uint32_t nExtraOutputDelay = 3);

    ~NvEncoderZeroCopy();
    void CreateEncoder(const NV_ENC_INITIALIZE_PARAMS* pInitializeParams);
    void DestroyEncoder();
    void RegisterInputArrays(const std::vector<CUarray>& cudaArrays,
                             unsigned int width, unsigned int height, unsigned int pitch);
    void UnregisterInputArrays();
    void AllocateOutputBuffers(uint32_t numBuffers);
    void ReleaseOutputBuffers();
    NV_ENC_INPUT_PTR MapInputSurface(uint32_t surfaceIdx);
    void UnmapInputSurface(uint32_t surfaceIdx);
    NVENCSTATUS EncodeFrame(uint32_t surfaceIdx, NV_ENC_PIC_PARAMS* pPicParams = nullptr);
    void EndEncode();
    void GetDefaultEncoderParams(NV_ENC_INITIALIZE_PARAMS* pInitializeParams,
                                  GUID presetGuid, NV_ENC_TUNING_INFO tuningInfo);
    uint32_t GetMinOutputDelay() const { return m_nOutputDelay; }
    bool HasBFrames() const { return m_bHasBFrames; }
    void SetIOCudaStreams(NV_ENC_CUSTREAM_PTR inputStream, NV_ENC_CUSTREAM_PTR outputStream);
    NV_ENC_OUTPUT_PTR GetOutputBfr(uint32_t frameIdx) const 
    { 
        return m_vOutputBuffers[frameIdx % m_vOutputBuffers.size()]; 
    }
    void GetLockBitstream(uint32_t frameIdx, NV_ENC_OUTPUT_PTR outputBuffer, ZeroCopyOutputFrame& outputFrame);
    void UnLockBitstream(uint32_t frameIdx, NV_ENC_OUTPUT_PTR outputBuffer);
    uint32_t GetRequiredBufferCount() const;
    uint32_t GetBufferHeight() const;
    void GetSequenceParams(std::vector<uint8_t>& seqParams);

private:
    void LoadNvEncApi();
    void OpenEncodeSession();
    void CloseEncodeSession();
    void ConfigureAV1CustomTiles();

private:
    CUcontext m_cuContext;
    void* m_hEncoder;
    NV_ENCODE_API_FUNCTION_LIST m_nvenc;
    uint32_t m_nWidth;
    uint32_t m_nHeight;
    NV_ENC_BUFFER_FORMAT m_eBufferFormat;
    GUID m_codecGuid;
    uint32_t m_nExtraOutputDelay;
    bool m_bEncoderInitialized;
    NV_ENC_INITIALIZE_PARAMS m_initializeParams;
    NV_ENC_CONFIG m_encodeConfig;
    uint32_t m_nEncoderBufferCount;
    uint32_t m_nOutputDelay;
    bool m_bUseExtCudaArray;
    bool m_bHasBFrames;
    std::vector<ZeroCopyInputSurface> m_vInputSurfaces;
    std::vector<NV_ENC_OUTPUT_PTR> m_vOutputBuffers;
    uint32_t m_nNextOutputIdx;
    std::mutex m_mtxEncoder;
    uint32_t m_av1TileHeights[NV_MAX_TILE_ROWS_AV1];
    uint32_t m_av1TileWidths[NV_MAX_TILE_COLS_AV1];
};

