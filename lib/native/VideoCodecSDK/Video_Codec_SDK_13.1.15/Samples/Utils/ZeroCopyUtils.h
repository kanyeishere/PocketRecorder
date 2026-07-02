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
#include <stdint.h>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cuda.h>
#include "nvEncodeAPI.h"
#include "nvcuvid.h"

struct ZeroCopyBuffer
{
    CUarray cudaArray;
    unsigned int width;
    unsigned int height;
    unsigned int bufHeight;
    unsigned int pitch;
    CUarray_format arrayFormat;
    NV_ENC_BUFFER_FORMAT encFormat;
    cudaVideoSurfaceFormat decFormat;
    bool inUse;
    int bufferIndex;
};

class ZeroCopyBufferPool
{
public:
    ZeroCopyBufferPool()
        : m_cuContext(nullptr)
        , m_bAllocated(false)
    {
    }

    ~ZeroCopyBufferPool()
    {
        Release();
    }

    bool Allocate(CUcontext cuContext, unsigned int numBuffers,
                  unsigned int width, unsigned int height,
                  cudaVideoSurfaceFormat decFormat,
                  NV_ENC_BUFFER_FORMAT encFormat,
                  GUID codecGuid = NV_ENC_CODEC_H264_GUID)
    {
        // Validate that decoder and encoder formats are compatible
        if (!AreFormatsCompatible(decFormat, encFormat))
        {
            return false;
        }

        if (m_bAllocated)
        {
            Release();
        }

        m_cuContext = cuContext;

        unsigned int alignedWidth = (width + 63) & ~63;

        unsigned int alignedHeight;
        if (codecGuid == NV_ENC_CODEC_AV1_GUID)
        {
            alignedHeight = (height + 63) & ~63;
        }
        else if (codecGuid == NV_ENC_CODEC_HEVC_GUID)
        {
            alignedHeight = (height + 31) & ~31;
        }
        else
        {
            alignedHeight = (height + 15) & ~15;
        }

        unsigned int chromaHeight;
        switch (encFormat)
        {
        case NV_ENC_BUFFER_FORMAT_NV12:
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
            chromaHeight = (alignedHeight + 1) / 2;
            break;
        case NV_ENC_BUFFER_FORMAT_NV16:
        case NV_ENC_BUFFER_FORMAT_P210:
            chromaHeight = alignedHeight;
            break;
        default:
            chromaHeight = (alignedHeight + 1) / 2;
            break;
        }

        unsigned int bufHeight = alignedHeight + chromaHeight;

        CUarray_format arrayFormat = GetCudaArrayFormat(decFormat);

        unsigned int bpp = GetBytesPerPixel(encFormat);
        unsigned int pitch = alignedWidth * bpp;

        CUresult result;
        result = cuCtxPushCurrent(cuContext);
        if (result != CUDA_SUCCESS)
        {
            return false;
        }

        m_vBuffers.resize(numBuffers);

        for (unsigned int i = 0; i < numBuffers; i++)
        {
            ZeroCopyBuffer& buf = m_vBuffers[i];

            CUDA_ARRAY3D_DESCRIPTOR desc = {};
            desc.Width = alignedWidth;
            desc.Height = alignedHeight;
            desc.Depth = 0;
            desc.NumChannels = 3;
            desc.Format = arrayFormat;
            desc.Flags = CUDA_ARRAY3D_SURFACE_LDST | CUDA_ARRAY3D_VIDEO_ENCODE_DECODE;

            result = cuArray3DCreate(&buf.cudaArray, &desc);
            if (result != CUDA_SUCCESS)
            {
                for (unsigned int j = 0; j < i; j++)
                {
                    cuArrayDestroy(m_vBuffers[j].cudaArray);
                }
                m_vBuffers.clear();
                CUcontext dummy;
                cuCtxPopCurrent(&dummy);
                return false;
            }

            buf.width = alignedWidth;
            buf.height = alignedHeight;
            buf.bufHeight = bufHeight;
            buf.pitch = pitch;
            buf.arrayFormat = arrayFormat;
            buf.encFormat = encFormat;
            buf.decFormat = decFormat;
            buf.inUse = false;
            buf.bufferIndex = static_cast<int>(i);
        }

        CUcontext dummy;
        cuCtxPopCurrent(&dummy);

        m_bAllocated = true;
        return true;
    }

    void Release()
    {
        if (!m_bAllocated || m_vBuffers.empty())
        {
            return;
        }

        CUresult result = cuCtxPushCurrent(m_cuContext);
        if (result == CUDA_SUCCESS)
        {
            for (auto& buf : m_vBuffers)
            {
                if (buf.cudaArray)
                {
                    cuArrayDestroy(buf.cudaArray);
                    buf.cudaArray = nullptr;
                }
            }
            CUcontext dummy;
            cuCtxPopCurrent(&dummy);
        }

        m_vBuffers.clear();
        m_bAllocated = false;
    }

    std::vector<CUarray> GetCudaArrays() const
    {
        std::vector<CUarray> arrays;
        arrays.reserve(m_vBuffers.size());
        for (const auto& buf : m_vBuffers)
        {
            arrays.push_back(buf.cudaArray);
        }
        return arrays;
    }

    ZeroCopyBuffer& GetBuffer(size_t index)
    {
        if (index >= m_vBuffers.size())
        {
            throw std::out_of_range("Buffer index out of range");
        }
        return m_vBuffers[index];
    }

    size_t GetBufferCount() const { return m_vBuffers.size(); }
    bool IsAllocated() const { return m_bAllocated; }
    unsigned int GetAlignedWidth() const { return m_vBuffers.empty() ? 0 : m_vBuffers[0].width; }
    unsigned int GetAlignedHeight() const { return m_vBuffers.empty() ? 0 : m_vBuffers[0].height; }
    unsigned int GetPitch() const { return m_vBuffers.empty() ? 0 : m_vBuffers[0].pitch; }

    static CUarray_format GetCudaArrayFormat(cudaVideoSurfaceFormat surfaceFormat)
    {
        switch (surfaceFormat)
        {
        case cudaVideoSurfaceFormat_NV12:
            return CU_AD_FORMAT_NV12;
        case cudaVideoSurfaceFormat_P016:
            return CU_AD_FORMAT_P016;
        case cudaVideoSurfaceFormat_NV16:
            return CU_AD_FORMAT_NV16;
        case cudaVideoSurfaceFormat_P216:
            return CU_AD_FORMAT_P216;
        default:
            return CU_AD_FORMAT_NV12;
        }
    }

    static unsigned int GetBytesPerPixel(NV_ENC_BUFFER_FORMAT format)
    {
        switch (format)
        {
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
        case NV_ENC_BUFFER_FORMAT_P210:
            return 2;
        case NV_ENC_BUFFER_FORMAT_ABGR:
        case NV_ENC_BUFFER_FORMAT_ARGB:
        case NV_ENC_BUFFER_FORMAT_ABGR10:
        case NV_ENC_BUFFER_FORMAT_ARGB10:
        case NV_ENC_BUFFER_FORMAT_AYUV:
            return 4;
        default:
            return 1;
        }
    }

    static cudaVideoSurfaceFormat EncFormatToDecFormat(NV_ENC_BUFFER_FORMAT encFormat)
    {
        switch (encFormat)
        {
        case NV_ENC_BUFFER_FORMAT_NV12:
            return cudaVideoSurfaceFormat_NV12;
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
            return cudaVideoSurfaceFormat_P016;
        case NV_ENC_BUFFER_FORMAT_NV16:
            return cudaVideoSurfaceFormat_NV16;
        case NV_ENC_BUFFER_FORMAT_P210:
            return cudaVideoSurfaceFormat_P216;
        default:
            return cudaVideoSurfaceFormat_NV12;
        }
    }

    static NV_ENC_BUFFER_FORMAT DecFormatToEncFormat(cudaVideoSurfaceFormat decFormat)
    {
        switch (decFormat)
        {
        case cudaVideoSurfaceFormat_NV12:
            return NV_ENC_BUFFER_FORMAT_NV12;
        case cudaVideoSurfaceFormat_P016:
            return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
        case cudaVideoSurfaceFormat_NV16:
            return NV_ENC_BUFFER_FORMAT_NV16;
        case cudaVideoSurfaceFormat_P216:
            return NV_ENC_BUFFER_FORMAT_P210;
        default:
            return NV_ENC_BUFFER_FORMAT_NV12;
        }
    }

    static bool AreFormatsCompatible(cudaVideoSurfaceFormat decFormat, NV_ENC_BUFFER_FORMAT encFormat)
    {
        NV_ENC_BUFFER_FORMAT expectedEncFormat = DecFormatToEncFormat(decFormat);
        return (encFormat == expectedEncFormat);
    }

private:
    CUcontext m_cuContext;
    std::vector<ZeroCopyBuffer> m_vBuffers;
    bool m_bAllocated;
};
