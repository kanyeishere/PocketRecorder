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
#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/pixdesc.h>
}
#include <stdexcept>
#include <algorithm>
#include <cstdio>  // For SEEK_SET constant used by avio_seek
#include "NvCodecUtils.h"

//---------------------------------------------------------------------------
//! \file FFmpegDemuxer.h 
//! \brief Provides functionality for stream demuxing
//!
//! This header file is used by Decode/Transcode apps to demux input video clips before decoding frames from it. 
//---------------------------------------------------------------------------

// WAR: FFmpeg compatibility: avformat_index_get_entry was added in FFmpeg 5.1 (libavformat 59.16.100)
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(59, 16, 100)
inline const AVIndexEntry* avformat_index_get_entry(AVStream* st, int idx) {
    if (idx < 0 || idx >= st->nb_index_entries) {
        return nullptr;
    }
    return &st->index_entries[idx];
}
#endif

struct StreamMetadata
{
    uint32_t width;
    uint32_t height;
    uint32_t numFrames;
    double averageFPS;
    double duration;
    float bitrate;
    std::string codecName;
};

struct ScannedStreamMetadata
{
    StreamMetadata streamMetadata;
    std::vector<uint32_t> keyFrameIndices;
    std::vector<uint32_t> idrFrameIndices;  // Subset of keyFrameIndices that are IDR frames (for open GOP support)
    std::vector<uint32_t> packetSize;
    std::vector<int64_t> pts;
    std::vector<int64_t> dts;    
};

struct PacketInfo {
    int64_t pts;
    int64_t dts;
    bool isKeyFrame;
    uint32_t packetSize;
};

enum SeekMode {
    /* Seek for exact frame number.
     * Suited for standalone demuxer seek. */
    EXACT_FRAME = 0,

    /* Seek for previous key frame in past.
     * Suitable for seek & decode.  */
     PREV_KEY_FRAME = 1,

     /* Seek for nearest key frame in future.
     * Suitable for seek & decode.  */
     NEAREST_FUTURE_KEY_FRAME = 2,

     SEEK_MODE_NUM_ELEMS
};

enum SeekCriteria {
    /* Seek frame by number.
     */
    BY_NUMBER = 0,

    /* Seek frame by timestamp.
     */
    BY_TIMESTAMP = 1,

    SEEK_CRITERIA_NUM_ELEMS
};

/**
* @brief libavformat wrapper class. Retrieves the elementary encoded stream from the container format.
*/
class FFmpegDemuxer {
private:
    AVFormatContext *fmtc = NULL;
    AVIOContext *avioc = NULL;
    AVPacket* pkt = NULL; /*!< AVPacket stores compressed data typically exported by demuxers and then passed as input to decoders */
    AVPacket* pktFiltered = NULL;
    AVBSFContext *bsfc = NULL;

    int iVideoStream;
    bool bMp4H264, bMp4HEVC, bMp4MPEG4;
    AVCodecID eVideoCodec;
    AVPixelFormat eChromaFormat;
    int nWidth, nHeight, nBitDepth, nBPP, nChromaHeight;
    double timeBase = 0.0;
    int64_t userTimeScale = 0;
    int64_t nPTSoffset = 0; 

    uint8_t *pDataWithHeader = NULL;

    unsigned int frameCount = 0;
    uint8_t *pSPSPPSData = nullptr;  // Cached SPS/PPS extracted from extradata after seek
    int nSPSPPSSize = 0;
    
    ScannedStreamMetadata scannedStreamMetadata;
    
    int iNumAVStreamIndexEntries = -1;  // Cached index entry count (-1 = not calculated yet)

public:
    /**
    *   @brief  Abstract base class for providing custom data input to demuxer.
    *          Implement this interface to stream data from custom sources.
    */
    class DataProvider {
    public:
        virtual ~DataProvider() {}
        virtual int GetData(uint8_t *pBuf, int nBuf) = 0;
    };

private:
    /**
    *   @brief  Private constructor to initialize libavformat resources and codec parameters.
    *   @param  fmtc - Pointer to AVFormatContext allocated inside avformat_open_input()
    *   @param  timeScale - Time scale for timestamp calculations (default: 1 Hz)
    */
    FFmpegDemuxer(AVFormatContext *fmtc, int64_t timeScale = 1 /*Hz*/) : 
        fmtc(fmtc),
        iVideoStream(-1),
        bMp4H264(false),
        bMp4HEVC(false),
        bMp4MPEG4(false),
        eVideoCodec(AV_CODEC_ID_NONE),
        eChromaFormat(AV_PIX_FMT_NONE),
        nWidth(0),
        nHeight(0),
        nBitDepth(0),
        nBPP(0),
        nChromaHeight(0) {
        if (!fmtc) {
            LOG(ERROR) << "No AVFormatContext provided.";
            return;
        }

        // Allocate the AVPackets and initialize to default values
        pkt = av_packet_alloc();
        pktFiltered = av_packet_alloc();
        if (!pkt || !pktFiltered) {
            LOG(ERROR) << "AVPacket allocation failed";
            return;
        }

        // Validate iformat pointer before accessing
        if (!fmtc->iformat) {
            LOG(ERROR) << "Invalid format context";
            av_packet_free(&pkt);
            av_packet_free(&pktFiltered);
            return;
        }
        
        LOG(INFO) << "Media format: " << fmtc->iformat->long_name << " (" << fmtc->iformat->name << ")";

        ck(avformat_find_stream_info(fmtc, NULL));
        iVideoStream = av_find_best_stream(fmtc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (iVideoStream < 0) {
            LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Could not find stream in input file";
            av_packet_free(&pkt);
            av_packet_free(&pktFiltered);
            return;
        }

        //fmtc->streams[iVideoStream]->need_parsing = AVSTREAM_PARSE_NONE;
        eVideoCodec = fmtc->streams[iVideoStream]->codecpar->codec_id;
        nWidth = fmtc->streams[iVideoStream]->codecpar->width;
        nHeight = fmtc->streams[iVideoStream]->codecpar->height;
        eChromaFormat = (AVPixelFormat)fmtc->streams[iVideoStream]->codecpar->format;
        AVRational rTimeBase = fmtc->streams[iVideoStream]->time_base;
        timeBase = av_q2d(rTimeBase);
        userTimeScale = timeScale;

        // Set bit depth, chroma height, bits per pixel based on eChromaFormat of input
        switch (eChromaFormat)
        {
        case AV_PIX_FMT_YUV420P10LE:
        case AV_PIX_FMT_GRAY10LE:   // monochrome is treated as 420 with chroma filled with 0x0
            nBitDepth = 10;
            nChromaHeight = (nHeight + 1) >> 1;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV420P12LE:
            nBitDepth = 12;
            nChromaHeight = (nHeight + 1) >> 1;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV444P10LE:
        case AV_PIX_FMT_GBRP10LE:   // 10-bit planar RGB (G-B-R planes), treated as 444
            nBitDepth = 10;
            nChromaHeight = nHeight << 1;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV444P12LE:
        case AV_PIX_FMT_GBRP12LE:   // 12-bit planar RGB (G-B-R planes), treated as 444
            nBitDepth = 12;
            nChromaHeight = nHeight << 1;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_GBRP:       // 8-bit planar RGB (G-B-R planes), treated as 444
        case AV_PIX_FMT_GBRAP:      // 8-bit planar RGB with alpha, treated as 444
            nBitDepth = 8;
            nChromaHeight = nHeight << 1;
            nBPP = 1;
            break;
        case AV_PIX_FMT_GBRAP10LE:  // 10-bit planar RGB with alpha, treated as 444
            nBitDepth = 10;
            nChromaHeight = nHeight << 1;
            nBPP = 2;
            break;
        case AV_PIX_FMT_GBRAP12LE:  // 12-bit planar RGB with alpha, treated as 444
            nBitDepth = 12;
            nChromaHeight = nHeight << 1;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV422P10LE:
            nBitDepth = 10;
            nChromaHeight = nHeight;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV422P12LE:
            nBitDepth = 12;
            nChromaHeight = nHeight;
            nBPP = 2;
            break;
        case AV_PIX_FMT_YUV422P:
            nBitDepth = 8;
            nChromaHeight = nHeight;
            nBPP = 1;
            break;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:   // jpeg decoder output is subsampled to NV12 for 422/444 so treat it as 420
        case AV_PIX_FMT_YUVJ444P:   // jpeg decoder output is subsampled to NV12 for 422/444 so treat it as 420
        case AV_PIX_FMT_GRAY8:      // monochrome is treated as 420 with chroma filled with 0x0
            nBitDepth = 8;
            nChromaHeight = (nHeight + 1) >> 1;
            nBPP = 1;
            break;
        default:
            LOG(WARNING) << "ChromaFormat not recognized (pixel format: " << av_get_pix_fmt_name(eChromaFormat) << "). Assuming 420";
            eChromaFormat = AV_PIX_FMT_YUV420P;
            nBitDepth = 8;
            nChromaHeight = (nHeight + 1) >> 1;
            nBPP = 1;
        }

        bMp4H264 = eVideoCodec == AV_CODEC_ID_H264 && (
                !strcmp(fmtc->iformat->long_name, "QuickTime / MOV") 
                || !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)") 
                || !strcmp(fmtc->iformat->long_name, "Matroska / WebM")
            );
        bMp4HEVC = eVideoCodec == AV_CODEC_ID_HEVC && (
                !strcmp(fmtc->iformat->long_name, "QuickTime / MOV")
                || !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)")
                || !strcmp(fmtc->iformat->long_name, "Matroska / WebM")
            );

        bMp4MPEG4 = eVideoCodec == AV_CODEC_ID_MPEG4 && (
                !strcmp(fmtc->iformat->long_name, "QuickTime / MOV")
                || !strcmp(fmtc->iformat->long_name, "FLV (Flash Video)")
                || !strcmp(fmtc->iformat->long_name, "Matroska / WebM")
            );

        // Initialize bitstream filter and its required resources
        if (bMp4H264) {
            const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
            if (!bsf) {
                LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "av_bsf_get_by_name() failed";
                av_packet_free(&pkt);
                av_packet_free(&pktFiltered);
                return;
            }
            ck(av_bsf_alloc(bsf, &bsfc));
            avcodec_parameters_copy(bsfc->par_in, fmtc->streams[iVideoStream]->codecpar);
            ck(av_bsf_init(bsfc));
        }
        if (bMp4HEVC) {
            const AVBitStreamFilter *bsf = av_bsf_get_by_name("hevc_mp4toannexb");
            if (!bsf) {
                LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "av_bsf_get_by_name() failed";
                av_packet_free(&pkt);
                av_packet_free(&pktFiltered);
                return;
            }
            ck(av_bsf_alloc(bsf, &bsfc));
            avcodec_parameters_copy(bsfc->par_in, fmtc->streams[iVideoStream]->codecpar);
            ck(av_bsf_init(bsfc));
        }
    }

    /**
    *   @brief  Create AVFormatContext from custom data provider for streaming input.
    *   @param  pDataProvider - Custom data provider implementing GetData interface
    *   @return Pointer to allocated AVFormatContext, NULL on error
    */
    AVFormatContext *CreateFormatContext(DataProvider *pDataProvider) {

        AVFormatContext *ctx = NULL;
        if (!(ctx = avformat_alloc_context())) {
            LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
            return NULL;
        }

        uint8_t *avioc_buffer = NULL;
        int avioc_buffer_size = 32 * 1024 * 1024;
        avioc_buffer = (uint8_t *)av_malloc(avioc_buffer_size);
        if (!avioc_buffer) {
            LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
            return NULL;
        }
        avioc = avio_alloc_context(avioc_buffer, avioc_buffer_size,
            0, pDataProvider, &ReadPacket, NULL, NULL);
        if (!avioc) {
            LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
            return NULL;
        }
        ctx->pb = avioc;

        ck(avformat_open_input(&ctx, NULL, NULL, NULL));
        return ctx;
    }

    /**
    *   @brief  Create AVFormatContext from file path for local file input.
    *   @param  szFilePath - File path pointing to input media file
    *   @return Pointer to allocated AVFormatContext, NULL on error
    */
     AVFormatContext *CreateFormatContext(const char *szFilePath) {
        avformat_network_init();

        AVFormatContext *ctx = NULL;
        ck(avformat_open_input(&ctx, szFilePath, NULL, NULL));
        return ctx;
    }

    /**
    *   @brief  Check if SPS/PPS are present in the packet data
    *   @param  data - Packet data to check
    *   @param  size - Size of packet data
    *   @return True if SPS/PPS are found, false otherwise
    */
    bool HasSPSPPSInPacket(const uint8_t* data, int size) {
        if (!data || size < 4) {
            return false;
        }
        
        // Look for start codes (0x00 0x00 0x01 or 0x00 0x00 0x00 0x01) followed by SPS/PPS NAL units
        for (int i = 0; i <= size - 4; i++) {
            if ((data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01) ||
                (i <= size - 5 && data[i] == 0x00 && data[i+1] == 0x00 && 
                 data[i+2] == 0x00 && data[i+3] == 0x01)) {
                int nalOffset = (data[i+2] == 0x01) ? i + 3 : i + 4;
                if (nalOffset < size) {
                    uint8_t nalType = data[nalOffset] & 0x1F;  // H.264: lower 5 bits
                    if (nalType == 7 || nalType == 8) {  // SPS (7) or PPS (8)
                        return true;
                    }
                    if (bMp4HEVC) {
                        nalType = (data[nalOffset] >> 1) & 0x3F;  // HEVC: bits 1-6
                        if (nalType >= 32 && nalType <= 34) {  // VPS (32), SPS (33), PPS (34)
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    /**
    *   @brief  Extract SPS/PPS from extradata and convert to Annex-B format
    *   @param  extraDataSize - Size of extradata
    */
    void ExtractSPSPPSFromExtradata(int extraDataSize) {
        if (pSPSPPSData) {
            av_freep(&pSPSPPSData);
            nSPSPPSSize = 0;
        }
        
        if (extraDataSize > 0 && bMp4H264 && extraDataSize >= 6) {
            // H.264 AVCC format: [config(5 bytes)] [numSPS(1)] [SPS lengths/data] [numPPS(1)] [PPS lengths/data]
            // Convert to Annex-B format (start-code prefixed)
            static constexpr int MAX_PPS_COUNT = 255;   // H.264 AVCC spec: 8-bit field
            const uint8_t* extradata = fmtc->streams[iVideoStream]->codecpar->extradata;
            int numSPS = extradata[5] & 0x1F;  // Lower 5 bits per AVCC spec, bounded to [0, 31]
            int offset = 6;
            
            // Calculate total size needed for Annex-B format
            int totalSize = 0;
            int tempOffset = offset;
            
            // Count SPS
            for (int i = 0; i < numSPS && tempOffset + 2 <= extraDataSize; i++) {
                int spsLen = (extradata[tempOffset] << 8) | extradata[tempOffset + 1];
                if (spsLen <= 0 || spsLen > extraDataSize) break;
                tempOffset += 2;
                if (tempOffset + spsLen <= extraDataSize) {
                    totalSize += 4 + spsLen;  // Start code (4 bytes) + SPS data
                    tempOffset += spsLen;
                } else {
                    break;
                }
            }
            
            // Count PPS
            if (tempOffset < extraDataSize) {
                int numPPS = extradata[tempOffset++];
                if (numPPS > MAX_PPS_COUNT) numPPS = MAX_PPS_COUNT;
                for (int i = 0; i < numPPS && tempOffset + 2 <= extraDataSize; i++) {
                    int ppsLen = (extradata[tempOffset] << 8) | extradata[tempOffset + 1];
                    if (ppsLen <= 0 || ppsLen > extraDataSize) break;
                    tempOffset += 2;
                    if (tempOffset + ppsLen <= extraDataSize) {
                        totalSize += 4 + ppsLen;  // Start code (4 bytes) + PPS data
                        tempOffset += ppsLen;
                    } else {
                        break;
                    }
                }
            }
            
            if (totalSize > 0) {
                pSPSPPSData = (uint8_t*)av_malloc(totalSize);
                if (pSPSPPSData) {
                    uint8_t* dst = pSPSPPSData;
                    tempOffset = offset;
                    
                    // Convert SPS to Annex-B
                    for (int i = 0; i < numSPS && tempOffset + 2 <= extraDataSize; i++) {
                        int spsLen = (extradata[tempOffset] << 8) | extradata[tempOffset + 1];
                        if (spsLen <= 0 || spsLen > extraDataSize) break;
                        tempOffset += 2;
                        if (tempOffset + spsLen <= extraDataSize) {
                            dst[0] = 0x00; dst[1] = 0x00; dst[2] = 0x00; dst[3] = 0x01;
                            memcpy(dst + 4, extradata + tempOffset, spsLen);
                            dst += 4 + spsLen;
                            tempOffset += spsLen;
                        } else {
                            break;
                        }
                    }
                    
                    // Convert PPS to Annex-B
                    if (tempOffset < extraDataSize) {
                        int numPPS = extradata[tempOffset++];
                        if (numPPS > MAX_PPS_COUNT) numPPS = MAX_PPS_COUNT;
                        for (int i = 0; i < numPPS && tempOffset + 2 <= extraDataSize; i++) {
                            int ppsLen = (extradata[tempOffset] << 8) | extradata[tempOffset + 1];
                            if (ppsLen <= 0 || ppsLen > extraDataSize) break;
                            tempOffset += 2;
                            if (tempOffset + ppsLen <= extraDataSize) {
                                dst[0] = 0x00; dst[1] = 0x00; dst[2] = 0x00; dst[3] = 0x01;
                                memcpy(dst + 4, extradata + tempOffset, ppsLen);
                                dst += 4 + ppsLen;
                                tempOffset += ppsLen;
                            } else {
                                break;
                            }
                        }
                    }
                    
                    nSPSPPSSize = dst - pSPSPPSData;
                }
            }
        }
    }

    /**
    *   @brief  Process a video packet with codec-specific handling
    *   @param  ppData      - Pointer to data buffer
    *   @param  pnBytes     - Pointer to number of bytes
    *   @param  pts         - Pointer to presentation timestamp
    *   @param  dts         - Pointer to decode timestamp  
    *   @param  duration    - Pointer to packet duration
    *   @return True if successful, false on error
    */
    bool ProcessVideoPacket(uint8_t **ppData, int *pnBytes, int64_t *pts = NULL, int64_t* dts = NULL, int64_t* duration = NULL, bool* isKeyFrame = NULL) {
        if (bMp4H264 || bMp4HEVC) {
            if (pktFiltered->data) {
                av_packet_unref(pktFiltered);
            }
            ck(av_bsf_send_packet(bsfc, pkt));
            ck(av_bsf_receive_packet(bsfc, pktFiltered));
            
            // After seeking (frameCount == 0), check if BSF already inserted SPS/PPS
            // Only prepend manually if BSF didn't insert them (avoid double-insertion)
            if (frameCount == 0 && pktFiltered->data && pktFiltered->size > 0) {
                bool hasSPSPPS = HasSPSPPSInPacket(pktFiltered->data, pktFiltered->size);
                
                if (!hasSPSPPS && pSPSPPSData && nSPSPPSSize > 0) {
                    // BSF didn't insert SPS/PPS, prepend manually as fallback
                    // Free previous allocation if any (from previous seek)
                    if (pDataWithHeader) {
                        av_free(pDataWithHeader);
                        pDataWithHeader = NULL;
                    }
                    pDataWithHeader = (uint8_t*)av_malloc(nSPSPPSSize + pktFiltered->size);
                    if (!pDataWithHeader) {
                        LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
                        return false;
                    }
                    memcpy(pDataWithHeader, pSPSPPSData, nSPSPPSSize);
                    memcpy(pDataWithHeader + nSPSPPSSize, pktFiltered->data, pktFiltered->size);
                    *ppData = pDataWithHeader;
                    *pnBytes = nSPSPPSSize + pktFiltered->size;
                } else {
                    // BSF already inserted SPS/PPS, use BSF output directly
                    *ppData = pktFiltered->data;
                    *pnBytes = pktFiltered->size;
                }
            } else {
                *ppData = pktFiltered->data;
                *pnBytes = pktFiltered->size;
            }
            if (pts)
                *pts = pktFiltered->pts;
            if (dts)
                *dts = pktFiltered->dts;
            if (duration)
                *duration = pktFiltered->duration;
            if (isKeyFrame)
                *isKeyFrame = pktFiltered->flags & AV_PKT_FLAG_KEY;
        } else {
            if (bMp4MPEG4 && frameCount == 0) {
                int extraDataSize = fmtc->streams[iVideoStream]->codecpar->extradata_size;
                if (extraDataSize > 0) {
                    // extradata contains start codes 00 00 01. Subtract its size
                    // Free previous allocation if any (from previous seek)
                    if (pDataWithHeader) {
                        av_free(pDataWithHeader);
                        pDataWithHeader = NULL;
                    }
                    pDataWithHeader = (uint8_t*)av_malloc(extraDataSize + pkt->size - 3 * sizeof(uint8_t));
                    if (!pDataWithHeader) {
                        LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__;
                        return false;
                    }
                    memcpy(pDataWithHeader, fmtc->streams[iVideoStream]->codecpar->extradata, extraDataSize);
                    memcpy(pDataWithHeader + extraDataSize, pkt->data + 3, pkt->size - 3 * sizeof(uint8_t));
                    *ppData = pDataWithHeader;
                    *pnBytes = extraDataSize + pkt->size - 3 * sizeof(uint8_t);
                }
            } else {
                *ppData = pkt->data;
                *pnBytes = pkt->size;
            }
            if (pts)
                *pts = pkt->pts;
            if (dts)
                *dts = pkt->dts;
            if (duration)
                *duration = pkt->duration;
            if (isKeyFrame)
                *isKeyFrame = pkt->flags & AV_PKT_FLAG_KEY;
        }
        frameCount++;
        return true;
    }
public:
    /**
    *   @brief  Constructor for file-based demuxing.
    *   @param  szFilePath - Path to input media file
    *   @param  timescale - Time scale for timestamp calculations (default: 1 Hz)
    */
    FFmpegDemuxer(const char *szFilePath, int64_t timescale = 1 /*Hz*/) : FFmpegDemuxer(CreateFormatContext(szFilePath), timescale) {}
    
    /**
    *   @brief  Constructor for streaming demuxing with custom data provider.
    *   @param  pDataProvider - Custom data provider for streaming input
    */
    FFmpegDemuxer(DataProvider *pDataProvider) : FFmpegDemuxer(CreateFormatContext(pDataProvider)) {
        if (fmtc) {
            avioc = fmtc->pb;
        }
    }
    
    /**
    *   @brief  Destructor - cleanup all allocated FFmpeg resources.
    */
    ~FFmpegDemuxer() {

        if (!fmtc) {
            return;
        }

        if (pkt) {
            av_packet_free(&pkt);
        }
        if (pktFiltered) {
            av_packet_free(&pktFiltered);
        }

        if (bsfc) {
            av_bsf_free(&bsfc);
        }

        avformat_close_input(&fmtc);

        if (avioc) {
            av_freep(&avioc->buffer);
            av_freep(&avioc);
        }

        if (pDataWithHeader) {
            av_free(pDataWithHeader);
        }

        if (pSPSPPSData) {
            av_freep(&pSPSPPSData);
            nSPSPPSSize = 0;
        }
    }
    
    /**
    *   @brief  Set the approximate PTS offset for seeking.
    *   Calculates the PTS offset based on B-frame count for accurate seeking.
    */
    void SetPTSOffset()
    {
        // return;
        if (eVideoCodec == AV_CODEC_ID_MPEG4) {
            // Create a new format context for temporary use
            AVFormatContext* tempFmtc = NULL;
            ck(avformat_open_input(&tempFmtc, fmtc->url, NULL, NULL));
            
            // Find video stream in the temporary context
            int tempVideoStream = av_find_best_stream(tempFmtc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if (tempVideoStream < 0) {
                avformat_close_input(&tempFmtc);
                LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Could not find video stream in temp context";
                throw std::runtime_error("Could not find video stream in temp context");
            }

            // Allocate packet for temporary use
            AVPacket* tempPkt = av_packet_alloc();
            if (!tempPkt) {
                avformat_close_input(&tempFmtc);
                LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Failed to allocate temporary packet";
                throw std::runtime_error("Failed to allocate temporary packet");
            }

            // Read first packet to get PTS/DTS
            while (av_read_frame(tempFmtc, tempPkt) >= 0) {
                if (tempPkt->stream_index == tempVideoStream) {
                    if (tempPkt->flags & AV_PKT_FLAG_KEY && tempPkt->duration > 0) {
                        nPTSoffset = tempPkt->pts - tempPkt->dts;
                        av_packet_unref(tempPkt);
                        break;
                    }
                }
                av_packet_unref(tempPkt);
            }

            // Cleanup temporary resources
            av_packet_free(&tempPkt);
            avformat_close_input(&tempFmtc);
        } else {
            // Original logic for non-MPEG4 codecs
            uint8_t* pData = nullptr;
            int nBytes = 0;
            int64_t pts = 0;
            int64_t dts = 0;
            int64_t duration = 0;
            bool isKeyFrame = false;
            
            // Clear any existing packet data before seeking
            if (pkt && pkt->data) {
                av_packet_unref(pkt);
            }
            if (pktFiltered && pktFiltered->data) {
                av_packet_unref(pktFiltered);
            }
            
            // Flush BSF context if it exists (clears any buffered packets)
            if (bsfc) {
                av_bsf_flush(bsfc);
            }
            
            int rv = av_seek_frame(
                fmtc, iVideoStream, 0, AVSEEK_FLAG_BACKWARD);
            if (rv < 0)
            {
                LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Failed to seek.";
                throw std::runtime_error("Failed to seek.");
            }
            
            bool bRes = this->DemuxVideo(&pData, &nBytes, &pts, &dts, &duration, &isKeyFrame);
            if (isKeyFrame && bRes && duration > 0)
            {
                nPTSoffset = pts - dts;
            }
            else
            {
                nPTSoffset = 0;
            }
            
            // Reset demuxer state after reading packet
            // Clear any existing packet data
            if (pkt && pkt->data) {
                av_packet_unref(pkt);
            }
            if (pktFiltered && pktFiltered->data) {
                av_packet_unref(pktFiltered);
            }
            
            // Flush BSF context again to ensure clean state
            if (bsfc) {
                av_bsf_flush(bsfc);
            }
            
            rv = av_seek_frame(
                fmtc, iVideoStream, 0, AVSEEK_FLAG_BACKWARD);
            if (rv < 0)
            {
                LOG(ERROR) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Failed to seek.";
                throw std::runtime_error("Failed to seek.");
            }
        }
    }

    /**
    *   @brief  Get the approximate PTS offset for seeking.
    *   @return PTS offset value
    */
    int64_t GetPTSOffset() const
    {
        return nPTSoffset;
    }
    
    /**
    *   @brief  Get the underlying AVFormatContext pointer.
    *   @return Pointer to AVFormatContext
    */
    AVFormatContext* GetAVFormatContext() {
        return fmtc;
    }
    
    /**
    *   @brief  Get the video codec ID of the input stream.
    *   @return AVCodecID enum value
    */
    AVCodecID GetVideoCodec() {
        return eVideoCodec;
    }
    
    /**
    *   @brief  Get the chroma format of the video stream.
    *   @return AVPixelFormat enum value
    */
    AVPixelFormat GetChromaFormat() {
        return eChromaFormat;
    }
    
    /**
    *   @brief  Get the width of the video frame in pixels.
    *   @return Frame width
    */
    int GetWidth() {
        return nWidth;
    }
    
    /**
    *   @brief  Get the height of the video frame in pixels.
    *   @return Frame height
    */
    int GetHeight() {
        return nHeight;
    }
    
    /**
    *   @brief  Get the bit depth of the video stream.
    *   @return Bit depth (8, 10, 12, etc.)
    */
    int GetBitDepth() {
        return nBitDepth;
    }
    
    /**
    *   @brief  Calculate total frame size in bytes including chroma planes.
    *   @return Total frame size in bytes
    */
    int GetFrameSize() {
        return nWidth * (nHeight + nChromaHeight) * nBPP;
    }
    
    /**
    *   @brief  Check if the last demuxed packet was a keyframe.
    *   @return True if last packet was a keyframe, false otherwise
    */
    bool IsLastPacketKeyframe() {
        return pkt && (pkt->flags & AV_PKT_FLAG_KEY);
    }
    
    /**
    *   @brief  Demux next packet from the input stream (video or non-video).
    *   @param  ppData         - Pointer to receive packet data
    *   @param  pnBytes        - Pointer to receive packet size
    *   @param  pts            - Pointer to receive presentation timestamp (optional)
    *   @param  dts            - Pointer to receive decode timestamp (optional)
    *   @param  duration       - Pointer to receive packet duration (optional)
    *   @param  isVideoPacket  - Pointer to receive video packet flag (optional)
    *   @param  streamIndex    - Pointer to receive stream index (optional)
    *   @return True if packet available, false if end of stream
    */
    bool Demux(uint8_t **ppData, int *pnBytes, int64_t *pts = NULL, int64_t* dts = NULL, int64_t* duration = NULL, int *isVideoPacket = NULL, int *streamIndex = NULL) {
        if (!fmtc) {
            return false;
        }

        *pnBytes = 0;

        if (pkt->data) {
            av_packet_unref(pkt);
        }

        if (av_read_frame(fmtc, pkt) < 0) {
            if (isVideoPacket)
                *isVideoPacket = 1;
            if (streamIndex)
                *streamIndex = iVideoStream;
            return false;
        }

        if (pkt->stream_index == iVideoStream)
        {
            if (!ProcessVideoPacket(ppData, pnBytes, pts, dts, duration)) {
                return false; // Error in video packet processing
            }
            if (isVideoPacket)
                *isVideoPacket = 1;
        }
        else
        {
            *ppData = pkt->data;
            *pnBytes = pkt->size;
            if (pts)
                *pts = pkt->pts;
            if (dts)
                *dts = pkt->dts;
            if (duration)
                *duration = pkt->duration;
            if (isVideoPacket)
                *isVideoPacket = 0;
        }

        if (streamIndex)
            *streamIndex = pkt->stream_index;

        return true;
    }

    /**
    *   @brief  Demux and return only video packets, automatically filtering out non-video packets
    *   @param  ppData      - Pointer to data buffer
    *   @param  pnBytes     - Pointer to number of bytes
    *   @param  pts         - Pointer to presentation timestamp
    *   @param  dts         - Pointer to decode timestamp  
    *   @param  duration    - Pointer to packet duration
    *   @return True if video packet found, false if end of stream
    */
    bool DemuxVideo(uint8_t **ppData, int *pnBytes, int64_t *pts = NULL, int64_t* dts = NULL, int64_t* duration = NULL, bool *isKeyFrame = NULL) {
        if (!fmtc || !pkt || !pktFiltered) {
            return false;
        }

        *pnBytes = 0;

        // Keep reading packets until we find a video packet or reach end of stream
        while (true) {
            if (pkt->data) {
                av_packet_unref(pkt);
            }

            if (av_read_frame(fmtc, pkt) < 0) {
                return false; // End of stream
            }

            // Check if this is a video packet
            if (pkt->stream_index == iVideoStream) {
                // This is a video packet - process it using the common function
                if (!ProcessVideoPacket(ppData, pnBytes, pts, dts, duration, isKeyFrame)) {
                    return false; // Error in video packet processing
                }
                return true; // Successfully found and processed a video packet
            }
            // If not a video packet, continue loop to read next packet
        }
    }

    /**
    *   @brief  Static callback function for custom data provider reading.
    *   @param  opaque - Pointer to DataProvider instance
    *   @param  pBuf   - Buffer to fill with data
    *   @param  nBuf   - Size of buffer in bytes
    *   @return Number of bytes read, or negative on error
    */
    static int ReadPacket(void *opaque, uint8_t *pBuf, int nBuf) {
        return ((DataProvider *)opaque)->GetData(pBuf, nBuf);
    }

    // Extended functionality for seek support
    
    /**
    *   @brief  Get the video stream pointer.
    *   @return Pointer to AVStream for video stream
    */
    AVStream* GetVideoStream() {
        if (fmtc != nullptr && iVideoStream >= 0) {
            return fmtc->streams[iVideoStream];
        }
        return nullptr;
    }
    
    /**
    *   @brief  Check if the stream supports seeking.
    *   @return True if seeking is supported, false otherwise
    */
    bool IsSeekable() {
        if (!fmtc) {
            return false;
        }
        // Check if format supports seeking via AVFMT_SEEK_TO_PTS flag
        if (fmtc->iformat && (fmtc->iformat->flags & AVFMT_SEEK_TO_PTS)) {
            return true;
        }
        // FLV and WebM are seekable even if they don't have AVFMT_SEEK_TO_PTS flag
        std::string container = GetContainerName();
        if (container == "flv" || container == "matroska,webm") {
            return true;
        }
        return false;
    }
    
    /**
    *   @brief  Try to reset the stream to the beginning using low-level avio_seek.
    *           This is useful for non-seekable streams that support byte-level seeking.
    *   @return True if reset was successful, false otherwise
    */
    bool TryResetViaAvioSeek() {
        if (!fmtc || !fmtc->pb) {
            return false;
        }
        // Check if AVIOContext supports seeking
        if (fmtc->pb->seekable & AVIO_SEEKABLE_NORMAL) {
            // Try low-level avio_seek to reset to byte position 0
            int64_t ret = avio_seek(fmtc->pb, 0, SEEK_SET);
            return (ret >= 0);
        }
        return false;
    }
    
    /**
    *   @brief  Get the container format name.
    *   @return String containing the container format name
    */
    std::string GetContainerName() const {
        if (!fmtc || !fmtc->iformat) {
            return "unknown";
        }
        
        // Check if it's a mov/mp4 container
        if (strcmp(fmtc->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0) {
            // Check major_brand metadata to differentiate
            AVDictionaryEntry* tag = av_dict_get(fmtc->metadata, "major_brand", nullptr, 0);
            if (tag) {
                if (strcmp(tag->value, "qt  ") == 0) {
                    return "mov";
                } else if (strcmp(tag->value, "mp42") == 0 || 
                          strcmp(tag->value, "isom") == 0 ||
                          strcmp(tag->value, "mp41") == 0) {
                    return "mp4";
                }
            }
            
            // Fallback to extension check if metadata not available
            const char* filename = fmtc->url;
            const char* ext = strrchr(filename, '.');
            if (ext) {
                if (_stricmp(ext, ".mp4") == 0) {
                    return "mp4";
                } else if (_stricmp(ext, ".mov") == 0) {
                    return "mov";
                }
            }
        }
        
        // For other formats, return the format name
        return fmtc->iformat->name;
    }
    
    /**
    *   @brief  Get PTS from FFmpeg index entry for a specific frame.
    *   @param  pavStream - Pointer to AVStream
    *   @param  frameIdx - Frame index
    *   @return PTS value from index entry, or -1 if not available
    */
    int64_t GetPTSFromIndex(AVStream* pavStream, int frameIdx) const {
        const AVIndexEntry* entry = avformat_index_get_entry(pavStream, frameIdx);
        if (entry) {
            return (entry->timestamp + nPTSoffset);
        }
        return -1;  // Index entry not available
    }
    
    /**
    *   @brief  Convert frame number to PTS.
    *   @param  pavStream - Pointer to AVStream
    *   @param  frame - Frame number
    *   @return PTS value for the frame
    */
    int64_t FrameToPts(AVStream* pavStream, int frame) const {
        std::string container = GetContainerName();
        
        // Calculate PTS from frame number and frame rate
        int64_t numerator = int64_t(frame) * pavStream->r_frame_rate.den * pavStream->time_base.den;
        int64_t denominator = int64_t(pavStream->r_frame_rate.num) * pavStream->time_base.num;
        int64_t calculatePTS = (numerator + denominator - 1) / denominator;

        // For flv and matroska/webm, skip index lookup and calculate directly
        if (container == "flv" || container == "matroska,webm") {
            return calculatePTS;
        }

        // For other containers, try index entry first, then fallback to calculation
        int64_t indexPTS = GetPTSFromIndex(pavStream, frame);
        return (indexPTS >= 0) ? indexPTS : calculatePTS;
    }
    
    /**
    *   @brief  Convert DTS to frame number.
    *   @param  dts - Decode timestamp
    *   @return Frame number corresponding to the DTS
    */
    int64_t dts_to_frame_number(int64_t dts) {
        std::string container = GetContainerName();
        double sec;
        
        if (container == "flv" || container == "mov") {
            // FLV and MOV use direct timebase conversion without start_time adjustment
            sec = (double)(dts) * timeBase;
        } else {
            // Other containers need start_time adjustment
            sec = (double)(dts - fmtc->streams[iVideoStream]->start_time) * timeBase;
        }
        
        double fps = av_q2d(fmtc->streams[iVideoStream]->avg_frame_rate);
        if (fps <= 0) {
            fps = av_q2d(av_guess_frame_rate(fmtc, fmtc->streams[iVideoStream], nullptr));
        }
        if (fps <= 0) {
            fps = 1.0 / av_q2d(fmtc->streams[iVideoStream]->time_base);
        }
        
        return (int64_t)(fps * sec + 0.5);
    }
    
    /**
    *   @brief  Convert time in seconds to timestamp.
    *   @param  ts_sec - Time in seconds
    *   @return Timestamp in stream timebase units
    */
    int64_t TsFromTime(double ts_sec) {
        /* Internal timestamp representation is integer, so multiply to AV_TIME_BASE
         * and switch to fixed point precision arithmetics; */
        auto const ts_tbu = llround(ts_sec * AV_TIME_BASE);
        
        // Rescale the timestamp to value represented in stream base units;
        AVRational factor;
        factor.num = 1;
        factor.den = AV_TIME_BASE;
        
        return av_rescale_q(ts_tbu, factor, fmtc->streams[iVideoStream]->time_base);
    }
    
    int32_t FrameIndexFromTime(double ts_sec) {
        AVStream* stream = fmtc->streams[iVideoStream];
        
        // Get number of stored index entries in the stream and cache it
        if (iNumAVStreamIndexEntries == -1) {
            iNumAVStreamIndexEntries = 0;
            while (iNumAVStreamIndexEntries < INT_MAX) {
                const AVIndexEntry* entry = avformat_index_get_entry(stream, iNumAVStreamIndexEntries);
                if (entry == nullptr) {
                    break;
                }
                iNumAVStreamIndexEntries++;
            }
        }

        // Check if index entries match total frames
        if (iNumAVStreamIndexEntries > 0 && stream->nb_frames > 0 && iNumAVStreamIndexEntries == stream->nb_frames) {
            // Rescale timestamp to the timebase of the video stream and use av_index_search_timestamp
            int64_t rescaledPTS = TsFromTime(ts_sec);
            
            // Get the index entry for the timestamp = 0, this is offset PTS
            const AVIndexEntry* entry = avformat_index_get_entry(stream, 0);
            if (!entry) {
                LOG(ERROR) << "No index entries available for seeking.";
                throw std::runtime_error("No index entries available for seeking.");
            }
            
            int64_t offsetPTS = entry->timestamp;
            // Get the nearest keyframe index entry for the timestamp
            int64_t actualPTS = rescaledPTS + offsetPTS;
            
            int actualIdx = av_index_search_timestamp(stream, actualPTS, AVSEEK_FLAG_ANY);
            if (actualIdx == -1) {
                double duration = (double)fmtc->duration / AV_TIME_BASE;
                std::string errString = "Provided timestamp : " + std::to_string(ts_sec) + " seconds" + 
                                        " is greater than the total duration of the clip " + 
                                        std::to_string(duration) + " seconds" + "\n";
                LOG(ERROR) << errString;
                throw std::runtime_error(errString);
            }
            return actualIdx;
        } else {
            // Sparse index: calculate frame number directly from time using frame rate
            // Prefer avg_frame_rate (more accurate) over r_frame_rate (estimate)
            double fps = av_q2d(stream->avg_frame_rate);
            if (fps <= 0) {
                // Fallback to r_frame_rate if avg_frame_rate is invalid
                if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
                    fps = (double)stream->r_frame_rate.num / stream->r_frame_rate.den;
                } else {
                    // Try to guess frame rate from stream
                    fps = av_q2d(av_guess_frame_rate(fmtc, stream, nullptr));
                }
            }
            if (fps <= 0) {
                // Last resort: calculate from time_base
                fps = 1.0 / av_q2d(stream->time_base);
            }
            if (fps <= 0) {
                LOG(ERROR) << "Invalid frame rate for time-based frame calculation.";
                throw std::runtime_error("Invalid frame rate for time-based frame calculation.");
            }
            // Calculate frame number: frame = time_in_seconds * fps
            int32_t frameNum = static_cast<int32_t>(llround(ts_sec * fps));
            return frameNum;
        }
    }

    /**
    *   @brief  Seek to a specific frame index.
    *   @param  frameIdx - Frame index to seek to
    *   @return True if seek was successful, false otherwise
    */
    bool Seek(uint32_t frameIdx) {
        if (!IsSeekable()) {
            LOG(ERROR) << "Seek isn't supported for this input.";
            return false;
        }

        int64_t iSeekTargetPTS = FrameToPts(fmtc->streams[iVideoStream], frameIdx);
      
        // Perform seek with fallback (common for all container types)
        int rv = avformat_seek_file(fmtc, iVideoStream, INT64_MIN, iSeekTargetPTS, iSeekTargetPTS, AVSEEK_FLAG_BACKWARD);
        if (rv < 0) {
            // Fallback to av_seek_frame if avformat_seek_file fails
            rv = av_seek_frame(fmtc, iVideoStream, iSeekTargetPTS, AVSEEK_FLAG_BACKWARD);
            if (rv < 0) {
                LOG(ERROR) << "Failed to seek to frame " << frameIdx;
                return false;
            }
        }
        // This clears stale packets that may have been read before the seek operation
        if (pkt && pkt->data) {
            av_packet_unref(pkt);
        }
        if (pktFiltered && pktFiltered->data) {
            av_packet_unref(pktFiltered);
        }
        
        // Flush BSF context after seeking to clear any buffered packets
        if (bsfc) {
            av_bsf_flush(bsfc);
        }
        
        // Extract SPS/PPS from extradata after seeking for manual prepending
        // BSF flush alone isn't sufficient - we need to prepend SPS/PPS manually
        if (bMp4H264 || bMp4HEVC) {
            int extraDataSize = fmtc->streams[iVideoStream]->codecpar->extradata_size;
            ExtractSPSPPSFromExtradata(extraDataSize);
        }
        
        // Reset frameCount after seeking to ensure extradata/SPS/PPS is prepended
        // This ensures the first packet after seek gets proper headers prepended
        frameCount = 0;
        
        return true;
    }
    
    /**
    *   @brief  Get stream metadata information.
    *   @return StreamMetadata struct containing stream information
    */
    StreamMetadata GetStreamMetadata() {
        StreamMetadata streamMetadata = {};
        std::string container = GetContainerName();
        if (container == "matroska,webm" || container == "mov" || container == "flv") {
            // Return cached scanned metadata if available
            if (scannedStreamMetadata.streamMetadata.numFrames > 0) {
                return scannedStreamMetadata.streamMetadata;
            }
        }
        
        // Otherwise, fetch basic metadata
        streamMetadata.width = (uint32_t)nWidth;
        streamMetadata.height = (uint32_t)nHeight;
        streamMetadata.numFrames = (uint32_t)fmtc->streams[iVideoStream]->nb_frames;
        double avgFPS = av_q2d(fmtc->streams[iVideoStream]->avg_frame_rate);
        if (avgFPS <= 0) {
            avgFPS = av_q2d(av_guess_frame_rate(fmtc, fmtc->streams[iVideoStream], nullptr));
        }
        if (avgFPS <= 0) {
            avgFPS = 1.0 / av_q2d(fmtc->streams[iVideoStream]->time_base);
        }
        streamMetadata.averageFPS = avgFPS;
        streamMetadata.duration = (double)fmtc->duration / AV_TIME_BASE;
        streamMetadata.bitrate = (float)fmtc->bit_rate;
        streamMetadata.codecName = avcodec_get_name(eVideoCodec);
        return streamMetadata;
    }

    /**
    *   @brief  Update scanned stream metadata.
    *   @param  metadata - ScannedStreamMetadata to update with
    */
    void UpdateScannedStreamMetadata(const ScannedStreamMetadata& metadata) {
        scannedStreamMetadata = metadata;
    }

    /**
    *   @brief  Check if a packet contains an IDR frame (or IDR-equivalent keyframe).
    *   For H.264/HEVC: Parses NAL units to distinguish IDR from non-IDR keyframes (CRA)
    *   For other supported codecs: All keyframes are treated as IDR (no open GOP concept)
    *   @param  pkt - AVPacket to analyze
    *   @param  codec - Codec ID
    *   @return True if packet contains IDR frame, false otherwise
    */
    bool IsIDRFrame(const AVPacket* pkt, AVCodecID codec) const {
        if (!pkt || !pkt->data || pkt->size < 5) {
            return false;
        }
        
        // Parse NAL units in the packet
        const uint8_t* data = pkt->data;
        int size = pkt->size;
        
        if (codec == AV_CODEC_ID_H264) {
            // H.264: Look for NAL type 5 (IDR)
            // Check if this is Annex-B format (start codes) or AVCC format (length-prefixed)
            bool isAnnexB = (size >= 4 && data[0] == 0 && data[1] == 0 && 
                            (data[2] == 1 || (data[2] == 0 && data[3] == 1)));
            
            if (isAnnexB) {
                // Annex-B format: [start code] [NAL header] [payload]
                // Start code: 0x00 0x00 0x01 or 0x00 0x00 0x00 0x01
                for (int i = 0; i < size - 4; i++) {
                    // Find start code
                    if (data[i] == 0 && data[i+1] == 0) {
                        int start_code_len = 0;
                        if (data[i+2] == 1) {
                            start_code_len = 3;
                        } else if (data[i+2] == 0 && data[i+3] == 1) {
                            start_code_len = 4;
                        } else {
                            continue;
                        }
                        
                        // NAL header is next byte after start code
                        if (i + start_code_len < size) {
                            uint8_t nal_header = data[i + start_code_len];
                            uint8_t nal_type = nal_header & 0x1F;  // Lower 5 bits
                            
                            if (nal_type == 5) {  // IDR slice
                                return true;
                            }
                        }
                    }
                }
            } else {
                // AVCC format: [length (4 bytes)] [NAL header] [payload]
                // Used in MP4/MOV/FLV/WebM containers
                static constexpr int MAX_NAL_UNITS = 4096;
                int offset = 0;
                for (int nalIdx = 0; nalIdx < MAX_NAL_UNITS && offset >= 0 && offset + 4 < size; nalIdx++) {
                    // Read NAL unit length (big-endian 4 bytes)
                    uint32_t nal_length = (data[offset] << 24) | (data[offset+1] << 16) | 
                                         (data[offset+2] << 8) | data[offset+3];
                    
                    if (nal_length == 0 || static_cast<int>(nal_length) < 0 ||
                        offset + 4 + static_cast<int>(nal_length) > size) {
                        break;  // Invalid or overflowed length
                    }
                    
                    // NAL header is after the length field
                    uint8_t nal_header = data[offset + 4];
                    uint8_t nal_type = nal_header & 0x1F;  // Lower 5 bits
                    
                    if (nal_type == 5) {  // IDR slice
                        return true;
                    }
                    
                    // Move to next NAL unit
                    offset += 4 + nal_length;
                }
            }
        } else if (codec == AV_CODEC_ID_HEVC) {
            // HEVC: Look for NAL type 19-20 (IDR_W_RADL, IDR_N_LP) or 21 (CRA)
            // Check if this is Annex-B format (start codes) or HVCC format (length-prefixed)
            bool isAnnexB = (size >= 4 && data[0] == 0 && data[1] == 0 && 
                            (data[2] == 1 || (data[2] == 0 && data[3] == 1)));
            
            if (isAnnexB) {
                // Annex-B format: [start code] [NAL header] [payload]
                for (int i = 0; i < size - 4; i++) {
                    // Find start code
                    if (data[i] == 0 && data[i+1] == 0) {
                        int start_code_len = 0;
                        if (data[i+2] == 1) {
                            start_code_len = 3;
                        } else if (data[i+2] == 0 && data[i+3] == 1) {
                            start_code_len = 4;
                        } else {
                            continue;
                        }
                        
                        // HEVC NAL header is 2 bytes after start code
                        if (i + start_code_len + 1 < size) {
                            uint8_t nal_header_byte0 = data[i + start_code_len];
                            uint8_t nal_type = (nal_header_byte0 >> 1) & 0x3F;  // Bits 1-6
                            
                            // Only NAL types 19-20 are IDR (IDR_W_RADL, IDR_N_LP)
                            // NAL type 21 is CRA (not IDR) - should not be included here
                            if (nal_type == 19 || nal_type == 20) {  // IDR only
                                return true;
                            }
                        }
                    }
                }
            } else {
                // HVCC format: [length (4 bytes)] [NAL header] [payload]
                // Used in MP4/MOV/FLV/WebM containers
                static constexpr int MAX_NAL_UNITS = 4096;
                int offset = 0;
                for (int nalIdx = 0; nalIdx < MAX_NAL_UNITS && offset >= 0 && offset + 4 < size; nalIdx++) {
                    // Read NAL unit length (big-endian 4 bytes)
                    uint32_t nal_length = (data[offset] << 24) | (data[offset+1] << 16) | 
                                         (data[offset+2] << 8) | data[offset+3];
                    
                    if (nal_length == 0 || static_cast<int>(nal_length) < 0 ||
                        offset + 4 + static_cast<int>(nal_length) > size) {
                        break;  // Invalid or overflowed length
                    }
                    
                    // HEVC NAL header is 2 bytes after the length field
                    if (offset + 4 + 1 < size) {
                        uint8_t nal_header_byte0 = data[offset + 4];
                        uint8_t nal_type = (nal_header_byte0 >> 1) & 0x3F;  // Bits 1-6
                        
                        // Only NAL types 19-20 are IDR (IDR_W_RADL, IDR_N_LP)
                        // NAL type 21 is CRA (not IDR) - should not be included here
                        if (nal_type == 19 || nal_type == 20) {  // IDR only
                            return true;
                        }
                    }
                    
                    // Move to next NAL unit
                    offset += 4 + nal_length;
                }
            }
        } else {
            // For other codecs (AV1, VP9, VP8, MPEG2, etc.):
            // They don't have the H.264/HEVC concept of "non-IDR keyframes" (CRA)
            // All keyframes in these codecs are self-contained (IDR-like)
            // So if the packet is marked as a keyframe, treat it as IDR
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                return true;
            }
        }
        
        return false;
    }

    /**
    *   @brief  Get scanned stream metadata synchronously.
    *   Scans the stream to extract keyframe indices and packet information.
    *   @param  scanForIDR - If true, parse NAL units to identify IDR frames (for Open GOP support)
    *   @return ScannedStreamMetadata with complete stream information
    */
    ScannedStreamMetadata GetScannedStreamMetadata(bool scanForIDR = false) {
        // Check if we already have cached scanned metadata
        if (!scannedStreamMetadata.keyFrameIndices.empty()) {
            return scannedStreamMetadata;
        }

        ScannedStreamMetadata metadata = {};
        
        if (!fmtc || !IsSeekable()) {
            return metadata;
        }
        
        // Get basic metadata
        metadata.streamMetadata.width = (uint32_t)nWidth;
        metadata.streamMetadata.height = (uint32_t)nHeight;
        metadata.streamMetadata.numFrames = (uint32_t)fmtc->streams[iVideoStream]->nb_frames;
        
        double avgFPS = av_q2d(fmtc->streams[iVideoStream]->avg_frame_rate);
        if (avgFPS <= 0) {
            avgFPS = av_q2d(av_guess_frame_rate(fmtc, fmtc->streams[iVideoStream], nullptr));
        }
        if (avgFPS <= 0) {
            avgFPS = 1.0 / av_q2d(fmtc->streams[iVideoStream]->time_base);
        }
        metadata.streamMetadata.averageFPS = avgFPS;
        metadata.streamMetadata.duration = (double)fmtc->duration / AV_TIME_BASE;
        metadata.streamMetadata.bitrate = (float)fmtc->bit_rate;
        metadata.streamMetadata.codecName = avcodec_get_name(eVideoCodec);
        
        // Scan stream for keyframes and packet info
        // Save current state for restoration
        int64_t saved_frame_count = frameCount;
        
        // Create a temporary packet for scanning
        AVPacket* scan_pkt = av_packet_alloc();
        if (!scan_pkt) {
            return metadata;
        }
        
        // Seek to beginning of stream
        int ret = av_seek_frame(fmtc, iVideoStream, 0, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            av_packet_free(&scan_pkt);
            return metadata;
        }
        
        uint32_t frame_idx = 0;
        
        // Estimate capacity to avoid reallocations (use numFrames from stream if available)
        // Rough estimate: ~1 keyframe per second for 30fps video
        constexpr uint32_t ESTIMATED_FPS = 30;
        constexpr uint32_t MAX_KEYFRAME_INDICES = 10000;  // Cap to limit memory: 4 bytes/element × 10000 = ~40KB storage + small vector overhead
        uint32_t estimatedKeyFrames = metadata.streamMetadata.numFrames > 0 ? 
                                    (uint32_t)metadata.streamMetadata.numFrames / ESTIMATED_FPS : MAX_KEYFRAME_INDICES;
        estimatedKeyFrames = (estimatedKeyFrames > MAX_KEYFRAME_INDICES) ? MAX_KEYFRAME_INDICES : estimatedKeyFrames;
        
        metadata.keyFrameIndices.reserve(estimatedKeyFrames);
        if (scanForIDR) {
            metadata.idrFrameIndices.reserve(estimatedKeyFrames);
        }
        
        // Read all packets to collect keyframe and timing info
        while (av_read_frame(fmtc, scan_pkt) >= 0) {
            if (scan_pkt->stream_index == iVideoStream) {
                // Check if this is a keyframe
                if (scan_pkt->flags & AV_PKT_FLAG_KEY) {
                    metadata.keyFrameIndices.push_back(frame_idx);
                    
                    // Additionally check if this is an IDR frame (for open GOP support)
                    // Only perform this check if requested (saves CPU time when not needed)
                    if (scanForIDR && IsIDRFrame(scan_pkt, eVideoCodec)) {
                        metadata.idrFrameIndices.push_back(frame_idx);
                    }
                }
                
                // Store packet information
                metadata.packetSize.push_back((uint32_t)scan_pkt->size);
                metadata.pts.push_back(scan_pkt->pts);
                metadata.dts.push_back(scan_pkt->dts);
                
                frame_idx++;
            }
            av_packet_unref(scan_pkt);
        }
        
        av_packet_free(&scan_pkt);
        
        // Update frame count with accurate scanned value
        if (frame_idx > 0) {
            metadata.streamMetadata.numFrames = frame_idx;
        }
        
        // Restore to beginning state (don't seek back to saved position as it may be invalid after scan)
        if (av_seek_frame(fmtc, iVideoStream, 0, AVSEEK_FLAG_BACKWARD) < 0) {
            LOG(WARNING) << "FFmpeg error: " << __FILE__ << " " << __LINE__ << " " << "Failed to seek to beginning after scan.";
        }
        frameCount = 0;
        
        // Cache the scanned metadata for future use
        scannedStreamMetadata = metadata;
        
        return metadata;
    }
};

/**
*   @brief  Convert FFmpeg codec ID to NVIDIA CUDA Video codec ID.
*   @param  id - FFmpeg AVCodecID enum value
*   @return Corresponding cudaVideoCodec enum value, cudaVideoCodec_NumCodecs if unsupported
*/
inline cudaVideoChromaFormat AVPixelFormat2NvChromaFormat(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV420P12LE:
        return cudaVideoChromaFormat_420;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_NV16:
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV422P12LE:
        return cudaVideoChromaFormat_422;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV444P10LE:
    case AV_PIX_FMT_YUV444P12LE:
    case AV_PIX_FMT_GBRP:       // Planar RGB (G-B-R planes), treated as 444
    case AV_PIX_FMT_GBRP10LE:   // 10-bit planar RGB, treated as 444
    case AV_PIX_FMT_GBRP12LE:   // 12-bit planar RGB, treated as 444
    case AV_PIX_FMT_GBRAP:      // Planar RGB with alpha, treated as 444
    case AV_PIX_FMT_GBRAP10LE:  // 10-bit planar RGB with alpha, treated as 444
    case AV_PIX_FMT_GBRAP12LE:  // 12-bit planar RGB with alpha, treated as 444
        return cudaVideoChromaFormat_444;
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_GRAY10LE:
    case AV_PIX_FMT_GRAY12LE:
        return cudaVideoChromaFormat_Monochrome;
    default:
        LOG(WARNING) << "ChromaFormat not recognized (pixel format: " << av_get_pix_fmt_name(format) << "). Assuming 420";
        return cudaVideoChromaFormat_420;  // Default to most common format
    }
}

inline cudaVideoCodec FFmpeg2NvCodecId(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_MPEG1VIDEO : return cudaVideoCodec_MPEG1;
    case AV_CODEC_ID_MPEG2VIDEO : return cudaVideoCodec_MPEG2;
    case AV_CODEC_ID_MPEG4      : return cudaVideoCodec_MPEG4;
    case AV_CODEC_ID_WMV3       :
    case AV_CODEC_ID_VC1        : return cudaVideoCodec_VC1;
    case AV_CODEC_ID_H264       : return cudaVideoCodec_H264;
    case AV_CODEC_ID_HEVC       : return cudaVideoCodec_HEVC;
    case AV_CODEC_ID_VP8        : return cudaVideoCodec_VP8;
    case AV_CODEC_ID_VP9        : return cudaVideoCodec_VP9;
    case AV_CODEC_ID_MJPEG      : return cudaVideoCodec_JPEG;
    case AV_CODEC_ID_AV1        : return cudaVideoCodec_AV1;
    default                     : return cudaVideoCodec_NumCodecs;
    }
}


