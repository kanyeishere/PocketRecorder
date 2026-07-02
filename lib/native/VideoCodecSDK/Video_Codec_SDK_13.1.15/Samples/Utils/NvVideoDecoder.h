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

//---------------------------------------------------------------------------
//! \file NvVideoDecoder.h
//! \brief NvVideoDecoder interface built on top of SeekUtils
//!
//! This header provides a simplified decoder interface offering easy 
//! frame access, batch processing, and seeking.
//---------------------------------------------------------------------------

#pragma once

#include <memory>
#include <vector>
#include <string>
#include "SeekUtils.h"
#include "FFmpegDemuxer.h"
#include "../NvCodec/NvDecoder/NvDecoder.h"
#include "NvCodecUtils.h"
#include "DecoderCache.h"

/**
 * @brief Result of ReconfigureDecoder operation
 */
enum class ReconfigureStatus {
    CacheHit,      // Decoder reused from cache
    CacheMiss,     // New decoder created
    ResetState     // Same source, just reset state (no demuxer/decoder recreation)
};

/**
 * @brief Simplified decoder class for easy frame access and seeking
 * 
 * This class provides a high-level interface for video decoding with:
 * - Random frame access via index
 * - Batch frame retrieval
 * - Automatic backward seek handling
 * - Simplified resource management
 * 
 * Example usage:
 * @code
 * NvVideoDecoder decoder("video.mp4", 0);
 * NvDecFrame frame = decoder.GetFrameByIndex(100);
 * std::vector<NvDecFrame> batch = decoder.GetBatchFrames(10);
 * @endcode
 */
class NvVideoDecoder {
private:
    std::unique_ptr<FFmpegDemuxer> m_pDemuxer;
    std::unique_ptr<NvDecoder> m_pDecoder;
    std::unique_ptr<SeekUtils> m_pSeekUtils;
    
    std::string m_strEncSource;
    CUcontext m_cuContext;
    CUstream m_cuStream;
    bool m_bUseDeviceMemory;
    
    // Configuration
    uint32_t m_uMaxWidth;
    uint32_t m_uMaxHeight;
    bool m_bEnableDecodeStats;
    bool m_bNeedScannedMetadata;  // Remember if scanning was needed (for reconfiguration)
    bool m_bEnableOpenGOP;        // Remember OpenGOP state (for reconfiguration)
    bool m_bEnablePTSFrameSkip;        // Remember PTS-based frame skipping enable state (for reconfiguration)
    
    // Cached metadata (to avoid repeated demuxer calls)
    mutable bool m_bMetadataCached;
    mutable StreamMetadata m_cachedMetadata;
    
    // Decoder cache: stores decoders indexed by (bitDepth, codec, chromaFormat)
    using CacheKey = std::tuple<int, cudaVideoCodec, cudaVideoChromaFormat>;
    DecoderCache<CacheKey, NvDecoder*> m_decoderCache;
    
public:
    /**
     * @brief Construct a new NvVideoDecoder object
     * 
     * @param encSource Path to video file or stream URL
     * @param cudaContext External CUDA context
     * @param cudaStream External CUDA stream (NULL = use default)
     * @param useDeviceMemory Keep decoded frames in GPU memory (default: true)
     * @param maxWidth Maximum video width to support (0 = auto)
     * @param maxHeight Maximum video height to support (0 = auto)
     * @param needScannedStreamMetadata Scan stream for keyframe info (default: false)
     * @param decoderCacheSize Number of decoders to cache for reuse (0 = disabled, default: 0)
     * @param enableDecodeStats Enable per-frame decode statistics (default: false)
     * @param enableOpenGOP Enable open GOP support for seeking (default: false)
     */
    NvVideoDecoder(const std::string& encSource,
                  CUcontext cudaContext,
                  CUstream cudaStream = NULL,
                  bool useDeviceMemory = true,
                  uint32_t maxWidth = 0,
                  uint32_t maxHeight = 0,
                  bool needScannedStreamMetadata = false,
                  uint32_t decoderCacheSize = 0,
                  bool enableDecodeStats = false,
                  bool enableOpenGOP = false);
    
    /**
     * @brief Destroy the NvVideoDecoder object
     * Automatically cleans up all resources
     */
    ~NvVideoDecoder();
    
    // Prevent copying
    NvVideoDecoder(const NvVideoDecoder&) = delete;
    NvVideoDecoder& operator=(const NvVideoDecoder&) = delete;
    
    /**
     * @brief Get a batch of sequential frames
     * 
     * @param batchSize Number of frames to retrieve
     * @return std::vector<NvDecFrame> Vector of decoded frames
     * @throws std::runtime_error if batch cannot be retrieved
     */
    std::vector<NvDecFrame> GetBatchFrames(uint32_t batchSize);
    
    /**
     * @brief Operator overload for single frame access using bracket notation (decoder[100])
     * 
     * @param index Frame index
     * @return NvDecFrame Decoded frame
     */
    NvDecFrame operator[](uint32_t index);
    
    /**
     * @brief Operator overload for multiple frame access using bracket notation (decoder[{10,20,30}])
     * 
     * @param indices Vector of frame indices
     * @return std::vector<NvDecFrame> Vector of decoded frames
     */
    std::vector<NvDecFrame> operator[](const std::vector<uint32_t>& indices);
    
    /**
     * @brief Seek to a specific frame index
     * Sets the internal position for subsequent batch operations
     * 
     * @param index Frame index to seek to
     */
    void SeekToIndex(uint32_t index);
    
    /**
     * @brief Get frame index from timestamp in seconds
     * 
     * @param timeInSeconds Timestamp in seconds
     * @return uint32_t Corresponding frame index
     */
    uint32_t GetIndexFromTimeInSeconds(float timeInSeconds);
    
    /**
     * @brief Get stream metadata (basic info)
     * 
     * @return StreamMetadata Structure with width, height, frame count, etc.
     */
    StreamMetadata GetStreamMetadata();
    
    /**
     * @brief Get scanned stream metadata (detailed info with keyframes)
     * Scans the entire video stream to extract:
     * - Keyframe indices (for efficient seeking)
     * - Packet sizes, PTS/DTS values
     * - GOP size (distance between keyframes)
     * - Accurate frame count
     * 
     * Note: This performs a full stream scan and may take time for large files.
     * The demuxer is reset to the beginning after scanning.
     * 
     * @return ScannedStreamMetadata Structure with complete stream analysis
     */
    ScannedStreamMetadata GetScannedStreamMetadata();
    
    /**
     * @brief Reconfigure decoder with a new video source
     * 
     * Efficiently switches to a new video source, automatically reusing cached
     * decoders when codec properties match (codec, bitDepth, chromaFormat).
     * If no matching decoder is cached, creates a new one.
     * 
     * @param newSource Path to new video file
     * @param isSameSource True if this is the same source (preserves keyframe cache), false otherwise
     * @param maxWidth Maximum video width to support (0 = use current setting or auto)
     * @param maxHeight Maximum video height to support (0 = use current setting or auto)
     * @return ReconfigureStatus::CacheHit if decoder was reused, ReconfigureStatus::CacheMiss if new decoder was created
     */
    ReconfigureStatus ReconfigureDecoder(const std::string& newSource, bool isSameSource = false, uint32_t maxWidth = 0, uint32_t maxHeight = 0);
    
    /**
     * @brief Check if the current stream is seekable
     * 
     * @return true if stream supports seeking
     * @return false if stream is sequential only
     */
    bool IsSeekable() const;
    
    /**
     * @brief Get the underlying NvDecoder instance
     * For advanced users who need direct access
     * 
     * @return NvDecoder* Pointer to decoder (do not delete)
     */
    NvDecoder* GetDecoder() { return m_pDecoder.get(); }
    
    /**
     * @brief Get bit depth of the video stream
     * 
     * @return int Bit depth (8, 10, 12, etc.)
     */
    int GetBitDepth() const;
    
    /**
     * @brief Get container format name
     * 
     * @return std::string Container name (e.g., "flv", "matroska,webm", "mov,mp4,m4a,3gp,3g2,mj2")
     */
    std::string GetContainerName() const;
    
    /**
     * @brief Get chroma format of the video stream
     * 
     * @return cudaVideoChromaFormat Chroma format enum
     */
    cudaVideoChromaFormat GetChromaFormat() const;
    
    /**
     * @brief Enable or disable open GOP support
     * When enabled, the decoder will detect IDR frames and use them for seeking
     * in open GOP streams, ensuring proper decoding of non-IDR I-frames.
     * 
     * State is preserved across ReconfigureDecoder() calls.
     * 
     * @param enable true to enable open GOP support, false to disable (default: false)
     */
    void EnableOpenGOPSupport(bool enable) { 
        m_bEnableOpenGOP = enable;
        if (m_pSeekUtils) {
            m_pSeekUtils->EnableOpenGOPSupport(enable);
        }
    }
    
    /**
     * @brief Check if end of stream (EOS) has been reached
     * 
     * This indicates that the decoder has reached the end of the video stream.
     * Useful for detecting when frame retrieval failures are due to EOS vs other errors.
     * 
     * @return true if EOS has been reached, false otherwise
     */
    bool IsEOSReached() const {
        return m_pSeekUtils ? m_pSeekUtils->IsEOSReached() : false;
    }
    
    /**
     * @brief Enable or disable PTS-based frame skipping during seek
     * When disabled, the decoder will not skip frames based on PTS threshold during seeking.
     * This can be useful for debugging or when the optimization causes issues.
     * 
     * State is preserved across ReconfigureDecoder() calls.
     * 
     * @param bEnable true to enable PTS-based frame skipping, false to disable (default: true)
     */
    void SetPTSFrameSkip(bool bEnable) {
        m_bEnablePTSFrameSkip = bEnable;
        if (m_pSeekUtils) {
            m_pSeekUtils->SetPTSFrameSkip(bEnable);
        }
    }
    bool IsPTSFrameSkipEnabled() const {
        return m_bEnablePTSFrameSkip;
    }
private:
    /**
     * @brief Initialize decoder components
     * Called by constructor
     */
    void Initialize(bool needScannedStreamMetadata, bool enableOpenGOP = false);
    
    /**
     * @brief Reset decoder if backward seek is detected
     * 
     * @param targetIndex Target frame index
     */
    void ResetDecoderIfRequired(uint32_t targetIndex);
    
    /**
     * @brief Check if seeking to target would be backward
     * 
     * @param targetIndex Target frame index
     * @return true if backward seek
     */
    bool IsBackwardSeek(uint32_t targetIndex);
    
    /**
     * @brief Validate frame index is within bounds
     * 
     * @param index Frame index to validate
     * @throws std::out_of_range if index is invalid
     */
    void ValidateFrameIndex(uint32_t index);
    
    /**
     * @brief Handle decoder evicted from cache
     * Cleans up the evicted decoder properly
     * 
     * @param decoder Evicted decoder pointer (nullptr if none evicted)
     */
    void HandleEvictedDecoder(NvDecoder* decoder);
    
    /**
     * @brief Scan stream for keyframes and set them in SeekUtils
     * Called during ReconfigureDecoder for new sources when scanning is enabled
     */
    void ScanAndSetKeyFrameIndices();
};


