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

#include "NvEncoderZeroCopy.h"
#include <cstring>

#ifndef _WIN32
inline bool operator==(const GUID& lhs, const GUID& rhs) {
    return !memcmp(&lhs, &rhs, sizeof(GUID));
}
inline bool operator!=(const GUID& lhs, const GUID& rhs) {
    return !(lhs == rhs);
}
#endif

NvEncoderZeroCopy::NvEncoderZeroCopy(CUcontext cuContext, uint32_t nWidth, uint32_t nHeight,
                                     NV_ENC_BUFFER_FORMAT eBufferFormat, GUID codecGuid,
                                     uint32_t nExtraOutputDelay)
    : m_cuContext(cuContext)
    , m_hEncoder(nullptr)
    , m_nWidth(nWidth)
    , m_nHeight(nHeight)
    , m_eBufferFormat(eBufferFormat)
    , m_codecGuid(codecGuid)
    , m_nExtraOutputDelay(nExtraOutputDelay)
    , m_bEncoderInitialized(false)
    , m_nEncoderBufferCount(0)
    , m_nOutputDelay(0)
    , m_bUseExtCudaArray(false)
    , m_bHasBFrames(false)
    , m_nNextOutputIdx(0)
{
    memset(&m_nvenc, 0, sizeof(m_nvenc));
    memset(&m_initializeParams, 0, sizeof(m_initializeParams));
    memset(&m_encodeConfig, 0, sizeof(m_encodeConfig));

    LoadNvEncApi();
    OpenEncodeSession();
}

NvEncoderZeroCopy::~NvEncoderZeroCopy()
{
    DestroyEncoder();
    CloseEncodeSession();
}

void NvEncoderZeroCopy::LoadNvEncApi()
{
    uint32_t version = 0;
    uint32_t currentVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;

    NVENCSTATUS status = NvEncodeAPIGetMaxSupportedVersion(&version);
    if (status != NV_ENC_SUCCESS)
    {
        NVENC_ZC_THROW_ERROR("NvEncodeAPIGetMaxSupportedVersion failed", status);
    }

    if (currentVersion > version)
    {
        NVENC_ZC_THROW_ERROR("Driver version does not support this NvEncodeAPI version", NV_ENC_ERR_INVALID_VERSION);
    }

    m_nvenc.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    status = NvEncodeAPICreateInstance(&m_nvenc);
    if (status != NV_ENC_SUCCESS)
    {
        NVENC_ZC_THROW_ERROR("NvEncodeAPICreateInstance failed", status);
    }
}

void NvEncoderZeroCopy::OpenEncodeSession()
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    sessionParams.device = m_cuContext;
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    NVENC_ZC_API_CALL(m_nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &m_hEncoder));
}

void NvEncoderZeroCopy::CloseEncodeSession()
{
    if (m_hEncoder)
    {
        m_nvenc.nvEncDestroyEncoder(m_hEncoder);
        m_hEncoder = nullptr;
    }
}

void NvEncoderZeroCopy::GetDefaultEncoderParams(NV_ENC_INITIALIZE_PARAMS* pInitializeParams,
                                                 GUID presetGuid, NV_ENC_TUNING_INFO tuningInfo)
{
    if (!m_hEncoder)
    {
        NVENC_ZC_THROW_ERROR("Encoder session not initialized", NV_ENC_ERR_NO_ENCODE_DEVICE);
    }

    if (!pInitializeParams || !pInitializeParams->encodeConfig)
    {
        NVENC_ZC_THROW_ERROR("Invalid parameters", NV_ENC_ERR_INVALID_PTR);
    }

    memset(pInitializeParams->encodeConfig, 0, sizeof(NV_ENC_CONFIG));
    NV_ENC_CONFIG* pEncodeConfig = pInitializeParams->encodeConfig;
    memset(pInitializeParams, 0, sizeof(NV_ENC_INITIALIZE_PARAMS));
    pInitializeParams->encodeConfig = pEncodeConfig;

    pInitializeParams->version = NV_ENC_INITIALIZE_PARAMS_VER;
    pInitializeParams->encodeConfig->version = NV_ENC_CONFIG_VER;

    pInitializeParams->encodeGUID = m_codecGuid;
    pInitializeParams->presetGUID = presetGuid;
    pInitializeParams->encodeWidth = m_nWidth;
    pInitializeParams->encodeHeight = m_nHeight;
    pInitializeParams->darWidth = m_nWidth;
    pInitializeParams->darHeight = m_nHeight;
    pInitializeParams->frameRateNum = 30;
    pInitializeParams->frameRateDen = 1;
    pInitializeParams->enablePTD = 1;
    pInitializeParams->maxEncodeWidth = m_nWidth;
    pInitializeParams->maxEncodeHeight = m_nHeight;
    pInitializeParams->tuningInfo = tuningInfo;

    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER, 0, { NV_ENC_CONFIG_VER } };
    m_nvenc.nvEncGetEncodePresetConfigEx(m_hEncoder, m_codecGuid, presetGuid, tuningInfo, &presetConfig);

    memcpy(pInitializeParams->encodeConfig, &presetConfig.presetCfg, sizeof(NV_ENC_CONFIG));
}

void NvEncoderZeroCopy::CreateEncoder(const NV_ENC_INITIALIZE_PARAMS* pInitializeParams)
{
    if (!m_hEncoder)
    {
        NVENC_ZC_THROW_ERROR("Encoder session not initialized", NV_ENC_ERR_NO_ENCODE_DEVICE);
    }

    if (m_bEncoderInitialized)
    {
        NVENC_ZC_THROW_ERROR("Encoder already initialized", NV_ENC_ERR_ENCODER_BUSY);
    }

    memcpy(&m_initializeParams, pInitializeParams, sizeof(m_initializeParams));
    if (pInitializeParams->encodeConfig)
    {
        memcpy(&m_encodeConfig, pInitializeParams->encodeConfig, sizeof(m_encodeConfig));
        m_initializeParams.encodeConfig = &m_encodeConfig;
    }

    if (m_codecGuid == NV_ENC_CODEC_AV1_GUID &&
        m_initializeParams.splitEncodeMode != NV_ENC_SPLIT_DISABLE_MODE &&
        m_encodeConfig.frameIntervalP <= 1)
    {
        ConfigureAV1CustomTiles();
    }
    NVENC_ZC_API_CALL(m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams));
    m_bEncoderInitialized = true;

    uint32_t numBFrames = m_encodeConfig.frameIntervalP > 0 ? m_encodeConfig.frameIntervalP - 1 : 0;
    uint32_t lookahead = (m_encodeConfig.rcParams.enableLookahead) ? m_encodeConfig.rcParams.lookaheadDepth : 0;

    m_bHasBFrames = (numBFrames > 0);
    m_nEncoderBufferCount = numBFrames + lookahead + m_nExtraOutputDelay + 4;

    m_nOutputDelay = m_encodeConfig.frameIntervalP + m_encodeConfig.rcParams.lookaheadDepth;
}

void NvEncoderZeroCopy::ConfigureAV1CustomTiles()
{
    static const uint32_t SB_SIZE = 64;
    static const uint32_t AV1_MAX_TILE_WIDTH_PIXELS = 4096;
    static const uint32_t AV1_MAX_TILE_AREA_PIXELS  = 4096 * 2304;
    static const uint32_t BL_ALIGN_SB_ROWS = 4;

    uint32_t picWidthSbs  = (m_nWidth  + SB_SIZE - 1) / SB_SIZE;
    uint32_t picHeightSbs = (m_nHeight + SB_SIZE - 1) / SB_SIZE;

    uint32_t maxTileWidthSbs = AV1_MAX_TILE_WIDTH_PIXELS / SB_SIZE;
    uint32_t numTileCols = (picWidthSbs + maxTileWidthSbs - 1) / maxTileWidthSbs;
    if (numTileCols == 0) numTileCols = 1;
    if (numTileCols > NV_MAX_TILE_COLS_AV1) numTileCols = NV_MAX_TILE_COLS_AV1;

    for (uint32_t i = 0; i < numTileCols; i++)
        m_av1TileWidths[i] = ((i + 1) * picWidthSbs / numTileCols) - (i * picWidthSbs / numTileCols);

    uint32_t maxTileAreaSbs = AV1_MAX_TILE_AREA_PIXELS / (SB_SIZE * SB_SIZE);
    uint32_t widestTileSbs = m_av1TileWidths[0];
    for (uint32_t i = 1; i < numTileCols; i++)
        if (m_av1TileWidths[i] > widestTileSbs) widestTileSbs = m_av1TileWidths[i];

    uint32_t frameAreaSbs = picWidthSbs * picHeightSbs;
    uint32_t min_log2_tiles = 0;
    while ((frameAreaSbs >> min_log2_tiles) > maxTileAreaSbs) min_log2_tiles++;
    uint32_t effectiveMaxArea = (min_log2_tiles > 0) ? (frameAreaSbs >> (min_log2_tiles + 1)) : frameAreaSbs;
    uint32_t maxTileHeightSbs = (effectiveMaxArea > 0) ? effectiveMaxArea / widestTileSbs : picHeightSbs;
    if (maxTileHeightSbs == 0) maxTileHeightSbs = 1;

    uint32_t numAlignedUnits = picHeightSbs / BL_ALIGN_SB_ROWS;
    uint32_t baseTileHeight = (numAlignedUnits > 0)
        ? (numAlignedUnits / BL_ALIGN_SB_ROWS) * BL_ALIGN_SB_ROWS
        : BL_ALIGN_SB_ROWS;

    uint32_t maxAligned = (maxTileHeightSbs / BL_ALIGN_SB_ROWS) * BL_ALIGN_SB_ROWS;
    if (maxAligned > 0 && baseTileHeight > maxAligned)
        baseTileHeight = maxAligned;
    if (baseTileHeight == 0)
        baseTileHeight = BL_ALIGN_SB_ROWS;

    uint32_t numTileRows = (picHeightSbs + baseTileHeight - 1) / baseTileHeight;
    if (numTileRows <= 1 || numTileRows > NV_MAX_TILE_ROWS_AV1)
        return;

    uint32_t sum = 0;
    for (uint32_t i = 0; i < numTileRows - 1; i++)
    {
        m_av1TileHeights[i] = baseTileHeight;
        sum += baseTileHeight;
    }
    m_av1TileHeights[numTileRows - 1] = picHeightSbs - sum;

    NV_ENC_CONFIG_AV1& av1Cfg = m_encodeConfig.encodeCodecConfig.av1Config;
    av1Cfg.enableCustomTileConfig = 1;
    av1Cfg.numTileColumns = numTileCols;
    av1Cfg.tileWidths = m_av1TileWidths;
    av1Cfg.numTileRows = numTileRows;
    av1Cfg.tileHeights = m_av1TileHeights;
}

void NvEncoderZeroCopy::DestroyEncoder()
{
    if (!m_bEncoderInitialized)
    {
        return;
    }

    UnregisterInputArrays();
    ReleaseOutputBuffers();

    m_bEncoderInitialized = false;
}

void NvEncoderZeroCopy::RegisterInputArrays(const std::vector<CUarray>& cudaArrays,
                                            unsigned int width, unsigned int height, unsigned int pitch)
{
    if (cudaArrays.empty())
    {
        NVENC_ZC_THROW_ERROR("Empty CUDA array list", NV_ENC_ERR_INVALID_PARAM);
    }

    if (!m_hEncoder || !m_bEncoderInitialized)
    {
        NVENC_ZC_THROW_ERROR("Encoder not initialized", NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
    }

    UnregisterInputArrays();

    m_bUseExtCudaArray = true;
    m_vInputSurfaces.resize(cudaArrays.size());

    unsigned int bufHeight = GetBufferHeight();

    CUDA_ENC_ZC_CALL(cuCtxPushCurrent(m_cuContext));

    for (size_t i = 0; i < cudaArrays.size(); i++)
    {
        ZeroCopyInputSurface& surface = m_vInputSurfaces[i];
        memset(&surface, 0, sizeof(surface));

        surface.cudaArray = cudaArrays[i];
        surface.width = width;
        surface.height = height;
        surface.bufHeight = bufHeight;
        surface.pitch = pitch;
        surface.bufferFormat = m_eBufferFormat;
        surface.isMapped = false;

        NV_ENC_REGISTER_RESOURCE registerResource = { NV_ENC_REGISTER_RESOURCE_VER };
        registerResource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
        registerResource.resourceToRegister = (void*)cudaArrays[i];
        registerResource.width = width;
        registerResource.height = height;
        registerResource.pitch = pitch;
        registerResource.bufferFormat = m_eBufferFormat;
        registerResource.bufferUsage = NV_ENC_INPUT_IMAGE;

        NVENC_ZC_API_CALL(m_nvenc.nvEncRegisterResource(m_hEncoder, &registerResource));

        surface.registeredPtr = registerResource.registeredResource;
    }

    CUcontext dummy;
    cuCtxPopCurrent(&dummy);

    AllocateOutputBuffers(static_cast<uint32_t>(cudaArrays.size()));
}

void NvEncoderZeroCopy::UnregisterInputArrays()
{
    if (!m_hEncoder || m_vInputSurfaces.empty())
    {
        return;
    }

    CUDA_ENC_ZC_CALL(cuCtxPushCurrent(m_cuContext));

    for (auto& surface : m_vInputSurfaces)
    {
        if (surface.isMapped && surface.mappedPtr)
        {
            m_nvenc.nvEncUnmapInputResource(m_hEncoder, surface.mappedPtr);
            surface.mappedPtr = nullptr;
            surface.isMapped = false;
        }

        if (surface.registeredPtr)
        {
            m_nvenc.nvEncUnregisterResource(m_hEncoder, surface.registeredPtr);
            surface.registeredPtr = nullptr;
        }
    }

    m_vInputSurfaces.clear();
    m_bUseExtCudaArray = false;

    CUcontext dummy;
    cuCtxPopCurrent(&dummy);
}

void NvEncoderZeroCopy::AllocateOutputBuffers(uint32_t numBuffers)
{
    if (!m_hEncoder || !m_bEncoderInitialized)
    {
        NVENC_ZC_THROW_ERROR("Encoder not initialized", NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
    }

    ReleaseOutputBuffers();

    m_vOutputBuffers.resize(numBuffers);

    for (uint32_t i = 0; i < numBuffers; i++)
    {
        NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
        NVENC_ZC_API_CALL(m_nvenc.nvEncCreateBitstreamBuffer(m_hEncoder, &createBitstreamBuffer));
        m_vOutputBuffers[i] = createBitstreamBuffer.bitstreamBuffer;
    }

    m_nNextOutputIdx = 0;
}

void NvEncoderZeroCopy::ReleaseOutputBuffers()
{
    if (!m_hEncoder)
    {
        return;
    }

    for (auto& outputBuffer : m_vOutputBuffers)
    {
        if (outputBuffer)
        {
            m_nvenc.nvEncDestroyBitstreamBuffer(m_hEncoder, outputBuffer);
            outputBuffer = nullptr;
        }
    }

    m_vOutputBuffers.clear();
}

NV_ENC_INPUT_PTR NvEncoderZeroCopy::MapInputSurface(uint32_t surfaceIdx)
{
    if (surfaceIdx >= m_vInputSurfaces.size())
    {
        NVENC_ZC_THROW_ERROR("Invalid surface index", NV_ENC_ERR_INVALID_PARAM);
    }

    ZeroCopyInputSurface& surface = m_vInputSurfaces[surfaceIdx];

    if (surface.isMapped)
    {
        return surface.mappedPtr;
    }

    NV_ENC_MAP_INPUT_RESOURCE mapResource = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    mapResource.registeredResource = surface.registeredPtr;

    NVENC_ZC_API_CALL(m_nvenc.nvEncMapInputResource(m_hEncoder, &mapResource));

    surface.mappedPtr = mapResource.mappedResource;
    surface.isMapped = true;

    return surface.mappedPtr;
}

void NvEncoderZeroCopy::UnmapInputSurface(uint32_t surfaceIdx)
{
    if (surfaceIdx >= m_vInputSurfaces.size())
    {
        return;
    }

    ZeroCopyInputSurface& surface = m_vInputSurfaces[surfaceIdx];

    if (!surface.isMapped || !surface.mappedPtr)
    {
        return;
    }

    m_nvenc.nvEncUnmapInputResource(m_hEncoder, surface.mappedPtr);
    surface.mappedPtr = nullptr;
    surface.isMapped = false;
}

NVENCSTATUS NvEncoderZeroCopy::EncodeFrame(uint32_t surfaceIdx, NV_ENC_PIC_PARAMS* pPicParams)
{
    std::lock_guard<std::mutex> lock(m_mtxEncoder);

    if (surfaceIdx >= m_vInputSurfaces.size())
    {
        NVENC_ZC_THROW_ERROR("Invalid surface index", NV_ENC_ERR_INVALID_PARAM);
    }

    NV_ENC_INPUT_PTR inputPtr = MapInputSurface(surfaceIdx);

    uint32_t outputIdx = m_nNextOutputIdx % static_cast<uint32_t>(m_vOutputBuffers.size());

    NV_ENC_PIC_PARAMS picParams = {};
    if (pPicParams)
    {
        memcpy(&picParams, pPicParams, sizeof(picParams));
    }
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = inputPtr;
    picParams.bufferFmt = m_eBufferFormat;
    picParams.inputWidth = m_nWidth;
    picParams.inputHeight = m_nHeight;
    picParams.outputBitstream = m_vOutputBuffers[outputIdx];
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.frameIdx = m_nNextOutputIdx;

    NVENCSTATUS status = m_nvenc.nvEncEncodePicture(m_hEncoder, &picParams);

    if (status == NV_ENC_SUCCESS || status == NV_ENC_ERR_NEED_MORE_INPUT)
    {
        m_nNextOutputIdx++;
    }

    return status;
}

void NvEncoderZeroCopy::EndEncode()
{
    NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    m_nvenc.nvEncEncodePicture(m_hEncoder, &picParams);
}

void NvEncoderZeroCopy::GetLockBitstream(uint32_t frameIdx, NV_ENC_OUTPUT_PTR outputBuffer, ZeroCopyOutputFrame& outputFrame)
{
    NV_ENC_LOCK_BITSTREAM lockBitstream = { NV_ENC_LOCK_BITSTREAM_VER };
    lockBitstream.outputBitstream = outputBuffer;
    lockBitstream.doNotWait = 0;

    NVENC_ZC_API_CALL(m_nvenc.nvEncLockBitstream(m_hEncoder, &lockBitstream));

    outputFrame.bitstream.resize(lockBitstream.bitstreamSizeInBytes);
    memcpy(outputFrame.bitstream.data(), lockBitstream.bitstreamBufferPtr, lockBitstream.bitstreamSizeInBytes);
    outputFrame.pictureType = lockBitstream.pictureType;
    outputFrame.timestamp = lockBitstream.outputTimeStamp;
    outputFrame.frameIdx = frameIdx % static_cast<uint32_t>(m_vOutputBuffers.size());
}

void NvEncoderZeroCopy::UnLockBitstream(uint32_t frameIdx, NV_ENC_OUTPUT_PTR outputBuffer)
{
    m_nvenc.nvEncUnlockBitstream(m_hEncoder, outputBuffer);
}

uint32_t NvEncoderZeroCopy::GetRequiredBufferCount() const
{
    return m_nEncoderBufferCount;
}

void NvEncoderZeroCopy::SetIOCudaStreams(NV_ENC_CUSTREAM_PTR inputStream, NV_ENC_CUSTREAM_PTR outputStream)
{
    NVENC_ZC_API_CALL(m_nvenc.nvEncSetIOCudaStreams(m_hEncoder, inputStream, outputStream));
}

uint32_t NvEncoderZeroCopy::GetBufferHeight() const
{
    uint32_t alignedHeight;
    if (m_codecGuid == NV_ENC_CODEC_AV1_GUID)
    {
        alignedHeight = (m_nHeight + 63) & ~63;
    }
    else if (m_codecGuid == NV_ENC_CODEC_HEVC_GUID)
    {
        alignedHeight = (m_nHeight + 31) & ~31;
    }
    else
    {
        alignedHeight = (m_nHeight + 15) & ~15;
    }
    
    uint32_t chromaHeight;

    switch (m_eBufferFormat)
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

    return alignedHeight + chromaHeight;
}

void NvEncoderZeroCopy::GetSequenceParams(std::vector<uint8_t>& seqParams)
{
    if (!m_hEncoder || !m_bEncoderInitialized)
    {
        seqParams.clear();
        return;
    }

    uint32_t spsppsSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD spsppsPayload = { NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER };

    spsppsPayload.inBufferSize = 0;
    spsppsPayload.spsId = 0;
    spsppsPayload.ppsId = 0;
    spsppsPayload.spsppsBuffer = nullptr;
    spsppsPayload.outSPSPPSPayloadSize = &spsppsSize;

    seqParams.resize(1024);
    spsppsPayload.inBufferSize = static_cast<uint32_t>(seqParams.size());
    spsppsPayload.spsppsBuffer = seqParams.data();

    NVENCSTATUS status = m_nvenc.nvEncGetSequenceParams(m_hEncoder, &spsppsPayload);
    if (status != NV_ENC_SUCCESS)
    {
        seqParams.clear();
        return;
    }

    seqParams.resize(spsppsSize);
}

