/*
 * This copyright notice applies to this header file only:
 *
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

#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>

#include "../../../Interface/nvcuvid.h"
#include "NvDecoder/NvDecoder.h"

#define START_TIMER auto start = std::chrono::high_resolution_clock::now();

#define STOP_TIMER(print_message) int64_t elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>( \
    std::chrono::high_resolution_clock::now() - start).count(); \
    std::cout << print_message << \
    elapsedTime \
    << " ms " << std::endl;

#define CUDA_DRVAPI_CALL( call )                                                                                                 \
    do                                                                                                                           \
    {                                                                                                                            \
        CUresult err__ = call;                                                                                                   \
        if (err__ != CUDA_SUCCESS)                                                                                               \
        {                                                                                                                        \
            const char *szErrName = NULL;                                                                                        \
            cuGetErrorName(err__, &szErrName);                                                                                   \
            std::ostringstream errorLog;                                                                                         \
            errorLog << "CUDA driver API error " << szErrName ;                                                                  \
            throw NVDECException::makeNVDECException(errorLog.str(), err__, __FUNCTION__, __FILE__, __LINE__);                   \
        }                                                                                                                        \
    }                                                                                                                            \
    while (0)

static const char * GetVideoCodecString(cudaVideoCodec eCodec) {
    static struct {
        cudaVideoCodec eCodec;
        const char *name;
    } aCodecName [] = {
        { cudaVideoCodec_MPEG1,     "MPEG-1"       },
        { cudaVideoCodec_MPEG2,     "MPEG-2"       },
        { cudaVideoCodec_MPEG4,     "MPEG-4 (ASP)" },
        { cudaVideoCodec_VC1,       "VC-1/WMV"     },
        { cudaVideoCodec_H264,      "AVC/H.264"    },
        { cudaVideoCodec_JPEG,      "M-JPEG"       },
        { cudaVideoCodec_H264_SVC,  "H.264/SVC"    },
        { cudaVideoCodec_H264_MVC,  "H.264/MVC"    },
        { cudaVideoCodec_HEVC,      "H.265/HEVC"   },
        { cudaVideoCodec_VP8,       "VP8"          },
        { cudaVideoCodec_VP9,       "VP9"          },
        { cudaVideoCodec_AV1,       "AV1"          },
        { cudaVideoCodec_NumCodecs, "Invalid"      },
        { cudaVideoCodec_YUV420,    "YUV  4:2:0"   },
        { cudaVideoCodec_YV12,      "YV12 4:2:0"   },
        { cudaVideoCodec_NV12,      "NV12 4:2:0"   },
        { cudaVideoCodec_YUYV,      "YUYV 4:2:2"   },
        { cudaVideoCodec_UYVY,      "UYVY 4:2:2"   },
    };

    if (eCodec >= 0 && eCodec <= cudaVideoCodec_NumCodecs) {
        return aCodecName[eCodec].name;
    }
    for (int i = cudaVideoCodec_NumCodecs + 1; i < sizeof(aCodecName) / sizeof(aCodecName[0]); i++) {
        if (eCodec == aCodecName[i].eCodec) {
            return aCodecName[eCodec].name;
        }
    }
    return "Unknown";
}

static const char * GetVideoChromaFormatString(cudaVideoChromaFormat eChromaFormat) {
    static struct {
        cudaVideoChromaFormat eChromaFormat;
        const char *name;
    } aChromaFormatName[] = {
        { cudaVideoChromaFormat_Monochrome, "YUV 400 (Monochrome)" },
        { cudaVideoChromaFormat_420,        "YUV 420"              },
        { cudaVideoChromaFormat_422,        "YUV 422"              },
        { cudaVideoChromaFormat_444,        "YUV 444"              },
    };

    if (eChromaFormat >= 0 && eChromaFormat < sizeof(aChromaFormatName) / sizeof(aChromaFormatName[0])) {
        return aChromaFormatName[eChromaFormat].name;
    }
    return "Unknown";
}

static float GetChromaHeightFactor(cudaVideoSurfaceFormat eSurfaceFormat)
{
    float factor = 0.5;
    switch (eSurfaceFormat)
    {
    case cudaVideoSurfaceFormat_NV12:
    case cudaVideoSurfaceFormat_P016:
        factor = 0.5;
        break;
    case cudaVideoSurfaceFormat_YUV444:
    case cudaVideoSurfaceFormat_YUV444_16Bit:
        factor = 1.0;
        break;
    case cudaVideoSurfaceFormat_NV16:
    case cudaVideoSurfaceFormat_P216:
        factor = 1.0;
        break;
    }

    return factor;
}

static int GetChromaPlaneCount(cudaVideoSurfaceFormat eSurfaceFormat)
{
    int numPlane = 1;
    switch (eSurfaceFormat)
    {
    case cudaVideoSurfaceFormat_NV12:
    case cudaVideoSurfaceFormat_P016:
        numPlane = 1;
        break;
    case cudaVideoSurfaceFormat_YUV444:
    case cudaVideoSurfaceFormat_YUV444_16Bit:
        numPlane = 2;
        break;
    case cudaVideoSurfaceFormat_NV16:
    case cudaVideoSurfaceFormat_P216:
        numPlane = 1;
        break;
    }

    return numPlane;
}

/**
*   @brief  This function is used to get chroma format from surface format
*/
cudaVideoChromaFormat NvDecoder::GetChromaFormat(cudaVideoSurfaceFormat eSurfaceFormat)
{
    cudaVideoChromaFormat format = cudaVideoChromaFormat_420;
    switch (eSurfaceFormat)
    {
    case cudaVideoSurfaceFormat_NV12:
    case cudaVideoSurfaceFormat_P016:
        format = cudaVideoChromaFormat_420;
        break;
    case cudaVideoSurfaceFormat_YUV444:
    case cudaVideoSurfaceFormat_YUV444_16Bit:
        format = cudaVideoChromaFormat_444;
        break;
    case cudaVideoSurfaceFormat_NV16:
    case cudaVideoSurfaceFormat_P216:
        format = cudaVideoChromaFormat_422;
        break;
    }

    return format;
}

/**
*   @brief  This function is used to get codec string from codec id
*/
const char *NvDecoder::GetCodecString(cudaVideoCodec eCodec)
{
    return GetVideoCodecString(eCodec);
}

/* Called when the parser encounters sequence header for AV1 SVC content
*  return value interpretation:
*      < 0 : fail, >=0: succeeded (bit 0-9: currOperatingPoint, bit 10-10: bDispAllLayer, bit 11-30: reserved, must be set 0)
*/
int NvDecoder::GetOperatingPoint(CUVIDOPERATINGPOINTINFO *pOPInfo)
{
    if (pOPInfo->codec == cudaVideoCodec_AV1)
    {
        if (pOPInfo->av1.operating_points_cnt > 1)
        {
            // clip has SVC enabled
            if (m_nOperatingPoint >= pOPInfo->av1.operating_points_cnt)
                m_nOperatingPoint = 0;

            printf("AV1 SVC clip: operating point count %d  ", pOPInfo->av1.operating_points_cnt);
            printf("Selected operating point: %d, IDC 0x%x bOutputAllLayers %d\n", m_nOperatingPoint, pOPInfo->av1.operating_points_idc[m_nOperatingPoint], m_bDispAllLayers);
            return (m_nOperatingPoint | (m_bDispAllLayers << 10));
        }
    }
    return -1;
}

/* Return value from HandleVideoSequence() are interpreted as   :
*  0: fail, 1: succeeded, > 1: override dpb size of parser (set by CUVIDPARSERPARAMS::ulMaxNumDecodeSurfaces while creating parser)
*/
int NvDecoder::HandleVideoSequence(CUVIDEOFORMAT *pVideoFormat)
{
    START_TIMER
    m_videoInfo.str("");
    m_videoInfo.clear();
    m_videoInfo << "Video Input Information" << std::endl
        << "\tCodec        : " << GetVideoCodecString(pVideoFormat->codec) << std::endl
        << "\tFrame rate   : " << pVideoFormat->frame_rate.numerator << "/" << pVideoFormat->frame_rate.denominator
            << " = " << 1.0 * pVideoFormat->frame_rate.numerator / pVideoFormat->frame_rate.denominator << " fps" << std::endl
        << "\tSequence     : " << (pVideoFormat->progressive_sequence ? "Progressive" : "Interlaced") << std::endl
        << "\tCoded size   : [" << pVideoFormat->coded_width << ", " << pVideoFormat->coded_height << "]" << std::endl
        << "\tDisplay area : [" << pVideoFormat->display_area.left << ", " << pVideoFormat->display_area.top << ", "
            << pVideoFormat->display_area.right << ", " << pVideoFormat->display_area.bottom << "]" << std::endl
        << "\tChroma       : " << GetVideoChromaFormatString(pVideoFormat->chroma_format) << std::endl
        << "\tBit depth    : " << pVideoFormat->bit_depth_luma_minus8 + 8
    ;
    m_videoInfo << std::endl;

    int nDecodeSurface = pVideoFormat->min_num_decode_surfaces;

    CUVIDDECODECAPS decodecaps;
    memset(&decodecaps, 0, sizeof(decodecaps));

    decodecaps.eCodecType = pVideoFormat->codec;
    decodecaps.eChromaFormat = pVideoFormat->chroma_format;
    decodecaps.nBitDepthMinus8 = pVideoFormat->bit_depth_luma_minus8;

    CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cuContext));
    NVDEC_API_CALL(cuvidGetDecoderCaps(&decodecaps));
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));

    if(!decodecaps.bIsSupported){
        NVDEC_THROW_ERROR("Codec not supported on this GPU", CUDA_ERROR_NOT_SUPPORTED);
        return nDecodeSurface;
    }

    if ((pVideoFormat->coded_width > decodecaps.nMaxWidth) ||
        (pVideoFormat->coded_height > decodecaps.nMaxHeight)){

        std::ostringstream errorString;
        errorString << std::endl
                    << "Resolution          : " << pVideoFormat->coded_width << "x" << pVideoFormat->coded_height << std::endl
                    << "Max Supported (wxh) : " << decodecaps.nMaxWidth << "x" << decodecaps.nMaxHeight << std::endl
                    << "Resolution not supported on this GPU";

        const std::string cErr = errorString.str();
        NVDEC_THROW_ERROR(cErr, CUDA_ERROR_NOT_SUPPORTED);
        return nDecodeSurface;
    }

    if ((pVideoFormat->coded_width>>4)*(pVideoFormat->coded_height>>4) > decodecaps.nMaxMBCount){

        std::ostringstream errorString;
        errorString << std::endl
                    << "MBCount             : " << (pVideoFormat->coded_width >> 4)*(pVideoFormat->coded_height >> 4) << std::endl
                    << "Max Supported mbcnt : " << decodecaps.nMaxMBCount << std::endl
                    << "MBCount not supported on this GPU";

        const std::string cErr = errorString.str();
        NVDEC_THROW_ERROR(cErr, CUDA_ERROR_NOT_SUPPORTED);
        return nDecodeSurface;
    }

    if (m_nWidth && m_nLumaHeight && m_nChromaHeight) {
        // cuvidCreateDecoder() has been called before, and now there's possible config change
        int result = ReconfigureDecoder(pVideoFormat);
        if (result == 0 || result == 1)
            return result;
        else
            return nDecodeSurface;
    }

    // eCodec has been set in the constructor (for parser). Here it's set again for potential correction
    m_eCodec = pVideoFormat->codec;
    m_eChromaFormat = pVideoFormat->chroma_format;
    m_nBitDepthMinus8 = pVideoFormat->bit_depth_luma_minus8;
    m_nBPP = m_nBitDepthMinus8 > 0 ? 2 : 1;

    // Set the output surface format same as chroma format
    if (m_eChromaFormat == cudaVideoChromaFormat_420 || cudaVideoChromaFormat_Monochrome)
        m_eOutputFormat = pVideoFormat->bit_depth_luma_minus8 ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
    else if (m_eChromaFormat == cudaVideoChromaFormat_444)
        m_eOutputFormat = pVideoFormat->bit_depth_luma_minus8 ? cudaVideoSurfaceFormat_YUV444_16Bit : cudaVideoSurfaceFormat_YUV444;
    else if (m_eChromaFormat == cudaVideoChromaFormat_422)
        m_eOutputFormat = pVideoFormat->bit_depth_luma_minus8 ? cudaVideoSurfaceFormat_P216 : cudaVideoSurfaceFormat_NV16;

    // Check if output format is supported. If not, check falback options
    if (!(decodecaps.nOutputFormatMask & (1 << m_eOutputFormat)))
    {
        if (decodecaps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_NV12))
            m_eOutputFormat = cudaVideoSurfaceFormat_NV12;
        else if (decodecaps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_P016))
            m_eOutputFormat = cudaVideoSurfaceFormat_P016;
        else if (decodecaps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_YUV444))
            m_eOutputFormat = cudaVideoSurfaceFormat_YUV444;
        else if (decodecaps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_YUV444_16Bit))
            m_eOutputFormat = cudaVideoSurfaceFormat_YUV444_16Bit;
        else if (decodecaps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_NV16))
            m_eOutputFormat = cudaVideoSurfaceFormat_NV16;
        else if (decodecaps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_P216))
            m_eOutputFormat = cudaVideoSurfaceFormat_P216;
        else 
            NVDEC_THROW_ERROR("No supported output format found", CUDA_ERROR_NOT_SUPPORTED);
    }
    m_videoFormat = *pVideoFormat;

    CUVIDDECODECREATEINFO videoDecodeCreateInfo = { 0 };
    videoDecodeCreateInfo.CodecType = pVideoFormat->codec;
    videoDecodeCreateInfo.ChromaFormat = pVideoFormat->chroma_format;
    videoDecodeCreateInfo.OutputFormat = m_eOutputFormat;
    videoDecodeCreateInfo.bitDepthMinus8 = pVideoFormat->bit_depth_luma_minus8;
    if (pVideoFormat->progressive_sequence)
        videoDecodeCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    else
        videoDecodeCreateInfo.DeinterlaceMode = cudaVideoDeinterlaceMode_Adaptive;
    videoDecodeCreateInfo.ulNumOutputSurfaces = 2;
    // With PreferCUVID, JPEG is still decoded by CUDA while video is decoded by NVDEC hardware
    videoDecodeCreateInfo.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    if (m_nNumDecSurfaces == 0 || m_nNumDecSurfaces > nDecodeSurface)
        m_nNumDecSurfaces = nDecodeSurface;

    videoDecodeCreateInfo.ulNumDecodeSurfaces = m_nNumDecSurfaces;
    videoDecodeCreateInfo.vidLock = m_ctxLock;
    videoDecodeCreateInfo.ulWidth = pVideoFormat->coded_width;
    videoDecodeCreateInfo.ulHeight = pVideoFormat->coded_height;
    
    // Enable decode stats if requested
    if (m_bEnableDecodeStats)
    {
        if (decodecaps.bIsDecodeStatsSupported)
        {
            videoDecodeCreateInfo.enableDecodeFeatures = (1 << cuvidDecodeFeature_DecStats);
        }
        else
        {
            printf("Warning: Decode stats are not supported. Ignoring decode stats request.\n");
            m_bEnableDecodeStats = false;
        }
    }
    
    // AV1 has max width/height of sequence in sequence header
    if (pVideoFormat->codec == cudaVideoCodec_AV1 && pVideoFormat->seqhdr_data_length > 0)
    {
        // dont overwrite if it is already set from cmdline or reconfig.txt
        if (!(m_nMaxWidth > pVideoFormat->coded_width || m_nMaxHeight > pVideoFormat->coded_height))
        {
            CUVIDEOFORMATEX *vidFormatEx = (CUVIDEOFORMATEX *)pVideoFormat;
            m_nMaxWidth = vidFormatEx->av1.max_width;
            m_nMaxHeight = vidFormatEx->av1.max_height;
        }
    }
    if (m_nMaxWidth < (int)pVideoFormat->coded_width)
        m_nMaxWidth = pVideoFormat->coded_width;
    if (m_nMaxHeight < (int)pVideoFormat->coded_height)
        m_nMaxHeight = pVideoFormat->coded_height;
    videoDecodeCreateInfo.ulMaxWidth = m_nMaxWidth;
    videoDecodeCreateInfo.ulMaxHeight = m_nMaxHeight;

    if (!(m_cropRect.r && m_cropRect.b) && !(m_resizeDim.w && m_resizeDim.h)) {
        m_nWidth = pVideoFormat->display_area.right - pVideoFormat->display_area.left;
        m_nLumaHeight = pVideoFormat->display_area.bottom - pVideoFormat->display_area.top;
        videoDecodeCreateInfo.ulTargetWidth = pVideoFormat->coded_width;
        videoDecodeCreateInfo.ulTargetHeight = pVideoFormat->coded_height;
    } else {
        if (m_resizeDim.w && m_resizeDim.h) {
            videoDecodeCreateInfo.display_area.left = pVideoFormat->display_area.left;
            videoDecodeCreateInfo.display_area.top = pVideoFormat->display_area.top;
            videoDecodeCreateInfo.display_area.right = pVideoFormat->display_area.right;
            videoDecodeCreateInfo.display_area.bottom = pVideoFormat->display_area.bottom;
            m_nWidth = m_resizeDim.w;
            m_nLumaHeight = m_resizeDim.h;
        }

        if (m_cropRect.r && m_cropRect.b) {
            videoDecodeCreateInfo.display_area.left = m_cropRect.l;
            videoDecodeCreateInfo.display_area.top = m_cropRect.t;
            videoDecodeCreateInfo.display_area.right = m_cropRect.r;
            videoDecodeCreateInfo.display_area.bottom = m_cropRect.b;
            m_nWidth = m_cropRect.r - m_cropRect.l;
            m_nLumaHeight = m_cropRect.b - m_cropRect.t;
        }
        videoDecodeCreateInfo.ulTargetWidth = m_nWidth;
        videoDecodeCreateInfo.ulTargetHeight = m_nLumaHeight;
    }

    m_nChromaHeight = (int)(ceil(m_nLumaHeight * GetChromaHeightFactor(m_eOutputFormat)));
    m_nNumChromaPlanes = GetChromaPlaneCount(m_eOutputFormat);
    m_nSurfaceHeight = videoDecodeCreateInfo.ulTargetHeight;
    m_nSurfaceWidth = videoDecodeCreateInfo.ulTargetWidth;
    m_displayRect.b = videoDecodeCreateInfo.display_area.bottom;
    m_displayRect.t = videoDecodeCreateInfo.display_area.top;
    m_displayRect.l = videoDecodeCreateInfo.display_area.left;
    m_displayRect.r = videoDecodeCreateInfo.display_area.right;

    m_videoInfo << "Video Decoding Params:" << std::endl
        << "\tNum Surfaces : " << videoDecodeCreateInfo.ulNumDecodeSurfaces << std::endl
        << "\tCrop         : [" << videoDecodeCreateInfo.display_area.left << ", " << videoDecodeCreateInfo.display_area.top << ", "
        << videoDecodeCreateInfo.display_area.right << ", " << videoDecodeCreateInfo.display_area.bottom << "]" << std::endl
        << "\tResize       : " << videoDecodeCreateInfo.ulTargetWidth << "x" << videoDecodeCreateInfo.ulTargetHeight << std::endl
        << "\tDeinterlace  : " << std::vector<const char *>{"Weave", "Bob", "Adaptive"}[videoDecodeCreateInfo.DeinterlaceMode]
    ;
    m_videoInfo << std::endl;

    CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cuContext));
    NVDEC_API_CALL(cuvidCreateDecoder(&m_hDecoder, &videoDecodeCreateInfo));
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
    STOP_TIMER("Session Initialization Time: ");
    return nDecodeSurface;
}

int NvDecoder::ReconfigureDecoder(CUVIDEOFORMAT *pVideoFormat)
{
    if (pVideoFormat->bit_depth_luma_minus8 != m_videoFormat.bit_depth_luma_minus8 || pVideoFormat->bit_depth_chroma_minus8 != m_videoFormat.bit_depth_chroma_minus8){

        NVDEC_THROW_ERROR("Reconfigure Not supported for bit depth change", CUDA_ERROR_NOT_SUPPORTED);
    }

    if (pVideoFormat->chroma_format != m_videoFormat.chroma_format) {

        NVDEC_THROW_ERROR("Reconfigure Not supported for chroma format change", CUDA_ERROR_NOT_SUPPORTED);
    }

    bool bDecodeResChange = !(pVideoFormat->coded_width == m_videoFormat.coded_width && pVideoFormat->coded_height == m_videoFormat.coded_height);
    bool bDisplayRectChange = !(pVideoFormat->display_area.bottom == m_videoFormat.display_area.bottom && pVideoFormat->display_area.top == m_videoFormat.display_area.top \
        && pVideoFormat->display_area.left == m_videoFormat.display_area.left && pVideoFormat->display_area.right == m_videoFormat.display_area.right);

    if ((pVideoFormat->coded_width > m_nMaxWidth) || (pVideoFormat->coded_height > m_nMaxHeight)) {
        // For VP9, let driver  handle the change if new width/height > maxwidth/maxheight
        if ((m_eCodec != cudaVideoCodec_VP9) || m_bReconfigExternal)
        {
            NVDEC_THROW_ERROR("Reconfigure Not supported when width/height > maxwidth/maxheight", CUDA_ERROR_NOT_SUPPORTED);
        }
        return 1;
    }

    if (!bDecodeResChange && !m_bReconfigExtPPChange && !m_bNumSurfacesChange) {
        // if the coded_width/coded_height hasn't changed but display resolution has changed, then need to update width/height for
        // correct output without cropping. Example : 1920x1080 vs 1920x1088
        if (bDisplayRectChange)
        {
            m_nWidth = pVideoFormat->display_area.right - pVideoFormat->display_area.left;
            m_nLumaHeight = pVideoFormat->display_area.bottom - pVideoFormat->display_area.top;
            m_nChromaHeight = (int)ceil(m_nLumaHeight * GetChromaHeightFactor(m_eOutputFormat));
            m_nNumChromaPlanes = GetChromaPlaneCount(m_eOutputFormat);
        }

        // no need for reconfigureDecoder(). Just return
        return 1;
    }

    CUVIDRECONFIGUREDECODERINFO reconfigParams = { 0 };

    reconfigParams.ulWidth = m_videoFormat.coded_width = pVideoFormat->coded_width;
    reconfigParams.ulHeight = m_videoFormat.coded_height = pVideoFormat->coded_height;

    // Dont change display rect and get scaled output from decoder. This will help display app to present apps smoothly
    reconfigParams.display_area.bottom = m_displayRect.b;
    reconfigParams.display_area.top = m_displayRect.t;
    reconfigParams.display_area.left = m_displayRect.l;
    reconfigParams.display_area.right = m_displayRect.r;
    reconfigParams.ulTargetWidth = m_nSurfaceWidth;
    reconfigParams.ulTargetHeight = m_nSurfaceHeight;

    // If external reconfigure is called along with resolution change even if post processing params is not changed,
    // do full reconfigure params update
    if ((m_bReconfigExternal && bDecodeResChange) || m_bReconfigExtPPChange) {
        // update display rect and target resolution if requested explicitely
        m_bReconfigExternal = false;
        m_bReconfigExtPPChange = false;
        m_videoFormat = *pVideoFormat;
        if (!(m_cropRect.r && m_cropRect.b) && !(m_resizeDim.w && m_resizeDim.h)) {
            m_nWidth = pVideoFormat->display_area.right - pVideoFormat->display_area.left;
            m_nLumaHeight = pVideoFormat->display_area.bottom - pVideoFormat->display_area.top;
            reconfigParams.ulTargetWidth = pVideoFormat->coded_width;
            reconfigParams.ulTargetHeight = pVideoFormat->coded_height;
        }
        else {
            if (m_resizeDim.w && m_resizeDim.h) {
                reconfigParams.display_area.left = pVideoFormat->display_area.left;
                reconfigParams.display_area.top = pVideoFormat->display_area.top;
                reconfigParams.display_area.right = pVideoFormat->display_area.right;
                reconfigParams.display_area.bottom = pVideoFormat->display_area.bottom;
                m_nWidth = m_resizeDim.w;
                m_nLumaHeight = m_resizeDim.h;
            }

            if (m_cropRect.r && m_cropRect.b) {
                reconfigParams.display_area.left = m_cropRect.l;
                reconfigParams.display_area.top = m_cropRect.t;
                reconfigParams.display_area.right = m_cropRect.r;
                reconfigParams.display_area.bottom = m_cropRect.b;
                m_nWidth = m_cropRect.r - m_cropRect.l;
                m_nLumaHeight = m_cropRect.b - m_cropRect.t;
            }
            reconfigParams.ulTargetWidth = m_nWidth;
            reconfigParams.ulTargetHeight = m_nLumaHeight;
        }

        m_nChromaHeight = (int)ceil(m_nLumaHeight * GetChromaHeightFactor(m_eOutputFormat));
        m_nNumChromaPlanes = GetChromaPlaneCount(m_eOutputFormat);
        m_nSurfaceHeight = reconfigParams.ulTargetHeight;
        m_nSurfaceWidth = reconfigParams.ulTargetWidth;
        m_displayRect.b = reconfigParams.display_area.bottom;
        m_displayRect.t = reconfigParams.display_area.top;
        m_displayRect.l = reconfigParams.display_area.left;
        m_displayRect.r = reconfigParams.display_area.right;
    }

    if (!m_bMemoryOptimize && (m_nNumDecSurfaces < pVideoFormat->min_num_decode_surfaces))
    {
        m_nNumDecSurfaces = pVideoFormat->min_num_decode_surfaces;
        m_bNumSurfacesChange = true;
    }
    reconfigParams.ulNumDecodeSurfaces = m_nNumDecSurfaces;

    START_TIMER
    CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cuContext));
    CUresult result = cuvidReconfigureDecoder(m_hDecoder, &reconfigParams);
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
    STOP_TIMER("Session Reconfigure Time: ");

    if (result == CUDA_SUCCESS) {
        m_bNumSurfacesChange = false;
        
        // Clear locked frames after successful reconfiguration
        if (!m_LockedFrameData.empty()) {
            m_LockedFrameData.clear();
        }
        
        return m_nNumDecSurfaces;
    }
    else {
        printf("Failed to Reconfigure Decoder\n");
        return 0;
    }
}

int NvDecoder::setReconfigParams(const Rect *pCropRect, const Dim *pResizeDim)
{
    m_bReconfigExternal = true;
    m_bReconfigExtPPChange = false;
    if (pCropRect)
    {
        if (!((pCropRect->t == m_cropRect.t) && (pCropRect->l == m_cropRect.l) &&
            (pCropRect->b == m_cropRect.b) && (pCropRect->r == m_cropRect.r)))
        {
            m_bReconfigExtPPChange = true;
            m_cropRect = *pCropRect;
        }
    }
    if (pResizeDim)
    {
        if (!((pResizeDim->w == m_resizeDim.w) && (pResizeDim->h == m_resizeDim.h)))
        {
            m_bReconfigExtPPChange = true;
            m_resizeDim = *pResizeDim;
        }
    }

    // Clear existing output buffers of different size
    uint8_t *pFrame = NULL;
    while (!m_vpFrame.empty())
    {
        pFrame = m_vpFrame.back();
        m_vpFrame.pop_back();
        if (m_bUseDeviceFrame)
        {
            CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cuContext));
            CUDA_DRVAPI_CALL(cuMemFree((CUdeviceptr)pFrame));
            CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
        }
        else
        {
            delete pFrame;
        }
    }

    // Clear existing decode stats buffers
    for (uint8_t *pDecodeStats : m_vpDecodeStats)
    {
        if (pDecodeStats)
        {
            delete[] pDecodeStats;
        }
    }
    m_vpDecodeStats.clear();
    m_vDecodeStatsSize.clear();
    
    // Clear timestamp and skip vectors to keep them in sync with frame buffers
    m_vTimestamp.clear();
    m_vSkipped.clear();
    
    // Clear locked frames during reconfiguration
    if (!m_LockedFrameData.empty()) {
        m_LockedFrameData.clear();
    }
    
    // Reset frame counters
    m_nDecodedFrame = 0;
    m_nDecodedFrameReturned = 0;

    return 1;
}

uint8_t* NvDecoder::AllocateFrameBuffer() {
    uint8_t *pFrame = NULL;
    m_nFrameAlloc++; // increment frame alloc count
    if (m_bUseDeviceFrame)
    {
        if (m_bDeviceFramePitched)
        {
            CUDA_DRVAPI_CALL(cuMemAllocPitch((CUdeviceptr *)&pFrame, &m_nDeviceFramePitch, 
                GetWidth() * m_nBPP, m_nLumaHeight + (m_nChromaHeight * m_nNumChromaPlanes), 16));
        }
        else
        {
            CUDA_DRVAPI_CALL(cuMemAlloc((CUdeviceptr *)&pFrame, GetFrameSize()));
        }
    }
    else
    {
        pFrame = new uint8_t[GetFrameSize()];
    }
    if (m_nFrameAlloc > 128) {
        printf("Warning: Too many frames getting allocated (%d), may cause out of memory issues.\n", m_nFrameAlloc);
    }
    return pFrame;
}

/* Return value from HandlePictureDecode() are interpreted as:
*  0: fail, >=1: succeeded
*/
int NvDecoder::HandlePictureDecode(CUVIDPICPARAMS *pPicParams) {
    if (!m_hDecoder)
    {
        NVDEC_THROW_ERROR("Decoder not initialized.", CUDA_ERROR_NOT_INITIALIZED);
        return false;
    }

    // Bounds check for array access
    if (pPicParams->CurrPicIdx < 0 || pPicParams->CurrPicIdx >= MAX_FRM_CNT)
    {
        return false;
    }

    if (m_bMemoryOptimize)
    {
        int PicIdx = pPicParams->CurrPicIdx;
        if (m_eCodec == cudaVideoCodec_AV1)
        {
            if (pPicParams->CodecSpecific.av1.decodePicIdx > PicIdx)
                PicIdx = pPicParams->CodecSpecific.av1.decodePicIdx;
        }
        else if (m_eCodec == cudaVideoCodec_VC1)
        {
            if (pPicParams->CodecSpecific.vc1.ForwardRefIdx > PicIdx)
                PicIdx = pPicParams->CodecSpecific.vc1.ForwardRefIdx;
            if (pPicParams->CodecSpecific.vc1.BackwardRefIdx > PicIdx)
                PicIdx = pPicParams->CodecSpecific.vc1.BackwardRefIdx;
        }

        if (PicIdx >= m_nNumDecSurfaces)
        {
            // Increase the number of decode surfaces through ReconfigureDecoder
            int iterations = 1000;
            CUVIDEOFORMAT videoFormat = m_videoFormat;

            m_nNumDecSurfaces = PicIdx + 1;
            m_bNumSurfacesChange = true;

            while (iterations != 0)
            {
                if (m_nNumDecSurfaces == ReconfigureDecoder(&m_videoFormat))
                    break;

                NvSleep(1); // Wait for 1 msec before retrying
                --iterations;
            }

            if (iterations == 0)
                return false;
        }
    }

    m_nPicNumInDecodeOrder[pPicParams->CurrPicIdx] = m_nDecodePicCnt++;
    CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cuContext));
    // Skip decoding non-reference frames that are before the seek point
    bool bSkip = !pPicParams->ref_pic_flag && (pPicParams->timestamp != 0) && (pPicParams->timestamp < m_nSeekPts);
    if (!bSkip)
    {
        NVDEC_API_CALL(cuvidDecodePicture(m_hDecoder, pPicParams));
    }
    if (m_bForce_zero_latency && ((!pPicParams->field_pic_flag) || (pPicParams->second_field)))
    {
        CUVIDPARSERDISPINFO dispInfo;
        memset(&dispInfo, 0, sizeof(dispInfo));
        dispInfo.picture_index = pPicParams->CurrPicIdx;
        dispInfo.progressive_frame = !pPicParams->field_pic_flag;
        dispInfo.top_field_first = pPicParams->bottom_field_flag ^ 1;
        dispInfo.timestamp = pPicParams->timestamp;  // Copy timestamp from decode params
        HandlePictureDisplay(&dispInfo);
    }
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
    return 1;
}

/* Return value from HandlePictureDisplay() are interpreted as:
*  0: fail, >=1: succeeded
*/
int NvDecoder::HandlePictureDisplay(CUVIDPARSERDISPINFO *pDispInfo) {
    if (pDispInfo->picture_index < 0 || pDispInfo->picture_index >= MAX_FRM_CNT)
    {
        return 0;
    }

    CUVIDPROCPARAMS videoProcessingParameters = {};
    videoProcessingParameters.progressive_frame = pDispInfo->progressive_frame;
    videoProcessingParameters.second_field = pDispInfo->repeat_first_field + 1;
    videoProcessingParameters.top_field_first = pDispInfo->top_field_first;
    videoProcessingParameters.unpaired_field = pDispInfo->repeat_first_field < 0;
    videoProcessingParameters.output_stream = m_cuvidStream;

    // Add VPPExt structure for decode stats if enabled
    CUVIDPROCPARAMSEXT videoProcessingParametersExt = {};
    if (m_bEnableDecodeStats)
    {
        videoProcessingParametersExt.decode_stats_dptr = &m_decodeStatsDptr;
        videoProcessingParameters.pCuvidProcExt = &videoProcessingParametersExt;
    }

    if (m_bExtractSEIMessage)
    {
        for (int field = 0; field < 2; field++)
        {
            if (m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIData)
            {
                // Write SEI Message
                uint8_t *seiBuffer = (uint8_t *)(m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIData);
                uint32_t seiNumMessages = m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].sei_message_count;
                CUSEIMESSAGE *seiMessagesInfo = m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIMessage;
                if (m_fpSEI)
                {
                    for (uint32_t i = 0; i < seiNumMessages; i++)
                    {
                        if ((m_eCodec == cudaVideoCodec_H264) ||
                            (m_eCodec == cudaVideoCodec_H264_SVC) ||
                            (m_eCodec == cudaVideoCodec_H264_MVC) ||
                            (m_eCodec == cudaVideoCodec_HEVC) ||
                            (m_eCodec == cudaVideoCodec_MPEG2))
                        {    
                            switch (seiMessagesInfo[i].sei_message_type)
                            {
                                case SEI_TYPE_TIME_CODE:
                                case SEI_TYPE_TIME_CODE_H264:
                                {
                                    if (m_eCodec != cudaVideoCodec_MPEG2)
                                    {
                                        TIMECODE *timecode = (TIMECODE *)seiBuffer;
                                        fwrite(timecode, sizeof(TIMECODE), 1, m_fpSEI);
                                    }
                                    else
                                    {
                                        TIMECODEMPEG2 *timecode = (TIMECODEMPEG2 *)seiBuffer;
                                        fwrite(timecode, sizeof(TIMECODEMPEG2), 1, m_fpSEI);
                                    }
                                }
                                break;
                                case SEI_TYPE_USER_DATA_REGISTERED:
                                case SEI_TYPE_USER_DATA_UNREGISTERED:
                                {
                                    fwrite(seiBuffer, seiMessagesInfo[i].sei_message_size, 1, m_fpSEI);
                                }
                                break;
                                case SEI_TYPE_MASTERING_DISPLAY_COLOR_VOLUME:
                                {
                                    SEIMASTERINGDISPLAYINFO *masteringDisplayVolume = (SEIMASTERINGDISPLAYINFO *)seiBuffer;
                                    fwrite(masteringDisplayVolume, sizeof(SEIMASTERINGDISPLAYINFO), 1, m_fpSEI);
                                }
                                break;
                                case SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO:
                                {
                                    SEICONTENTLIGHTLEVELINFO *contentLightLevelInfo = (SEICONTENTLIGHTLEVELINFO *)seiBuffer;
                                    fwrite(&contentLightLevelInfo->max_content_light_level, sizeof(contentLightLevelInfo->max_content_light_level), 1, m_fpSEI);
                                    fwrite(&contentLightLevelInfo->max_pic_average_light_level, sizeof(contentLightLevelInfo->max_pic_average_light_level), 1, m_fpSEI);
                                }
                                break;
                                case SEI_TYPE_ALTERNATIVE_TRANSFER_CHARACTERISTICS:
                                {
                                    SEIALTERNATIVETRANSFERCHARACTERISTICS *transferCharacteristics = (SEIALTERNATIVETRANSFERCHARACTERISTICS *)seiBuffer;
                                    fwrite(transferCharacteristics, sizeof(SEIALTERNATIVETRANSFERCHARACTERISTICS), 1, m_fpSEI);
                                }
                                break;
                            }
                        }
                        if (m_eCodec == cudaVideoCodec_AV1)
                        {
                            fwrite(seiBuffer, seiMessagesInfo[i].sei_message_size, 1, m_fpSEI);
                        }    
                        seiBuffer += seiMessagesInfo[i].sei_message_size;
                    }
                }
                free(m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIData);
                free(m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIMessage);
                m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIData = NULL;
                m_SEIMessagesDisplayOrder[pDispInfo->picture_index][field].pSEIMessage = NULL;
            }
        }
    }

    // Check decode status for ALL frames (even those skipped by seekPTS)
    CUVIDGETDECODESTATUS DecodeStatus;
    memset(&DecodeStatus, 0, sizeof(DecodeStatus));
    CUresult result = cuvidGetDecodeStatus(m_hDecoder, pDispInfo->picture_index, &DecodeStatus);
    if (result == CUDA_SUCCESS)
    {
        if (DecodeStatus.decodeStatus == cuvidDecodeStatus_Error || DecodeStatus.decodeStatus == cuvidDecodeStatus_Error_Concealed)
        {
            printf("Decode Error occurred for picture %d\n", m_nPicNumInDecodeOrder[pDispInfo->picture_index]);
        }
    }

    // When m_nSeekPts is set, mark frames with timestamp < m_nSeekPts as skipped
    bool bSkip = (m_nSeekPts != 0 && pDispInfo->timestamp != 0 && (uint64_t)pDispInfo->timestamp < m_nSeekPts);
    if(bSkip)
    {
        if ((unsigned)++m_nDecodedFrame > m_vpFrame.size())
        {
            m_vpFrame.push_back(nullptr);
            m_vTimestamp.push_back(0);
            m_vSkipped.push_back(true);
            if (m_bEnableDecodeStats)
            {
                m_vpDecodeStats.push_back(nullptr);
                m_vDecodeStatsSize.push_back(0);
            }
        }
        m_vTimestamp[m_nDecodedFrame - 1] = pDispInfo->timestamp;
        m_vSkipped[m_nDecodedFrame - 1] = true;
        if (m_bEnableDecodeStats)
        {
            m_vDecodeStatsSize[m_nDecodedFrame - 1] = 0;
        }
        return 1;
    }
    // Process frame (even if skipped, to keep frame counting accurate)
    CUdeviceptr dpSrcFrame = 0;
    unsigned int nSrcPitch = 0;
    unsigned int decodeStatsSize = 0;  // Track decode stats size from driver

    CUDA_DRVAPI_CALL(cuCtxPushCurrent(m_cuContext));
    NVDEC_API_CALL(cuvidMapVideoFrame(m_hDecoder, pDispInfo->picture_index, &dpSrcFrame,
            &nSrcPitch, &videoProcessingParameters));

    uint8_t *pDecodedFrame = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtxVPFrame);
        if ((unsigned)++m_nDecodedFrame > m_vpFrame.size())
        {
            uint8_t *pFrame = AllocateFrameBuffer();
            if (pFrame == nullptr) {
                throw std::runtime_error("Failed to allocate frame buffer");
            }
            m_vpFrame.push_back(pFrame);
        }
        // If frame buffer is not allocated, allocate it now. 
        // This is to handle the case where the frame buffer is not allocated in the skip case.
        if (m_vpFrame[m_nDecodedFrame - 1] == nullptr) {
            m_vpFrame[m_nDecodedFrame - 1] = AllocateFrameBuffer();
            if (m_vpFrame[m_nDecodedFrame - 1] == nullptr) {
                throw std::runtime_error("Failed to allocate frame buffer");
            }
        }
        pDecodedFrame = m_vpFrame[m_nDecodedFrame - 1];

        if ((int)m_vTimestamp.size() < m_nDecodedFrame) {
            m_vTimestamp.resize(m_vpFrame.size());
        }
        m_vTimestamp[m_nDecodedFrame - 1] = pDispInfo->timestamp;
        
        // Store skip flag (resize if needed)
        if ((int)m_vSkipped.size() < m_nDecodedFrame) {
            m_vSkipped.resize(m_vpFrame.size());
        }
        m_vSkipped[m_nDecodedFrame - 1] = bSkip;
        
        // Handle decode stats allocation if enabled
        if (m_bEnableDecodeStats)
        {    
            // Always allocate stats buffer to keep vectors in sync
            // Use decodeStatsSize from driver if available, otherwise calculate
            decodeStatsSize = videoProcessingParametersExt.decode_stats_size;
            if (decodeStatsSize == 0)
            {
                // Use fallback calculation to keep vector sizes in sync
                int mb_width = (GetDecodeWidth() + 15) >> 4;
                int mb_height = (GetHeight() + 15) >> 4;
                decodeStatsSize = sizeof(CUVIDDECODESTATSHEADER) + mb_width * mb_height * sizeof(CUVIDDECODESTATS);
            }

            if ((unsigned)m_nDecodedFrame > m_vpDecodeStats.size())
            {
                uint8_t* pDecodeStats = new uint8_t[decodeStatsSize];
                if (pDecodeStats == nullptr) {
                    throw std::runtime_error("Failed to allocate decode stats buffer");
                }
                m_vpDecodeStats.push_back(pDecodeStats);
                m_vDecodeStatsSize.resize(m_vpFrame.size());  
            }

            // If decode stats buffer is not allocated, allocate it now.
            // This is to handle the case where the decode stats buffer is not allocated in the skip case.
            if (m_vpDecodeStats[m_nDecodedFrame - 1] == nullptr) {
                m_vpDecodeStats[m_nDecodedFrame - 1] = new uint8_t[decodeStatsSize];
                if (m_vpDecodeStats[m_nDecodedFrame - 1] == nullptr) {
                    throw std::runtime_error("Failed to allocate decode stats buffer");
                }
            }
            // Update size for reused buffer slot
            m_vDecodeStatsSize[m_nDecodedFrame - 1] = decodeStatsSize;
        }
    }

    // Copy luma plane
    CUDA_MEMCPY2D m = { 0 };
    m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    m.srcDevice = dpSrcFrame;
    m.srcPitch = nSrcPitch;
    m.dstMemoryType = m_bUseDeviceFrame ? CU_MEMORYTYPE_DEVICE : CU_MEMORYTYPE_HOST;
    m.dstDevice = (CUdeviceptr)(m.dstHost = pDecodedFrame);
    m.dstPitch = m_nDeviceFramePitch ? m_nDeviceFramePitch : GetWidth() * m_nBPP;
    m.WidthInBytes = GetWidth() * m_nBPP;
    m.Height = m_nLumaHeight;
    CUDA_DRVAPI_CALL(cuMemcpy2DAsync(&m, m_cuvidStream));

    // Copy chroma plane
    // NVDEC output has luma height aligned by 2. Adjust chroma offset by aligning height
    m.srcDevice = (CUdeviceptr)((uint8_t *)dpSrcFrame + m.srcPitch * ((m_nSurfaceHeight + 1) & ~1));
    m.dstDevice = (CUdeviceptr)(m.dstHost = pDecodedFrame + m.dstPitch * m_nLumaHeight);
    m.Height = m_nChromaHeight;
    CUDA_DRVAPI_CALL(cuMemcpy2DAsync(&m, m_cuvidStream));

    if (m_nNumChromaPlanes == 2)
    {
        m.srcDevice = (CUdeviceptr)((uint8_t *)dpSrcFrame + m.srcPitch * ((m_nSurfaceHeight + 1) & ~1) * 2);
        m.dstDevice = (CUdeviceptr)(m.dstHost = pDecodedFrame + m.dstPitch * m_nLumaHeight * 2);
        m.Height = m_nChromaHeight;
        CUDA_DRVAPI_CALL(cuMemcpy2DAsync(&m, m_cuvidStream));
    }
    if(!m_bExternalStream)
    {
        CUDA_DRVAPI_CALL(cuStreamSynchronize(m_cuvidStream));
    }
    CUDA_DRVAPI_CALL(cuCtxPopCurrent(NULL));
    // Handle decode stats if enabled
    if (m_bEnableDecodeStats && m_decodeStatsDptr != 0)
    {    
        uint8_t* pDecodeStats = m_vpDecodeStats[m_nDecodedFrame - 1];
        CUDA_DRVAPI_CALL(cuMemcpyDtoH(pDecodeStats, m_decodeStatsDptr, decodeStatsSize));
    }

    NVDEC_API_CALL(cuvidUnmapVideoFrame(m_hDecoder, dpSrcFrame));
    return 1;
}

int NvDecoder::GetSEIMessage(CUVIDSEIMESSAGEINFO *pSEIMessageInfo)
{
    uint32_t seiNumMessages = pSEIMessageInfo->sei_message_count;
    CUSEIMESSAGE *seiMessagesInfo = pSEIMessageInfo->pSEIMessage;
    size_t totalSEIBufferSize = 0;
    if (pSEIMessageInfo->picIdx >= MAX_FRM_CNT)
    {
        printf("Invalid picture index (%u)\n", pSEIMessageInfo->picIdx);
        return 0;
    }
    for (uint32_t i = 0; i < seiNumMessages; i++)
    {
        totalSEIBufferSize += seiMessagesInfo[i].sei_message_size;
    }
    if (!m_pCurrSEIMessage)
    {
        printf("Out of Memory, Allocation failed for m_pCurrSEIMessage\n");
        return 0;
    }
    m_pCurrSEIMessage->pSEIData = malloc(totalSEIBufferSize);
    if (!m_pCurrSEIMessage->pSEIData)
    {
        printf("Out of Memory, Allocation failed for SEI Buffer\n");
        return 0;
    }
    memcpy(m_pCurrSEIMessage->pSEIData, pSEIMessageInfo->pSEIData, totalSEIBufferSize);
    m_pCurrSEIMessage->pSEIMessage = (CUSEIMESSAGE *)malloc(sizeof(CUSEIMESSAGE) * seiNumMessages);
    if (!m_pCurrSEIMessage->pSEIMessage)
    {
        free(m_pCurrSEIMessage->pSEIData);
        m_pCurrSEIMessage->pSEIData = NULL;
        return 0;
    }
    memcpy(m_pCurrSEIMessage->pSEIMessage, pSEIMessageInfo->pSEIMessage, sizeof(CUSEIMESSAGE) * seiNumMessages);
    m_pCurrSEIMessage->sei_message_count = pSEIMessageInfo->sei_message_count;
    if (m_SEIMessagesDisplayOrder[pSEIMessageInfo->picIdx][0].pSEIData == NULL)
    {
        m_SEIMessagesDisplayOrder[pSEIMessageInfo->picIdx][0] = *m_pCurrSEIMessage;
    }
    else
    {
        m_SEIMessagesDisplayOrder[pSEIMessageInfo->picIdx][1] = *m_pCurrSEIMessage;
    }
    return 1;
}

NvDecoder::NvDecoder(CUcontext cuContext, bool bUseDeviceFrame, cudaVideoCodec eCodec, bool bLowLatency, 
    bool bDeviceFramePitched, const Rect *pCropRect, const Dim *pResizeDim, bool extract_user_SEI_Message,
    int maxWidth, int maxHeight, unsigned int clkRate, bool force_zero_latency, unsigned int initial_dec_surfaces, bool bEnableDecodeStats, CUstream custream) :
    m_cuContext(cuContext), m_bUseDeviceFrame(bUseDeviceFrame), m_eCodec(eCodec), m_bDeviceFramePitched(bDeviceFramePitched),
    m_bExtractSEIMessage(extract_user_SEI_Message), m_nMaxWidth (maxWidth), m_nMaxHeight(maxHeight),
    m_bForce_zero_latency(force_zero_latency), m_nNumDecSurfaces (initial_dec_surfaces), m_bEnableDecodeStats(bEnableDecodeStats)
{
    if (pCropRect) m_cropRect = *pCropRect;
    if (pResizeDim) m_resizeDim = *pResizeDim;
    m_bMemoryOptimize = (initial_dec_surfaces != 0);

    NVDEC_API_CALL(cuvidCtxLockCreate(&m_ctxLock, cuContext));

    if(custream==NULL)
    {
        ck(cuStreamCreate(&m_cuvidStream, CU_STREAM_DEFAULT));
    }
    else
    {
        m_bExternalStream = true;
        m_cuvidStream = custream;
    }
    
    if (m_bExtractSEIMessage)
    {
        m_fpSEI = fopen("sei_message.txt", "wb");
        if (!m_fpSEI) {
            LOG(ERROR) << "Failed to open sei_message.txt for writing";
        }
        m_pCurrSEIMessage = new CUVIDSEIMESSAGEINFO;
        memset(&m_SEIMessagesDisplayOrder, 0, sizeof(m_SEIMessagesDisplayOrder));
    }
    
    // Initialize decode stats if enabled
    if (m_bEnableDecodeStats)
    {
        m_decodeStatsDptr = 0;
    }
    
    CUVIDPARSERPARAMS videoParserParameters = {};
    videoParserParameters.CodecType = eCodec;
    videoParserParameters.ulMaxNumDecodeSurfaces = 1;
    videoParserParameters.bMemoryOptimize = m_bMemoryOptimize;
    videoParserParameters.ulClockRate = clkRate;
    videoParserParameters.ulMaxDisplayDelay = bLowLatency ? 0 : 1;
    videoParserParameters.pUserData = this;
    videoParserParameters.pfnSequenceCallback = HandleVideoSequenceProc;
    videoParserParameters.pfnDecodePicture = HandlePictureDecodeProc;
    videoParserParameters.pfnDisplayPicture = m_bForce_zero_latency ? NULL : HandlePictureDisplayProc;
    videoParserParameters.pfnGetOperatingPoint = HandleOperatingPointProc;
    videoParserParameters.pfnGetSEIMsg = m_bExtractSEIMessage ? HandleSEIMessagesProc : NULL;
    NVDEC_API_CALL(cuvidCreateVideoParser(&m_hParser, &videoParserParameters));
}

NvDecoder::~NvDecoder() {
    try {
        START_TIMER

        if (m_pCurrSEIMessage) {
            delete m_pCurrSEIMessage;
            m_pCurrSEIMessage = NULL;
        }

        // Clean up decode stats buffers
        for (uint8_t *pDecodeStats : m_vpDecodeStats)
        {
            if (pDecodeStats)
            {
                delete[] pDecodeStats;
            }
        }
        m_vpDecodeStats.clear();
        m_vDecodeStatsSize.clear();

        if (m_fpSEI) {
            fclose(m_fpSEI);
            m_fpSEI = NULL;
        }

        if (m_hParser) {
            cuvidDestroyVideoParser(m_hParser);
        }

        // CUDA context management for all GPU memory cleanup
        // Check if CUDA context is valid and push it if not valid
        CUresult ctxCheckResult = CUDA_SUCCESS;
        CUcontext currentCtx = nullptr;
        bool contextWasPushed = false;

        if (m_bUseDeviceFrame) {
            ctxCheckResult = cuCtxGetCurrent(&currentCtx);
            if (ctxCheckResult != CUDA_SUCCESS || currentCtx == nullptr) {
                // Try to push our context if not current
                ctxCheckResult = cuCtxPushCurrent(m_cuContext);
                contextWasPushed = (ctxCheckResult == CUDA_SUCCESS);
            }
        }

        // Clean up decoder (requires CUDA context)
        if (m_hDecoder) {
            cuvidDestroyDecoder(m_hDecoder);
        }

        // Clean up locked frame data
        if (!m_LockedFrameData.empty()) {
            std::cout << "WARNING: " << m_LockedFrameData.size() << " locked frames were not unlocked by application. Cleaning up..." << std::endl;

            for (const auto& lockedFrame : m_LockedFrameData) {
                // Clean up frame buffer (needs manual cleanup)
                if (lockedFrame.pFrame) {
                    if (m_bUseDeviceFrame) {
                        // Device memory - free with CUDA only if context is valid
                        cuMemFree((CUdeviceptr)lockedFrame.pFrame);
                    } else {
                        // Host memory - free with delete[]
                        delete[] lockedFrame.pFrame;
                    }
                }

                // Clean up decode stats buffer (needs manual cleanup)
                if (lockedFrame.pDecodeStats) {
                    delete[] lockedFrame.pDecodeStats;
                }
            }
        }
        m_LockedFrameData.clear();

        {
            // Clean up regular frame buffers
            std::lock_guard<std::mutex> lock(m_mtxVPFrame);

            for (uint8_t *pFrame : m_vpFrame)
            {
                if (m_bUseDeviceFrame)
                {
                    cuMemFree((CUdeviceptr)pFrame);
                }
                else
                {
                    delete[] pFrame;
                }
            }
        }

        // Pop context only if we pushed it
        if (contextWasPushed) {
            cuCtxPopCurrent(NULL);
        }

        cuvidCtxLockDestroy(m_ctxLock);

        STOP_TIMER("Session Deinitialization Time: ");
    } 
    catch (...) {
        fprintf(stderr, "NvDecoder::~NvDecoder(): exception during cleanup (suppressed)\n");
    }
}

int NvDecoder::Decode(const uint8_t *pData, int nSize, int nFlags, int64_t nTimestamp)
{
    m_nDecodedFrame = 0;
    m_nDecodedFrameReturned = 0;
    CUVIDSOURCEDATAPACKET packet = { 0 };
    packet.payload = pData;
    packet.payload_size = nSize;
    packet.flags = nFlags | CUVID_PKT_TIMESTAMP;
    packet.timestamp = nTimestamp;
    if (!pData || nSize == 0) {
        packet.flags |= CUVID_PKT_ENDOFSTREAM;
    }
    NVDEC_API_CALL(cuvidParseVideoData(m_hParser, &packet));

    return m_nDecodedFrame;
}



uint8_t* NvDecoder::GetFrame(int64_t* pTimestamp, uint8_t **pDecodeStats, uint32_t *pDecodeStatsSize, bool *pSkipped)
{
    std::lock_guard<std::mutex> lock(m_mtxVPFrame);
    
    if (m_nDecodedFrame > 0)
    {
        m_nDecodedFrame--;
        if (pTimestamp)
            *pTimestamp = m_vTimestamp[m_nDecodedFrameReturned];
        
        // Set skip flag if requested
        if (pSkipped)
            *pSkipped = m_vSkipped[m_nDecodedFrameReturned];
        
        // Set decode stats if available
        if (pDecodeStats) {
            if (m_bEnableDecodeStats) {
                *pDecodeStats = m_vpDecodeStats[m_nDecodedFrameReturned];
                if (pDecodeStatsSize)
                    *pDecodeStatsSize = m_vDecodeStatsSize[m_nDecodedFrameReturned];
            } else {
                // No decode stats available for this frame
                *pDecodeStats = nullptr;
                if (pDecodeStatsSize)
                    *pDecodeStatsSize = 0;
            }
        }
        
        return m_vpFrame[m_nDecodedFrameReturned++];
    }

    return NULL;
}

uint8_t* NvDecoder::GetLockedFrame(int64_t* pTimestamp, uint8_t **pDecodeStats, uint32_t *pDecodeStatsSize, bool *pSkipped)
{
    std::lock_guard<std::mutex> lock(m_mtxVPFrame);
    
    if (m_nDecodedFrame > 0) {
        uint8_t *pFrame, *pFrameDecodeStats;
        uint32_t frameDecodeStatsSize;
        uint64_t timestamp;
        
        m_nDecodedFrame--;
        pFrame = m_vpFrame[0];
        m_vpFrame.erase(m_vpFrame.begin(), m_vpFrame.begin() + 1);
        
        timestamp = m_vTimestamp[0];
        m_vTimestamp.erase(m_vTimestamp.begin(), m_vTimestamp.begin() + 1);
        
        bool bSkipped = m_vSkipped[0];
        m_vSkipped.erase(m_vSkipped.begin(), m_vSkipped.begin() + 1);
        
        if (pTimestamp)
            *pTimestamp = timestamp;
        
        if (pSkipped)
            *pSkipped = bSkipped;
        
        if (m_bEnableDecodeStats) {
            pFrameDecodeStats = m_vpDecodeStats[0];
            frameDecodeStatsSize = m_vDecodeStatsSize[0];
            if (pDecodeStats) {
                *pDecodeStats = pFrameDecodeStats;
            }
            if (pDecodeStatsSize) {
                *pDecodeStatsSize = frameDecodeStatsSize;
            }
            m_vpDecodeStats.erase(m_vpDecodeStats.begin(), m_vpDecodeStats.begin() + 1);
            m_vDecodeStatsSize.erase(m_vDecodeStatsSize.begin(), m_vDecodeStatsSize.begin() + 1);
        } else {
            pFrameDecodeStats = nullptr;
            frameDecodeStatsSize = 0;
            if (pDecodeStats) {
                *pDecodeStats = nullptr;
            }
            if (pDecodeStatsSize) {
                *pDecodeStatsSize = 0;
            }
        }

        // Prepare locked frame data structure (only for memory that needs tracking)
        // Only track non-NULL frames (skipped frames may be NULL)
        if (pFrame != nullptr) {
            LockedFrameData lockedData;
            lockedData.pFrame = pFrame;
            lockedData.pDecodeStats = pFrameDecodeStats;
            lockedData.decodeStatsSize = frameDecodeStatsSize;

            // Add to locked frames management
            m_LockedFrameData.push_back(lockedData);
        }
        
        return pFrame;
    }

    return NULL;
}

void NvDecoder::UnlockFrame(uint8_t **pFrame, int nFrame)
{
    std::lock_guard<std::mutex> lock(m_mtxVPFrame);
    
    // Process each frame to unlock
    for (int i = 0; i < nFrame; i++) {
        uint8_t* frameToUnlock = pFrame[i];
        bool foundInLocked = false;
        
        // Find and remove from locked frames
        for (auto it = m_LockedFrameData.begin(); it != m_LockedFrameData.end(); ++it) {
            if (it->pFrame == frameToUnlock) {
                // Return frame to memory pool
                m_vpFrame.push_back(it->pFrame);
                m_vTimestamp.push_back(0); // Dummy timestamp since we don't track it in LockedFrameData
                m_vSkipped.push_back(false);
                // Return decode stats if available
                if (m_bEnableDecodeStats && it->pDecodeStats) {
                    m_vpDecodeStats.push_back(it->pDecodeStats);
                    m_vDecodeStatsSize.push_back(it->decodeStatsSize); // Restore the original size
                }
                
                // Remove from locked frames
                m_LockedFrameData.erase(it);
                foundInLocked = true;
                break;
            }
        }
    }
}

