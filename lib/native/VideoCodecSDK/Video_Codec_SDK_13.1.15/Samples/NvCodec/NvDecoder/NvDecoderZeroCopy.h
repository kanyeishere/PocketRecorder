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
#include <condition_variable>
#include <stdint.h>
#include <string>
#include <sstream>
#include <cuda.h>
#include "nvcuvid.h"

class NvDecoderZeroCopyException : public std::exception
{
public:
    NvDecoderZeroCopyException(const std::string& errorStr, CUresult errorCode)
        : m_errorString(errorStr), m_errorCode(errorCode) {}

    virtual ~NvDecoderZeroCopyException() throw() {}
    virtual const char* what() const throw() { return m_errorString.c_str(); }
    CUresult getErrorCode() const { return m_errorCode; }
    const std::string& getErrorString() const { return m_errorString; }

    static NvDecoderZeroCopyException makeException(const std::string& errorStr, CUresult errorCode,
        const std::string& functionName, const std::string& fileName, int lineNo);

private:
    std::string m_errorString;
    CUresult m_errorCode;
};

inline NvDecoderZeroCopyException NvDecoderZeroCopyException::makeException(
    const std::string& errorStr, CUresult errorCode,
    const std::string& functionName, const std::string& fileName, int lineNo)
{
    std::ostringstream errorLog;
    errorLog << functionName << " : " << errorStr << " at " << fileName << ":" << lineNo << std::endl;
    return NvDecoderZeroCopyException(errorLog.str(), errorCode);
}

#define NVDEC_ZC_THROW_ERROR(errorStr, errorCode) \
    do { \
        throw NvDecoderZeroCopyException::makeException(errorStr, errorCode, __FUNCTION__, __FILE__, __LINE__); \
    } while (0)

#define NVDEC_ZC_API_CALL(cuvidAPI) \
    do { \
        CUresult errorCode = cuvidAPI; \
        if (errorCode != CUDA_SUCCESS) { \
            std::ostringstream errorLog; \
            errorLog << #cuvidAPI << " returned error " << errorCode; \
            throw NvDecoderZeroCopyException::makeException(errorLog.str(), errorCode, __FUNCTION__, __FILE__, __LINE__); \
        } \
    } while (0)

#define CUDA_ZC_CALL(cuAPI) \
    do { \
        CUresult err = cuAPI; \
        if (err != CUDA_SUCCESS) { \
            const char *szErrName = NULL; \
            cuGetErrorName(err, &szErrName); \
            std::ostringstream errorLog; \
            errorLog << #cuAPI << " returned error " << szErrName; \
            throw NvDecoderZeroCopyException::makeException(errorLog.str(), err, __FUNCTION__, __FILE__, __LINE__); \
        } \
    } while (0)

struct ZeroCopyDisplayInfo
{
    int picIdx;
    int64_t timestamp;
    bool isProgressive;
    bool topFieldFirst;
};

class NvDecoderZeroCopy
{
public:
    NvDecoderZeroCopy(CUcontext cuContext, cudaVideoCodec eCodec,
                      CUstream cuStream = nullptr,
                      unsigned int maxWidth = 0, unsigned int maxHeight = 0,
                      bool bLowLatency = false);
    ~NvDecoderZeroCopy();
    void SetExternalOutputArrays(const std::vector<CUarray>& cudaArrays);
    int Decode(const uint8_t* pData, int nSize, int nFlags = 0, int64_t nTimestamp = 0);
    int GetDecodedFrame(ZeroCopyDisplayInfo* pDisplayInfo = nullptr);
    void UnlockFrame(int arrayIndex);

private:
    static int CUDAAPI HandleVideoSequenceProc(void* pUserData, CUVIDEOFORMAT* pVideoFormat);
    static int CUDAAPI HandlePictureDecodeProc(void* pUserData, CUVIDPICPARAMS* pPicParams);
    static int CUDAAPI HandlePictureDisplayProc(void* pUserData, CUVIDPARSERDISPINFO* pDispInfo);
    int HandleVideoSequence(CUVIDEOFORMAT* pVideoFormat);
    int HandlePictureDecode(CUVIDPICPARAMS* pPicParams);
    int HandlePictureDisplay(CUVIDPARSERDISPINFO* pDispInfo);
    void CreateDecoder(CUVIDEOFORMAT* pVideoFormat);
    void DestroyDecoder();

private:
    CUcontext m_cuContext;
    CUstream m_cuStream;
    CUvideodecoder m_hDecoder;
    CUvideoparser m_hParser;
    cudaVideoCodec m_eCodec;
    cudaVideoSurfaceFormat m_eOutputFormat;
    cudaVideoChromaFormat m_eChromaFormat;
    bool m_bLowLatency;
    int m_nWidth;
    int m_nHeight;
    int m_nBitDepthMinus8;
    unsigned int m_nNumDecodeSurfaces;
    unsigned int m_nMaxWidth;
    unsigned int m_nMaxHeight;
    bool m_bUseExtCudaArray;
    std::vector<CUarray> m_vExtCudaArrays;
    unsigned int m_nNumRegisteredSurfaces;
    std::vector<bool> m_vFrameInUse;
    std::mutex m_mtxFrameQueue;
    std::condition_variable m_cvBufferAvailable;

    struct DisplayFrame
    {
        int picIdx;
        int64_t timestamp;
        bool progressive;
        bool topFieldFirst;
    };
    std::vector<DisplayFrame> m_vDisplayQueue;
    CUVIDDECODECREATEINFO m_decoderCreateInfo;
    bool m_bDecoderCreated;
};

