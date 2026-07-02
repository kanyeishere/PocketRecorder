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

#include "NvDecoderZeroCopy.h"
#include <algorithm>
#include <cstring>
#include <chrono>

NvDecoderZeroCopy::NvDecoderZeroCopy(CUcontext cuContext, cudaVideoCodec eCodec,
                                     CUstream cuStream,
                                     unsigned int maxWidth, unsigned int maxHeight,
                                     bool bLowLatency)
    : m_cuContext(cuContext)
    , m_cuStream(cuStream)
    , m_hDecoder(nullptr)
    , m_hParser(nullptr)
    , m_eCodec(eCodec)
    , m_eOutputFormat(cudaVideoSurfaceFormat_NV12)
    , m_eChromaFormat(cudaVideoChromaFormat_420)
    , m_bLowLatency(bLowLatency)
    , m_nWidth(0)
    , m_nHeight(0)
    , m_nBitDepthMinus8(0)
    , m_nNumDecodeSurfaces(0)
    , m_nMaxWidth(maxWidth)
    , m_nMaxHeight(maxHeight)
    , m_bUseExtCudaArray(true)
    , m_nNumRegisteredSurfaces(0)
    , m_bDecoderCreated(false)
{
    memset(&m_decoderCreateInfo, 0, sizeof(m_decoderCreateInfo));

    CUVIDPARSERPARAMS parserParams = {};
    parserParams.CodecType = eCodec;
    parserParams.ulMaxNumDecodeSurfaces = 1;
    parserParams.ulMaxDisplayDelay = bLowLatency ? 0 : 1;
    parserParams.pUserData = this;
    parserParams.pfnSequenceCallback = HandleVideoSequenceProc;
    parserParams.pfnDecodePicture = HandlePictureDecodeProc;
    parserParams.pfnDisplayPicture = HandlePictureDisplayProc;

    NVDEC_ZC_API_CALL(cuvidCreateVideoParser(&m_hParser, &parserParams));
}

NvDecoderZeroCopy::~NvDecoderZeroCopy()
{
    if (m_cuStream)
    {
        cuStreamSynchronize(m_cuStream);
    }
    
    DestroyDecoder();

    if (m_hParser)
    {
        cuvidDestroyVideoParser(m_hParser);
        m_hParser = nullptr;
    }
}

void NvDecoderZeroCopy::SetExternalOutputArrays(const std::vector<CUarray>& cudaArrays)
{
    if (cudaArrays.empty())
    {
        NVDEC_ZC_THROW_ERROR("Empty CUDA array list", CUDA_ERROR_INVALID_VALUE);
    }

    m_vExtCudaArrays = cudaArrays;
    m_nNumRegisteredSurfaces = 0;

    m_vFrameInUse.resize(cudaArrays.size(), false);

    if (m_bDecoderCreated && m_hDecoder)
    {
        CUVIDREGISTERDECODESURFACESINFO dsi = {};
        dsi.ulNumDecodeSurfaces = static_cast<unsigned int>(cudaArrays.size());
        dsi.pDecodeSurfaces = const_cast<CUarray*>(cudaArrays.data());

        CUDA_ZC_CALL(cuCtxPushCurrent(m_cuContext));
        NVDEC_ZC_API_CALL(cuvidRegisterDecodeSurfaces(m_hDecoder, &dsi));
        CUcontext dummy;
        cuCtxPopCurrent(&dummy);
        m_nNumRegisteredSurfaces = static_cast<unsigned int>(cudaArrays.size());
    }
}

int NvDecoderZeroCopy::Decode(const uint8_t* pData, int nSize, int nFlags, int64_t nTimestamp)
{
    if (!m_hParser)
    {
        NVDEC_ZC_THROW_ERROR("Parser not initialized", CUDA_ERROR_NOT_INITIALIZED);
    }

    size_t queueSizeBefore;
    {
        std::lock_guard<std::mutex> lock(m_mtxFrameQueue);
        queueSizeBefore = m_vDisplayQueue.size();
    }

    CUVIDSOURCEDATAPACKET packet = {};
    packet.payload = pData;
    packet.payload_size = static_cast<unsigned long>(nSize);
    packet.flags = nFlags | CUVID_PKT_TIMESTAMP;
    packet.timestamp = nTimestamp;

    if (!pData || nSize == 0)
    {
        packet.flags |= CUVID_PKT_ENDOFSTREAM;
    }

    NVDEC_ZC_API_CALL(cuvidParseVideoData(m_hParser, &packet));

    size_t queueSizeAfter;
    {
        std::lock_guard<std::mutex> lock(m_mtxFrameQueue);
        queueSizeAfter = m_vDisplayQueue.size();
    }
    
    return static_cast<int>(queueSizeAfter - queueSizeBefore);
}

int NvDecoderZeroCopy::GetDecodedFrame(ZeroCopyDisplayInfo* pDisplayInfo)
{
    std::lock_guard<std::mutex> lock(m_mtxFrameQueue);

    if (m_vDisplayQueue.empty())
    {
        return -1;
    }

    DisplayFrame frame = m_vDisplayQueue.front();
    m_vDisplayQueue.erase(m_vDisplayQueue.begin());

    if (pDisplayInfo)
    {
        pDisplayInfo->picIdx = frame.picIdx;
        pDisplayInfo->timestamp = frame.timestamp;
        pDisplayInfo->isProgressive = frame.progressive;
        pDisplayInfo->topFieldFirst = frame.topFieldFirst;
    }

    return frame.picIdx;
}

void NvDecoderZeroCopy::UnlockFrame(int arrayIndex)
{
    {
        std::lock_guard<std::mutex> lock(m_mtxFrameQueue);

        if (arrayIndex >= 0 && static_cast<size_t>(arrayIndex) < m_vFrameInUse.size())
        {
            m_vFrameInUse[arrayIndex] = false;
        }
    }

    m_cvBufferAvailable.notify_all();
}

int CUDAAPI NvDecoderZeroCopy::HandleVideoSequenceProc(void* pUserData, CUVIDEOFORMAT* pVideoFormat)
{
    return reinterpret_cast<NvDecoderZeroCopy*>(pUserData)->HandleVideoSequence(pVideoFormat);
}

int CUDAAPI NvDecoderZeroCopy::HandlePictureDecodeProc(void* pUserData, CUVIDPICPARAMS* pPicParams)
{
    return reinterpret_cast<NvDecoderZeroCopy*>(pUserData)->HandlePictureDecode(pPicParams);
}

int CUDAAPI NvDecoderZeroCopy::HandlePictureDisplayProc(void* pUserData, CUVIDPARSERDISPINFO* pDispInfo)
{
    return reinterpret_cast<NvDecoderZeroCopy*>(pUserData)->HandlePictureDisplay(pDispInfo);
}

int NvDecoderZeroCopy::HandleVideoSequence(CUVIDEOFORMAT* pVideoFormat)
{
    m_nWidth = pVideoFormat->coded_width;
    m_nHeight = pVideoFormat->coded_height;
    m_eChromaFormat = pVideoFormat->chroma_format;
    m_nBitDepthMinus8 = pVideoFormat->bit_depth_luma_minus8;

    if (m_nBitDepthMinus8 > 0)
    {
        switch (m_eChromaFormat)
        {
        case cudaVideoChromaFormat_420:
            m_eOutputFormat = cudaVideoSurfaceFormat_P016;
            break;
        case cudaVideoChromaFormat_422:
            m_eOutputFormat = cudaVideoSurfaceFormat_P216;
            break;
        default:
            m_eOutputFormat = cudaVideoSurfaceFormat_P016;
            break;
        }
    }
    else
    {
        switch (m_eChromaFormat)
        {
        case cudaVideoChromaFormat_420:
            m_eOutputFormat = cudaVideoSurfaceFormat_NV12;
            break;
        case cudaVideoChromaFormat_422:
            m_eOutputFormat = cudaVideoSurfaceFormat_NV16;
            break;
        default:
            m_eOutputFormat = cudaVideoSurfaceFormat_NV12;
            break;
        }
    }

    m_nNumDecodeSurfaces = pVideoFormat->min_num_decode_surfaces;

    if (m_bLowLatency)
    {
        m_nNumDecodeSurfaces = (std::max)(m_nNumDecodeSurfaces, 4u);
    }
    else
    {
        m_nNumDecodeSurfaces = (std::max)(m_nNumDecodeSurfaces + 4, 8u);
    }

    if (m_bDecoderCreated)
    {
        if (m_nWidth != m_decoderCreateInfo.ulWidth ||
            m_nHeight != m_decoderCreateInfo.ulHeight ||
            m_eOutputFormat != (cudaVideoSurfaceFormat)m_decoderCreateInfo.OutputFormat)
        {
            DestroyDecoder();
            CreateDecoder(pVideoFormat);
        }
    }
    else
    {
        CreateDecoder(pVideoFormat);
    }

    return m_nNumDecodeSurfaces;
}

int NvDecoderZeroCopy::HandlePictureDecode(CUVIDPICPARAMS* pPicParams)
{
    if (!m_hDecoder)
    {
        return 0;
    }

    if (m_bUseExtCudaArray)
    {
        if (pPicParams->CurrPicIdx < 0 || 
            static_cast<size_t>(pPicParams->CurrPicIdx) >= m_vExtCudaArrays.size())
        {
            std::ostringstream errorLog;
            errorLog << "Decoder picture index " << pPicParams->CurrPicIdx 
                     << " out of range [0, " << m_vExtCudaArrays.size() - 1 << "]"
                     << " - need more CUDA arrays for this content";
            throw NvDecoderZeroCopyException::makeException(
                errorLog.str(), CUDA_ERROR_INVALID_VALUE, __FUNCTION__, __FILE__, __LINE__);
        }
        
        int picIdxDecode = -1;
        
        if (m_eCodec == cudaVideoCodec_AV1 && 
            pPicParams->CodecSpecific.av1.decodePicIdx >= 0)
        {
            picIdxDecode = pPicParams->CodecSpecific.av1.decodePicIdx;
            
            if (static_cast<size_t>(picIdxDecode) >= m_vExtCudaArrays.size())
            {
                std::ostringstream errorLog;
                errorLog << "AV1 decode picture index " << picIdxDecode 
                         << " out of range [0, " << m_vExtCudaArrays.size() - 1 << "]";
                throw NvDecoderZeroCopyException::makeException(
                    errorLog.str(), CUDA_ERROR_INVALID_VALUE, __FUNCTION__, __FILE__, __LINE__);
            }
        }
        
        {
            std::unique_lock<std::mutex> lock(m_mtxFrameQueue);

            if (!m_cvBufferAvailable.wait_for(lock, std::chrono::milliseconds(5000),
                [this, pPicParams]() {
                    return !m_vFrameInUse[pPicParams->CurrPicIdx];
                }))
            {
                std::ostringstream errorLog;
                errorLog << "Buffer timeout - frame " << pPicParams->CurrPicIdx 
                         << " still in use after 5 seconds";
                throw NvDecoderZeroCopyException::makeException(
                    errorLog.str(), CUDA_ERROR_TIMEOUT, __FUNCTION__, __FILE__, __LINE__);
            }
        }
    }

    CUresult result;
    if (m_bUseExtCudaArray && m_cuStream)
    {
        result = cuvidDecodePictureAsync(m_hDecoder, pPicParams, m_cuStream);
    }
    else
    {
        result = cuvidDecodePicture(m_hDecoder, pPicParams);
    }

    if (result != CUDA_SUCCESS)
    {
        std::ostringstream errorLog;
        errorLog << "cuvidDecodePicture failed with error " << result;
        throw NvDecoderZeroCopyException::makeException(errorLog.str(), result, __FUNCTION__, __FILE__, __LINE__);
    }

    if (m_bUseExtCudaArray)
    {
        std::lock_guard<std::mutex> lock(m_mtxFrameQueue);
        m_vFrameInUse[pPicParams->CurrPicIdx] = true;
    }

    return 1;
}

int NvDecoderZeroCopy::HandlePictureDisplay(CUVIDPARSERDISPINFO* pDispInfo)
{
    if (!pDispInfo)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mtxFrameQueue);

    DisplayFrame frame;
    frame.picIdx = pDispInfo->picture_index;
    frame.timestamp = pDispInfo->timestamp;
    frame.progressive = (pDispInfo->progressive_frame != 0);
    frame.topFieldFirst = (pDispInfo->top_field_first != 0);

    m_vDisplayQueue.push_back(frame);

    return 1;
}

void NvDecoderZeroCopy::CreateDecoder(CUVIDEOFORMAT* pVideoFormat)
{
    if (m_bDecoderCreated)
    {
        DestroyDecoder();
    }

    memset(&m_decoderCreateInfo, 0, sizeof(m_decoderCreateInfo));

    m_decoderCreateInfo.ulWidth = pVideoFormat->coded_width;
    m_decoderCreateInfo.ulHeight = pVideoFormat->coded_height;
    m_decoderCreateInfo.ulTargetWidth = pVideoFormat->coded_width;
    m_decoderCreateInfo.ulTargetHeight = pVideoFormat->coded_height;
    m_decoderCreateInfo.CodecType = pVideoFormat->codec;
    m_decoderCreateInfo.ChromaFormat = pVideoFormat->chroma_format;
    m_decoderCreateInfo.bitDepthMinus8 = pVideoFormat->bit_depth_luma_minus8;
    m_decoderCreateInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    m_decoderCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Adaptive;
    m_decoderCreateInfo.OutputFormat = m_eOutputFormat;
    m_decoderCreateInfo.ulNumOutputSurfaces = m_bUseExtCudaArray ? 0 : 2;

    if (m_bUseExtCudaArray && !m_vExtCudaArrays.empty())
    {
        m_decoderCreateInfo.ulNumDecodeSurfaces = 
            static_cast<unsigned int>(m_vExtCudaArrays.size());
    }
    else
    {
        m_decoderCreateInfo.ulNumDecodeSurfaces = m_nNumDecodeSurfaces;
    }

    m_decoderCreateInfo.ulMaxWidth = (m_nMaxWidth > 0) ? m_nMaxWidth : pVideoFormat->coded_width;
    m_decoderCreateInfo.ulMaxHeight = (m_nMaxHeight > 0) ? m_nMaxHeight : pVideoFormat->coded_height;

    if (m_bUseExtCudaArray)
    {
        switch (m_eOutputFormat)
        {
            case cudaVideoSurfaceFormat_NV12:
                m_decoderCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV12_Opaque;
                break;
            case cudaVideoSurfaceFormat_P016:
                m_decoderCreateInfo.OutputFormat = cudaVideoSurfaceFormat_P016_Opaque;
                break;
            case cudaVideoSurfaceFormat_NV16:
                m_decoderCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV16_Opaque;
                break;
            case cudaVideoSurfaceFormat_P216:
                m_decoderCreateInfo.OutputFormat = cudaVideoSurfaceFormat_P216_Opaque;
                break;
            default:
                m_decoderCreateInfo.OutputFormat = cudaVideoSurfaceFormat_NV12_Opaque;
                break;
        }
    }

    CUDA_ZC_CALL(cuCtxPushCurrent(m_cuContext));
    NVDEC_ZC_API_CALL(cuvidCreateDecoder(&m_hDecoder, &m_decoderCreateInfo));
    CUcontext dummy;
    cuCtxPopCurrent(&dummy);

    m_bDecoderCreated = true;

    if (m_bUseExtCudaArray && !m_vExtCudaArrays.empty() && m_nNumRegisteredSurfaces == 0)
    {
        CUDA_ZC_CALL(cuCtxPushCurrent(m_cuContext));

        CUVIDREGISTERDECODESURFACESINFO dsi = {};
        dsi.ulNumDecodeSurfaces = static_cast<unsigned int>(m_vExtCudaArrays.size());
        dsi.pDecodeSurfaces = m_vExtCudaArrays.data();

        NVDEC_ZC_API_CALL(cuvidRegisterDecodeSurfaces(m_hDecoder, &dsi));
        m_nNumRegisteredSurfaces = static_cast<unsigned int>(m_vExtCudaArrays.size());

        cuCtxPopCurrent(&dummy);
    }
}

void NvDecoderZeroCopy::DestroyDecoder()
{
    if (m_hDecoder)
    {
        cuvidDestroyDecoder(m_hDecoder);
        m_hDecoder = nullptr;
    }
    m_bDecoderCreated = false;
    m_nNumRegisteredSurfaces = 0;
}

