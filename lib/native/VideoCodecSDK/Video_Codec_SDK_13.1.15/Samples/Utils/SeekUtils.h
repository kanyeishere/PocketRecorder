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
//! \file SeekUtils.h
//! \brief Seek functionality for VideoCodecSDK
//!
//! This header provides seek functionality for frame-by-frame seeking, 
//! batch operations, and GOP-aware seeking.
//---------------------------------------------------------------------------

#pragma once

#include "NvCodecUtils.h"
#include "NvDecoder/NvDecoder.h"
#include "FFmpegDemuxer.h"
#include <mutex>
#include <algorithm>

#include <vector>
#include <deque>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

enum class SeekStatus {
    INVALID_INDEX_ENTRY = -2
};

/**
 * @brief Frame processing status returned by ProcessDecodedFrame
 * 
 */
enum class FrameProcessStatus {
    CONTINUE_DECODE         = 0,    // Not target, continue decoding
    FOUND_TARGET            = 1,    // Found exact target match
    STOP_DECODE             = 2     // Passed target, stop decoding
};

struct NvDecFrame {
    CUdeviceptr data;           // GPU frame buffer pointer
    int64_t timestamp;          // Frame timestamp
    uint8_t* decodeStats;       // Decode statistics buffer
    uint32_t decodeStatsSize;   // Decode statistics buffer size
    bool isSkipped;             // True if frame was skipped by seekPTS optimization
    NvDecFrame() : data(0), timestamp(0), decodeStats(nullptr), decodeStatsSize(0), isSkipped(false) {
    }
};

class SeekUtils {
private:
    FFmpegDemuxer* m_pDemuxer;
    NvDecoder* m_pDecoder;
    std::vector<NvDecFrame> m_vTargetFrames;
    std::vector<NvDecFrame> m_vPendingFrames;
    int32_t m_nPreviousTargetIndex;
    uint32_t m_uFrameSizeInBytes;
    uint32_t m_uFramesDecodedTillNow;
    bool m_bDiscontinuityFlag;
    int64_t m_nTargetFramePTS;
    std::vector<int64_t> m_vPreviouslyDecodedFramesPTS;
    AVStream* m_pVideoStream;
    bool m_bEOSReached;
    bool m_bSeekDirectionBackwards;
    bool m_bSeekToIndexSet;
    std::vector<uint32_t> m_vKeyFrameIndices;
    std::vector<uint32_t> m_vIDRFrameIndices;       // IDR frames (subset of keyframes, for open GOP support)
    bool m_bEnableOpenGOP;                          // Enable open GOP detection and handling (default: false)
    bool m_bNeedScannedMetadata;                    // Whether scanned stream metadata is needed (set by user or auto-detected)
    
    // Sequential mode support for non-seekable streams
    bool m_bSeekable;                           // Whether stream supports seeking
    
    // PTS-based frame skipping control
    bool m_bEnablePTSFrameSkip;                // Enable PTS-based frame skipping during seek (default: true)

    // Private helper functions for GetFramesByIdx
    // These helper functions extract common patterns to improve maintainability
    
    // Filter frame by duplicate PTS and target PTS
    bool ShouldSkipFrame(const NvDecFrame& frame);
    
    // Store remaining frames as pending after finding target
    void CacheRemainingFrames(int startIdx, int totalFrames);
    
    // Retrieve and process a decoded frame with skipped frame handling
    FrameProcessStatus ProcessDecodedFrame(uint32_t targetIndex, NvDecFrame& outputFrame, bool skipCheckEnabled = true);
    
    // Search pending frames queue for target
    bool SearchPendingFrames(uint32_t targetIndex, NvDecFrame& outputFrame, uint32_t currentPosOffset);
    
    // Flush decoder and process frames to find target
    // @param useProcessDecodedFrame If true, uses ProcessDecodedFrame (GOP-aware); if false, stores all frames in pending (sequential)
    FrameProcessStatus FlushAndProcessFrames(uint32_t targetIndex, NvDecFrame& outputFrame, const char* context, bool useProcessDecodedFrame = true);
    
    // Main decode loop - demux packets and process frames until target found
    bool DecodeUntilTarget(uint32_t targetIndex, NvDecFrame& outputFrame, const char* context, bool useProcessDecodedFrame);
    
    // Sequential mode implementation
    bool GetFrameSequential(uint32_t targetIndex, NvDecFrame& outputFrame);
    
    // GOP-aware implementations
    bool GetFrameGOPAware(uint32_t targetIndex, NvDecFrame& outputFrame);
    
    // Internal helper for single frame retrieval without unlock/track logic
    // Used internally by GetFramesByIdxList to avoid recursion
    bool GetFrameByIdxInternal(uint32_t targetIndex, NvDecFrame& outputFrame);

public:
    SeekUtils(FFmpegDemuxer* demuxer, NvDecoder* decoder);
    ~SeekUtils();

    // Core seek functionality
    // Single frame retrieval - GOP-aware with frame caching optimization
    bool GetFramesByIdx(uint32_t targetIndex, NvDecFrame& outputFrame);
    
    // Multi-frame retrieval - accepts multiple indices and returns multiple frames
    bool GetFramesByIdxList(const std::vector<uint32_t>& indices, std::vector<NvDecFrame>& outputFrames);
    
    // Batch retrieval
    bool GetFramesByBatch(uint32_t batchsize, std::vector<NvDecFrame>& outputFrames);
    
    // Keyframe management
    int GetNearestKeyFrameIndexForTarget(AVStream* stream, int64_t idx);
    int GetNearestIDRFrameIndexForTarget(AVStream* stream, int64_t idx);
    void SetKeyFrameIndices(const std::vector<uint32_t>& keyFrameIndices);
    void SetIDRFrameIndices(const std::vector<uint32_t>& idrFrameIndices);
    const std::vector<uint32_t>& GetKeyFrameIndices() const { return m_vKeyFrameIndices; }
    const std::vector<uint32_t>& GetIDRFrameIndices() const { return m_vIDRFrameIndices; }
    
    // Seek position tracking
    int64_t GetPreviousTargetIndex() const { return m_nPreviousTargetIndex; }
    
    // Open GOP detection
    bool IsOpenGOP() const;
    void EnableOpenGOPSupport(bool enable) { m_bEnableOpenGOP = enable; }
    bool IsOpenGOPSupportEnabled() const { return m_bEnableOpenGOP; }
    
    // Scanned metadata control
    void SetNeedScannedMetadata(bool needScan) { m_bNeedScannedMetadata = needScan; }
    bool GetNeedScannedMetadata() const { return m_bNeedScannedMetadata; }
    
    // Frame management
    NvDecFrame GetFrame(bool bLockFrame);
    void UnlockFrames();
    void UnlockFrame(NvDecFrame& decframe);
    

    
    // Seek state management
    void ClearState(bool bForceEOS = false);
    void Initialize(FFmpegDemuxer* demuxer, NvDecoder* decoder);
    void UpdateDemuxer(FFmpegDemuxer* demuxer);
    void SeekToIndex(uint32_t newTargetIdx);
    
    // Utility functions
    uint32_t GetIndexFromTimeInSeconds(float timeInSeconds);
    std::pair<bool, int64_t> ShouldSeek(int64_t previous_target, int64_t current_target);
    int64_t TsFromTime(double ts_sec);
    bool IsSeekBackwards(int64_t newTargetIdx);
    bool IsEOSReached();
    std::vector<NvDecFrame> GetPendingFrames();
    
    /**
     * @brief Check if stream scanning is required
     * For webm/flv/mov containers, scanning is needed if duration or numFrames is 0
     * 
     * @return true if scanning is required
     */
    bool IsScanRequired();
    
    // EOS management
    void setEOS(bool newVal) { m_bEOSReached = newVal; }
    
    // Sequential mode queries (for non-seekable streams)
    bool IsSeekable() const { return m_bSeekable; }
    
    // PTS-based frame skipping control
    void SetPTSFrameSkip(bool bEnable) { m_bEnablePTSFrameSkip = bEnable; }
    bool IsPTSFrameSkipEnabled() const { return m_bEnablePTSFrameSkip; }
};
