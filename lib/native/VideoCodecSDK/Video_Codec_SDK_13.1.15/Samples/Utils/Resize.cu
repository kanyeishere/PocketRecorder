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

#include <cuda_runtime.h>
#include "NvCodecUtils.h"

template<typename YuvUnitx2>
static __global__ void Resize(cudaTextureObject_t texY, cudaTextureObject_t texUv,
        uint8_t *pDst, uint8_t *pDstUV, int nPitch, int nWidth, int nHeight,
        float fxScale, float fyScale)
{
    int ix = blockIdx.x * blockDim.x + threadIdx.x,
        iy = blockIdx.y * blockDim.y + threadIdx.y;

    if (ix >= nWidth / 2 || iy >= nHeight / 2) {
        return;
    }

    int x = ix * 2, y = iy * 2;
    typedef decltype(YuvUnitx2::x) YuvUnit;
    const int MAX = (1 << (sizeof(YuvUnit) * 8)) - 1;
    *(YuvUnitx2 *)(pDst + y * nPitch + x * sizeof(YuvUnit)) = YuvUnitx2 {
        (YuvUnit)(tex2D<float>(texY, x / fxScale, y / fyScale) * MAX),
        (YuvUnit)(tex2D<float>(texY, (x + 1) / fxScale, y / fyScale) * MAX)
    };
    y++;
    *(YuvUnitx2 *)(pDst + y * nPitch + x * sizeof(YuvUnit)) = YuvUnitx2 {
        (YuvUnit)(tex2D<float>(texY, x / fxScale, y / fyScale) * MAX),
        (YuvUnit)(tex2D<float>(texY, (x + 1) / fxScale, y / fyScale) * MAX)
    };
    float2 uv = tex2D<float2>(texUv, ix / fxScale, (nHeight + iy) / fyScale + 0.5f);
    *(YuvUnitx2 *)(pDstUV + iy * nPitch + ix * 2 * sizeof(YuvUnit)) = YuvUnitx2{ (YuvUnit)(uv.x * MAX), (YuvUnit)(uv.y * MAX) };
}

template <typename YuvUnitx2>
static void Resize(unsigned char *dpDst, unsigned char* dpDstUV, int nDstPitch, int nDstWidth, int nDstHeight, unsigned char *dpSrc, int nSrcPitch, int nSrcWidth, int nSrcHeight, cudaStream_t stream) {
    cudaResourceDesc resDesc = {};
    resDesc.resType = cudaResourceTypePitch2D;
    resDesc.res.pitch2D.devPtr = dpSrc;
    resDesc.res.pitch2D.desc = cudaCreateChannelDesc<decltype(YuvUnitx2::x)>();
    resDesc.res.pitch2D.width = nSrcWidth;
    resDesc.res.pitch2D.height = nSrcHeight;
    resDesc.res.pitch2D.pitchInBytes = nSrcPitch;

    cudaTextureDesc texDesc = {};
    texDesc.filterMode = cudaFilterModeLinear;
    texDesc.readMode = cudaReadModeNormalizedFloat;

    cudaTextureObject_t texY=0;
    ck(cudaCreateTextureObject(&texY, &resDesc, &texDesc, NULL));

    resDesc.res.pitch2D.desc = cudaCreateChannelDesc<YuvUnitx2>();
    resDesc.res.pitch2D.width = nSrcWidth / 2;
    resDesc.res.pitch2D.height = nSrcHeight * 3 / 2;

    cudaTextureObject_t texUv=0;
    ck(cudaCreateTextureObject(&texUv, &resDesc, &texDesc, NULL));

    Resize<YuvUnitx2><<<dim3((nDstWidth + 31) / 32, (nDstHeight + 31) / 32), dim3(16, 16), 0, stream>>>(texY, texUv, dpDst, dpDstUV,
        nDstPitch, nDstWidth, nDstHeight, 1.0f * nDstWidth / nSrcWidth, 1.0f * nDstHeight / nSrcHeight);

    ck(cudaDestroyTextureObject(texY));
    ck(cudaDestroyTextureObject(texUv));
}

template <typename YuvUnitx2>
static void ResizeWithTexture(unsigned char *dpDst, unsigned char* dpDstUV, int nDstPitch, int nDstWidth, int nDstHeight,
                            const TextureObject& texObj, int nSrcWidth, int nSrcHeight, cudaStream_t stream) {
    Resize<YuvUnitx2> << <dim3((nDstWidth + 31) / 32, (nDstHeight + 31) / 32), dim3(16, 16), 0, stream >> >(texObj.texY, texObj.texUV, dpDst, dpDstUV,
        nDstPitch, nDstWidth, nDstHeight, 1.0f * nDstWidth / nSrcWidth, 1.0f * nDstHeight / nSrcHeight);
}

void ResizeNv12(unsigned char *dpDstNv12, int nDstPitch, int nDstWidth, int nDstHeight, unsigned char *dpSrcNv12, int nSrcPitch, int nSrcWidth, int nSrcHeight, unsigned char* dpDstNv12UV, cudaStream_t stream)
{
    unsigned char* dpDstUV = dpDstNv12UV ? dpDstNv12UV : dpDstNv12 + (nDstPitch*nDstHeight);
    Resize<uchar2>(dpDstNv12, dpDstUV, nDstPitch, nDstWidth, nDstHeight, dpSrcNv12, nSrcPitch, nSrcWidth, nSrcHeight, stream);
}

void ResizeNv12WithTexture(unsigned char *dpDstNv12, int nDstPitch, int nDstWidth, int nDstHeight, 
                          const TextureObject& texObj, int nSrcWidth, int nSrcHeight, unsigned char* dpDstNv12UV, cudaStream_t stream)
{
    unsigned char* dpDstUV = dpDstNv12UV ? dpDstNv12UV : dpDstNv12 + (nDstPitch*nDstHeight);
    return ResizeWithTexture<uchar2>(dpDstNv12, dpDstUV, nDstPitch, nDstWidth, nDstHeight, texObj, nSrcWidth, nSrcHeight, stream);
}

void ResizeP016(unsigned char *dpDstP016, int nDstPitch, int nDstWidth, int nDstHeight, unsigned char *dpSrcP016, int nSrcPitch, int nSrcWidth, int nSrcHeight, unsigned char* dpDstP016UV, cudaStream_t stream)
{
    unsigned char* dpDstUV = dpDstP016UV ? dpDstP016UV : dpDstP016 + (nDstPitch*nDstHeight);
    Resize<ushort2>(dpDstP016, dpDstUV, nDstPitch, nDstWidth, nDstHeight, dpSrcP016, nSrcPitch, nSrcWidth, nSrcHeight, stream);
}

void ResizeP016WithTexture(unsigned char *dpDstP016, int nDstPitch, int nDstWidth, int nDstHeight,
                          const TextureObject& texObj, int nSrcWidth, int nSrcHeight, unsigned char* dpDstP016UV, cudaStream_t stream)
{
    unsigned char* dpDstUV = dpDstP016UV ? dpDstP016UV : dpDstP016 + (nDstPitch*nDstHeight);
    return ResizeWithTexture<ushort2>(dpDstP016, dpDstUV, nDstPitch, nDstWidth, nDstHeight, texObj, nSrcWidth, nSrcHeight, stream);
}

// 422 Resize Kernel
template<typename YuvUnitx2>
static __global__ void Resize422(cudaTextureObject_t texY, cudaTextureObject_t texUv,
        uint8_t *pDst, uint8_t *pDstUV, int nPitch, int nWidth, int nHeight,
        float fxScale, float fyScale)
{
    int ix = blockIdx.x * blockDim.x + threadIdx.x,
        iy = blockIdx.y * blockDim.y + threadIdx.y;

    if (ix >= nWidth / 2 || iy >= nHeight / 2) {
        return;
    }

    int x = ix * 2, y = iy * 2;
    typedef decltype(YuvUnitx2::x) YuvUnit;
    const int MAX = (1 << (sizeof(YuvUnit) * 8)) - 1;
    
    // Process luma (2 rows, 2 pixels per row)
    *(YuvUnitx2 *)(pDst + y * nPitch + x * sizeof(YuvUnit)) = YuvUnitx2 {
        (YuvUnit)(tex2D<float>(texY, x / fxScale, y / fyScale) * MAX),
        (YuvUnit)(tex2D<float>(texY, (x + 1) / fxScale, y / fyScale) * MAX)
    };
    y++;
    *(YuvUnitx2 *)(pDst + y * nPitch + x * sizeof(YuvUnit)) = YuvUnitx2 {
        (YuvUnit)(tex2D<float>(texY, x / fxScale, y / fyScale) * MAX),
        (YuvUnit)(tex2D<float>(texY, (x + 1) / fxScale, y / fyScale) * MAX)
    };
    
    // Process chroma (full-height for 422 - 2 rows per thread)
    // Row iy*2
    float2 uv = tex2D<float2>(texUv, ix / fxScale, (nHeight + iy * 2) / fyScale + 0.5f);
    *(YuvUnitx2 *)(pDstUV + (iy * 2) * nPitch + ix * 2 * sizeof(YuvUnit)) = YuvUnitx2{ (YuvUnit)(uv.x * MAX), (YuvUnit)(uv.y * MAX) };
    // Row iy*2+1
    uv = tex2D<float2>(texUv, ix / fxScale, (nHeight + iy * 2 + 1) / fyScale + 0.5f);
    *(YuvUnitx2 *)(pDstUV + (iy * 2 + 1) * nPitch + ix * 2 * sizeof(YuvUnit)) = YuvUnitx2{ (YuvUnit)(uv.x * MAX), (YuvUnit)(uv.y * MAX) };
}

template <typename YuvUnitx2>
static void ResizeWithTexture422(unsigned char *dpDst, unsigned char* dpDstUV, int nDstPitch, int nDstWidth, int nDstHeight,
                            const TextureObject& texObj, int nSrcWidth, int nSrcHeight, cudaStream_t stream) {
    Resize422<YuvUnitx2> << <dim3((nDstWidth + 31) / 32, (nDstHeight + 31) / 32), dim3(16, 16), 0, stream >> >(texObj.texY, texObj.texUV, dpDst, dpDstUV,
        nDstPitch, nDstWidth, nDstHeight, 1.0f * nDstWidth / nSrcWidth, 1.0f * nDstHeight / nSrcHeight);
}

void ResizeNv16WithTexture(unsigned char *dpDstNv16, int nDstPitch, int nDstWidth, int nDstHeight, 
                          const TextureObject& texObj, int nSrcWidth, int nSrcHeight, unsigned char* dpDstNv16UV, cudaStream_t stream)
{
    unsigned char* dpDstUV = dpDstNv16UV ? dpDstNv16UV : dpDstNv16 + (nDstPitch*nDstHeight);
    return ResizeWithTexture422<uchar2>(dpDstNv16, dpDstUV, nDstPitch, nDstWidth, nDstHeight, texObj, nSrcWidth, nSrcHeight, stream);
}

void ResizeP210WithTexture(unsigned char *dpDstP210, int nDstPitch, int nDstWidth, int nDstHeight,
                          const TextureObject& texObj, int nSrcWidth, int nSrcHeight, unsigned char* dpDstP210UV, cudaStream_t stream)
{
    unsigned char* dpDstUV = dpDstP210UV ? dpDstP210UV : dpDstP210 + (nDstPitch*nDstHeight);
    return ResizeWithTexture422<ushort2>(dpDstP210, dpDstUV, nDstPitch, nDstWidth, nDstHeight, texObj, nSrcWidth, nSrcHeight, stream);
}

// 444 Resize Kernel
template<typename YuvUnitx2>
static __global__ void Resize444(cudaTextureObject_t texY, cudaTextureObject_t texUV,
        uint8_t *pDst, uint8_t *pDstU, uint8_t *pDstV, int nPitch, int nWidth, int nHeight,
        float fxScale, float fyScale)
{
    int ix = blockIdx.x * blockDim.x + threadIdx.x,
        iy = blockIdx.y * blockDim.y + threadIdx.y;

    if (ix >= nWidth / 2 || iy >= nHeight / 2) {
        return;
    }

    int x = ix * 2, y = iy * 2;
    typedef decltype(YuvUnitx2::x) YuvUnit;
    const int MAX = (1 << (sizeof(YuvUnit) * 8)) - 1;
    
    // Process luma (2 rows, 2 pixels per row)
    *(YuvUnitx2 *)(pDst + y * nPitch + x * sizeof(YuvUnit)) = YuvUnitx2 {
        (YuvUnit)(tex2D<float>(texY, x / fxScale, y / fyScale) * MAX),
        (YuvUnit)(tex2D<float>(texY, (x + 1) / fxScale, y / fyScale) * MAX)
    };
    y++;
    *(YuvUnitx2 *)(pDst + y * nPitch + x * sizeof(YuvUnit)) = YuvUnitx2 {
        (YuvUnit)(tex2D<float>(texY, x / fxScale, y / fyScale) * MAX),
        (YuvUnit)(tex2D<float>(texY, (x + 1) / fxScale, y / fyScale) * MAX)
    };
    
    // Process U plane (full-width, full-height for 444)
    // Row iy*2
    float u0 = tex2D<float>(texUV, x / fxScale, (nHeight + iy * 2) / fyScale);
    float u1 = tex2D<float>(texUV, (x + 1) / fxScale, (nHeight + iy * 2) / fyScale);
    *(YuvUnitx2 *)(pDstU + (iy * 2) * nPitch + x * sizeof(YuvUnit)) = 
        YuvUnitx2{(YuvUnit)(u0 * MAX), (YuvUnit)(u1 * MAX)};
    
    // Row iy*2+1
    u0 = tex2D<float>(texUV, x / fxScale, (nHeight + iy * 2 + 1) / fyScale);
    u1 = tex2D<float>(texUV, (x + 1) / fxScale, (nHeight + iy * 2 + 1) / fyScale);
    *(YuvUnitx2 *)(pDstU + (iy * 2 + 1) * nPitch + x * sizeof(YuvUnit)) = 
        YuvUnitx2{(YuvUnit)(u0 * MAX), (YuvUnit)(u1 * MAX)};
    
    // Process V plane (full-width, full-height for 444)
    // Row iy*2
    float v0 = tex2D<float>(texUV, x / fxScale, (nHeight * 2 + iy * 2) / fyScale);
    float v1 = tex2D<float>(texUV, (x + 1) / fxScale, (nHeight * 2 + iy * 2) / fyScale);
    *(YuvUnitx2 *)(pDstV + (iy * 2) * nPitch + x * sizeof(YuvUnit)) = 
        YuvUnitx2{(YuvUnit)(v0 * MAX), (YuvUnit)(v1 * MAX)};
    
    // Row iy*2+1
    v0 = tex2D<float>(texUV, x / fxScale, (nHeight * 2 + iy * 2 + 1) / fyScale);
    v1 = tex2D<float>(texUV, (x + 1) / fxScale, (nHeight * 2 + iy * 2 + 1) / fyScale);
    *(YuvUnitx2 *)(pDstV + (iy * 2 + 1) * nPitch + x * sizeof(YuvUnit)) = 
        YuvUnitx2{(YuvUnit)(v0 * MAX), (YuvUnit)(v1 * MAX)};
}

template <typename YuvUnitx2>
static void ResizeWithTexture444(unsigned char *dpDst, unsigned char* dpDstU, unsigned char* dpDstV, 
                                 int nDstPitch, int nDstWidth, int nDstHeight,
                                 const TextureObject& texObj, int nSrcWidth, int nSrcHeight, cudaStream_t stream) {
    Resize444<YuvUnitx2> << <dim3((nDstWidth + 31) / 32, (nDstHeight + 31) / 32), dim3(16, 16), 0, stream >> >(texObj.texY, texObj.texUV, dpDst, dpDstU, dpDstV,
        nDstPitch, nDstWidth, nDstHeight, 1.0f * nDstWidth / nSrcWidth, 1.0f * nDstHeight / nSrcHeight);
}

void ResizeYUV444WithTexture(unsigned char *dpDstYUV444, int nDstPitch, int nDstWidth, int nDstHeight, 
                             const TextureObject& texObj, int nSrcWidth, int nSrcHeight, 
                             unsigned char* dpDstYUV444U, unsigned char* dpDstYUV444V, cudaStream_t stream)
{
    unsigned char* dpDstU = dpDstYUV444U ? dpDstYUV444U : dpDstYUV444 + (nDstPitch*nDstHeight);
    unsigned char* dpDstV = dpDstYUV444V ? dpDstYUV444V : dpDstU + (nDstPitch*nDstHeight);
    return ResizeWithTexture444<uchar2>(dpDstYUV444, dpDstU, dpDstV, nDstPitch, nDstWidth, nDstHeight, texObj, nSrcWidth, nSrcHeight, stream);
}

void ResizeYUV444_10BitWithTexture(unsigned char *dpDstYUV444, int nDstPitch, int nDstWidth, int nDstHeight,
                                   const TextureObject& texObj, int nSrcWidth, int nSrcHeight, 
                                   unsigned char* dpDstYUV444U, unsigned char* dpDstYUV444V, cudaStream_t stream)
{
    unsigned char* dpDstU = dpDstYUV444U ? dpDstYUV444U : dpDstYUV444 + (nDstPitch*nDstHeight);
    unsigned char* dpDstV = dpDstYUV444V ? dpDstYUV444V : dpDstU + (nDstPitch*nDstHeight);
    return ResizeWithTexture444<ushort2>(dpDstYUV444, dpDstU, dpDstV, nDstPitch, nDstWidth, nDstHeight, texObj, nSrcWidth, nSrcHeight, stream);
}

static __global__ void Scale(cudaTextureObject_t texSrc,
    uint8_t *pDst, int nPitch, int nWidth, int nHeight,
    float fxScale, float fyScale)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x,
        y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= nWidth || y >= nHeight)
    {
        return;
    }

    *(unsigned char*)(pDst + (y * nPitch) + x) = (unsigned char)(fminf((tex2D<float>(texSrc, x * fxScale, y * fyScale)) * 255.0f, 255.0f));
}

static __global__ void Scale_uv(cudaTextureObject_t texSrc,
    uint8_t *pDst, int nPitch, int nWidth, int nHeight,
    float fxScale, float fyScale)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x,
        y = blockIdx.y * blockDim.y + threadIdx.y;

    if (x >= nWidth || y >= nHeight)
    {
        return;
    }

    float2 uv = tex2D<float2>(texSrc, x * fxScale, y * fyScale);
    uchar2 uvOut = uchar2{ (unsigned char)(fminf(uv.x * 255.0f, 255.0f)), (unsigned char)(fminf(uv.y * 255.0f, 255.0f)) };

    *(uchar2*)(pDst + (y * nPitch) + 2 * x) = uvOut;
}

void ScaleKernelLaunch(unsigned char *dpDst, int nDstPitch, int nDstWidth, int nDstHeight, unsigned char *dpSrc, int nSrcPitch, int nSrcWidth, int nSrcHeight, bool bUVPlane = false) 
{
    cudaResourceDesc resDesc = {};
    resDesc.resType = cudaResourceTypePitch2D;
    resDesc.res.pitch2D.devPtr = dpSrc;
    resDesc.res.pitch2D.desc = bUVPlane ? cudaCreateChannelDesc<uchar2>() : cudaCreateChannelDesc<unsigned char>();
    resDesc.res.pitch2D.width = nSrcWidth;
    resDesc.res.pitch2D.height = nSrcHeight;
    resDesc.res.pitch2D.pitchInBytes = nSrcPitch;

    cudaTextureDesc texDesc = {};
    texDesc.filterMode = cudaFilterModeLinear;
    texDesc.readMode = cudaReadModeNormalizedFloat;

    texDesc.addressMode[0] = cudaAddressModeClamp;
    texDesc.addressMode[1] = cudaAddressModeClamp;
    texDesc.addressMode[2] = cudaAddressModeClamp;

    cudaTextureObject_t texSrc = 0;
    ck(cudaCreateTextureObject(&texSrc, &resDesc, &texDesc, NULL));

    dim3 blockSize(16, 16, 1);
    dim3 gridSize(((uint32_t)nDstWidth + blockSize.x - 1) / blockSize.x, ((uint32_t)nDstHeight + blockSize.y - 1) / blockSize.y, 1);

    if (bUVPlane)
    {
        Scale_uv << <gridSize, blockSize >> >(texSrc, dpDst,
            nDstPitch, nDstWidth, nDstHeight, 1.0f * nSrcWidth / nDstWidth, 1.0f * nSrcHeight / nDstHeight);
    }
    else
    {
        Scale << <gridSize, blockSize >> >(texSrc, dpDst,
            nDstPitch, nDstWidth, nDstHeight, 1.0f * nSrcWidth / nDstWidth, 1.0f * nSrcHeight / nDstHeight);
    }

    ck(cudaGetLastError());
    ck(cudaDestroyTextureObject(texSrc));
}

void ScaleYUV420(unsigned char *dpDstY,
                 unsigned char* dpDstU,
                unsigned char* dpDstV,
                int nDstPitch,
                int nDstChromaPitch,
                int nDstWidth,
                int nDstHeight,
                unsigned char *dpSrcY,
                unsigned char* dpSrcU,
                unsigned char* dpSrcV, 
                int nSrcPitch,
                int nSrcChromaPitch,
                int nSrcWidth,
                int nSrcHeight,
                bool bSemiplanar)
{
    int chromaWidthDst = (nDstWidth + 1) / 2;
    int chromaHeightDst = (nDstHeight + 1) / 2;

    int chromaWidthSrc = (nSrcWidth + 1) / 2;
    int chromaHeightSrc = (nSrcHeight + 1) / 2;

    ScaleKernelLaunch(dpDstY, nDstPitch, nDstWidth, nDstHeight, dpSrcY, nSrcPitch, nSrcWidth, nSrcHeight);

    if (bSemiplanar)
    {
        ScaleKernelLaunch(dpDstU, nDstChromaPitch, chromaWidthDst, chromaHeightDst, dpSrcU, nSrcChromaPitch, chromaWidthSrc, chromaHeightSrc, true);
    }
    else
    {
        ScaleKernelLaunch(dpDstU, nDstChromaPitch, chromaWidthDst, chromaHeightDst, dpSrcU, nSrcChromaPitch, chromaWidthSrc, chromaHeightSrc);
        ScaleKernelLaunch(dpDstV, nDstChromaPitch, chromaWidthDst, chromaHeightDst, dpSrcV, nSrcChromaPitch, chromaWidthSrc, chromaHeightSrc);
    }
}
