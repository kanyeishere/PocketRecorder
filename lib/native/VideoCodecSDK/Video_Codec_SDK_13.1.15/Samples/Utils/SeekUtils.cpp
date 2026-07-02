/*
 * This copyright notice applies to this source file only:
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

#include "SeekUtils.h"
#include <functional>
#include <sstream>
#include <typeinfo>
#include <iostream>
#include <chrono>

SeekUtils::SeekUtils(FFmpegDemuxer* demuxer, NvDecoder* decoder) :
    m_nPreviousTargetIndex(-1),
    m_uFramesDecodedTillNow(0),
    m_uFrameSizeInBytes(0),
    m_nTargetFramePTS(0),
    m_bDiscontinuityFlag(false),
    m_bEOSReached(false),
    m_bSeekDirectionBackwards(false),
    m_bSeekToIndexSet(false),
    m_vKeyFrameIndices(),
    m_bEnableOpenGOP(false),
    m_bNeedScannedMetadata(false),
    m_bSeekable(true),
    m_bEnablePTSFrameSkip(true)
{
    Initialize(demuxer, decoder);
}

SeekUtils::~SeekUtils() {
    UnlockFrames();
}

void SeekUtils::Initialize(FFmpegDemuxer* demuxer, NvDecoder* decoder)
{
    m_pDemuxer = demuxer;
    m_pDecoder = decoder;
    m_pVideoStream = m_pDemuxer->GetVideoStream();

    // Detect if stream is seekable
    m_bSeekable = m_pDemuxer->IsSeekable();
    if (m_bSeekable) {
        // Set the approximate offset PTS
        m_pDemuxer->SetPTSOffset();
    }
}

void SeekUtils::UpdateDemuxer(FFmpegDemuxer* demuxer)
{
    if (!demuxer) {
        throw std::runtime_error("UpdateDemuxer: Invalid demuxer pointer");
    }
    
    m_pDemuxer = demuxer;
    m_pVideoStream = m_pDemuxer->GetVideoStream();

    // Update seekability detection
    m_bSeekable = m_pDemuxer->IsSeekable();
    if (m_bSeekable) {
        // Set the approximate offset PTS
        m_pDemuxer->SetPTSOffset();
    }
}

void SeekUtils::SetKeyFrameIndices(const std::vector<uint32_t>& keyFrameIndices)
{
    m_vKeyFrameIndices = keyFrameIndices;
}

void SeekUtils::SetIDRFrameIndices(const std::vector<uint32_t>& idrFrameIndices)
{
    m_vIDRFrameIndices = idrFrameIndices;
}

/* 
 * Stream has open GOP if:
 * 1. Open GOP support is enabled by the application
 * 2. We have keyframe indices (I-frames/CRA detected)
 * 3. Number of IDR frames is less than total keyframes
 *    - This includes the case where IDRframes=0 (all keyframes are CRA/non-IDR)
 *    - Closed GOP: IDRframes == keyframes (all keyframes are IDR)
 */

bool SeekUtils::IsOpenGOP() const
{
    if (!m_bEnableOpenGOP) {
        return false;  // Open GOP support disabled
    }
    
    bool isOpen = !m_vKeyFrameIndices.empty() && 
                  !m_vIDRFrameIndices.empty() && 
                  m_vIDRFrameIndices.size() < m_vKeyFrameIndices.size();
    

    return isOpen;
}

void SeekUtils::ClearState(bool bForceEOS)
{
    // Unlock previous frames
    UnlockFrames();
    m_vTargetFrames.clear();
    m_vPendingFrames.clear();
    m_vPreviouslyDecodedFramesPTS.clear();
    m_nPreviousTargetIndex = -1;
    m_uFramesDecodedTillNow = 0;
    m_uFrameSizeInBytes = 0;
    m_nTargetFramePTS = 0;
    m_bDiscontinuityFlag = false;
    m_bSeekToIndexSet = false;
    
    // Reset decoder's seek PTS optimization state
    if (m_pDecoder && m_bEnablePTSFrameSkip) {
        m_pDecoder->setSeekPTS(0);
    }
    
    // Only seek if stream is seekable
    if (m_bSeekable) {
        m_pDemuxer->Seek(0);
    }
    
    if (m_bEOSReached || bForceEOS)
    {
        // Null pointer check
        if (!m_pDecoder) {
            LOG(ERROR) << "Decoder pointer is null, cannot flush";
            return;
        }

        PacketData emptyPacket = PacketData();
        int  numDecodedFrames = m_pDecoder->Decode((uint8_t*)emptyPacket.bsl_data,
            (int)emptyPacket.bsl);
        for (int i = 0; i < numDecodedFrames; i++)
        {
            GetFrame(false);
        }
        m_bEOSReached = false;
    }
}

/*
 * Internal helper - Gets a single frame without unlock/track logic
 * Used internally by GetFramesByIdxList to avoid recursion
 */
bool SeekUtils::GetFrameByIdxInternal(uint32_t targetIndex, NvDecFrame& outputFrame)
{
    // Use GOP-aware mode if we have keyframe indices available
    if (!m_vKeyFrameIndices.empty() && m_bSeekable) {
        // Container-based GOP-aware mode
        return GetFrameGOPAware(targetIndex, outputFrame);
    } else {
        // Sequential mode: For non-seekable streams or streams without keyframe index
        return GetFrameSequential(targetIndex, outputFrame);
    }
}

/*
 * Multi-frame version - accepts multiple indices, returns multiple frames
 * Handles unlock/track for proper frame lifecycle management
 */
bool SeekUtils::GetFramesByIdxList(const std::vector<uint32_t>& indices, std::vector<NvDecFrame>& outputFrames)
{
    if (!m_pDemuxer || !m_pDecoder) {
        throw std::runtime_error("SeekUtils::GetFramesByIdxList: Demuxer or decoder pointer is null");
    }
    // Unlock previous target frames (frames returned to application in previous call)
    // Pending frames remain locked to prevent decoder from overwriting them
    for (NvDecFrame frame : m_vTargetFrames)
    {
        uint8_t* framePtr = (uint8_t*)frame.data;
        m_pDecoder->UnlockFrame(&framePtr);
    }
    m_vTargetFrames.clear();
    
    outputFrames.clear();
    outputFrames.reserve(indices.size());
    
    try {
        for (const uint32_t& currentIndex : indices) {
            NvDecFrame frame;
            bool success = GetFrameByIdxInternal(currentIndex, frame);  // Call internal helper
            
            if (success) {
                outputFrames.push_back(frame);
                // Track frame for cleanup before next batch
                m_vTargetFrames.push_back(frame);
            }
        }
        
        return !outputFrames.empty();
    } catch (const std::exception& e) {
        return false;
    } catch (...) {
        return false;
    }
}

NvDecFrame SeekUtils::GetFrame(bool bLockFrame)
{
    int64_t tupTimestamp = 0;
    CUdeviceptr tupData = 0;
    uint8_t *pDecodeStats = nullptr;
    uint32_t decodeStatsSize = 0;
    bool bSkipped = false;
    
    if (bLockFrame)
    {
       tupData = (CUdeviceptr)m_pDecoder->GetLockedFrame(&tupTimestamp, &pDecodeStats, &decodeStatsSize, &bSkipped);
    }
    else
    {
        tupData = (CUdeviceptr)m_pDecoder->GetFrame(&tupTimestamp, &pDecodeStats, &decodeStatsSize, &bSkipped);
    }

    // Create NvDecFrame directly from decoder output
    NvDecFrame frame;
    frame.data = tupData;
    frame.timestamp = tupTimestamp;
    frame.decodeStats = pDecodeStats;
    frame.decodeStatsSize = decodeStatsSize;
    frame.isSkipped = bSkipped;
    
    return frame;
}

int SeekUtils::GetNearestKeyFrameIndexForTarget(AVStream* stream,
    int64_t idx)
{
    std::string container = m_pDemuxer->GetContainerName();
    if (container == "flv"
        || container == "matroska,webm" || container == "mov")
    {
        // Use keyFrameIndices from scanned metadata for FLV and WebM
        if (!m_vKeyFrameIndices.empty())
        {
            // Find the nearest keyframe index that is <= idx
            auto it = std::upper_bound(m_vKeyFrameIndices.begin(), m_vKeyFrameIndices.end(), static_cast<uint32_t>(idx));
            if (it != m_vKeyFrameIndices.begin())
            {
                --it;
                return static_cast<int>(*it);
            }
            else if (!m_vKeyFrameIndices.empty())
            {
                return static_cast<int>(m_vKeyFrameIndices[0]);
            }
        }
        
        // Fallback to original FFmpeg method if keyFrameIndices not available
        int targetpts = (int)m_pDemuxer->FrameToPts(stream, (int)idx);
        int KeyFrameIndex =
            av_index_search_timestamp(stream, targetpts, AVSEEK_FLAG_BACKWARD);
        const AVIndexEntry* entry = avformat_index_get_entry(stream, KeyFrameIndex);
        if (entry == NULL) {
            LOG(WARNING) << "Entry is null for KeyFrameIndex " << KeyFrameIndex;
            return 0;
        }
        int currentKeyFrameIndex = (int)m_pDemuxer->dts_to_frame_number(entry->timestamp);
        return currentKeyFrameIndex;
    }
    else
    {
        const AVIndexEntry* entry = avformat_index_get_entry(stream, (int)idx);
        if (entry == NULL)
        {
            LOG(WARNING) << "Entry is null for index " << idx << "\n";
            return static_cast<int>(SeekStatus::INVALID_INDEX_ENTRY);
        }
        int targetpts = (int)entry->timestamp;
        int currentKeyFrameIndex =
            av_index_search_timestamp(stream, targetpts, AVSEEK_FLAG_BACKWARD);
        return currentKeyFrameIndex;
    }
}

/*
 * Get nearest IDR frame index for target frame.
 * For open GOP streams, this ensures we have all reference frames by seeking to IDR.
 */
int SeekUtils::GetNearestIDRFrameIndexForTarget(AVStream* stream, int64_t idx)
{
    // If no IDR frame indices available, fall back to regular keyframe search
    if (m_vIDRFrameIndices.empty()) {
        return GetNearestKeyFrameIndexForTarget(stream, idx);
    }
    
    // Find the nearest IDR frame index that is <= idx
    auto it = std::upper_bound(m_vIDRFrameIndices.begin(), m_vIDRFrameIndices.end(), static_cast<uint32_t>(idx));
    if (it != m_vIDRFrameIndices.begin()) {
        --it;
        return static_cast<int>(*it);
    } else {
        // If target is before first IDR, use first IDR
        return static_cast<int>(m_vIDRFrameIndices[0]);
    }
}

uint32_t SeekUtils::GetIndexFromTimeInSeconds(float timeInSeconds)
{
    int32_t frameIndex = m_pDemuxer->FrameIndexFromTime(static_cast<double>(timeInSeconds));
    return static_cast<uint32_t>(frameIndex);
}

void SeekUtils::UnlockFrames()
{
    for (NvDecFrame& frame : m_vTargetFrames)
    {
        if (frame.data != 0) {
            UnlockFrame(frame);
        }
    }
    for (NvDecFrame& frame : m_vPendingFrames)
    {
        if (frame.data != 0) {
            UnlockFrame(frame);
        }
    }
    
    m_vTargetFrames.clear();
    m_vPendingFrames.clear();
}

void SeekUtils::UnlockFrame(NvDecFrame& decframe)
{
    uint8_t* dataptr = (uint8_t*)decframe.data;
    if (dataptr != NULL)
    {
        m_pDecoder->UnlockFrame(&dataptr);
    }
}

std::pair<bool, int64_t> SeekUtils::ShouldSeek(int64_t previous_target, int64_t current_target)
{
    // For Open GOP, use IDR frames for GOP boundary detection
    // For Closed GOP, use regular keyframes
    int rightindex, leftindex;
    
    if (IsOpenGOP()) {
        rightindex = GetNearestIDRFrameIndexForTarget(m_pVideoStream, current_target);
        if (previous_target == -1)
        {
            return std::make_pair(true, rightindex);
        }
        leftindex = GetNearestIDRFrameIndexForTarget(m_pVideoStream, previous_target);
    } else {
        rightindex = GetNearestKeyFrameIndexForTarget(m_pVideoStream, current_target);
        if (previous_target == -1)
        {
            return std::make_pair(true, rightindex);
        }
        leftindex = GetNearestKeyFrameIndexForTarget(m_pVideoStream, previous_target);
    }

    if (leftindex == static_cast<int>(SeekStatus::INVALID_INDEX_ENTRY) || rightindex == static_cast<int>(SeekStatus::INVALID_INDEX_ENTRY))
    {
        return std::make_pair(false, static_cast<int>(SeekStatus::INVALID_INDEX_ENTRY));
    }

    if (leftindex == rightindex)
    {
        return std::make_pair(false, rightindex);
    }
    else if (rightindex - previous_target < 4)
    {
        return std::make_pair(false, rightindex);
    }
    else
    {
        return std::make_pair(true, rightindex);
    }
}

int64_t SeekUtils::TsFromTime(double ts_sec)
{
    /* Internal timestamp representation is integer, so multiply to AV_TIME_BASE
     * and switch to fixed point precision arithmetics; */
    auto const ts_tbu = llround(ts_sec * AV_TIME_BASE);

    // Rescale the timestamp to value represented in stream base units;
    AVRational factor;
    factor.num = 1;
    factor.den = AV_TIME_BASE;

    return av_rescale_q(ts_tbu, factor, m_pVideoStream->time_base);
}

void SeekUtils::SeekToIndex(uint32_t newTargetIdx)
{
    m_nPreviousTargetIndex = newTargetIdx;
    m_bSeekToIndexSet = true;
}

/*
 * Batch retrieval
*/
bool SeekUtils::GetFramesByBatch(uint32_t batchsize, std::vector<NvDecFrame>& outputFrames)
{
    outputFrames.clear();
    
    try {
        // Generate indices for the batch
        std::vector<uint32_t> indices;
        indices.reserve(batchsize);
        
        uint32_t start = m_nPreviousTargetIndex + 1;
        uint32_t end = m_nPreviousTargetIndex + batchsize + 1;
        StreamMetadata metadata = m_pDemuxer->GetStreamMetadata();
        uint32_t numFrames = metadata.numFrames;
        
        if (m_bSeekToIndexSet) {
            start = m_nPreviousTargetIndex;
            end = m_nPreviousTargetIndex + batchsize;
            m_bSeekToIndexSet = false;
        }
        
        // Build index list
        for (uint32_t i = start; i < end; i++) {
            if (numFrames == 0 || i < numFrames) {
                indices.push_back(i);
            }
        }
        
        // Use multi-frame version
        return GetFramesByIdxList(indices, outputFrames);
        
    } catch (const std::exception& e) {
        return false;
    } catch (...) {
        return false;
    }
}

bool SeekUtils::IsSeekBackwards(int64_t newTargetIdx)
{
    // Only consider it a backward seek if requesting same or an earlier frame
    if (newTargetIdx <= m_nPreviousTargetIndex && m_nPreviousTargetIndex != -1)
    {
        m_bSeekDirectionBackwards = true;
        return true;
    }
    m_bSeekDirectionBackwards = false;
    return false;
}

bool SeekUtils::IsEOSReached()
{
    return m_bEOSReached;
}

std::vector<NvDecFrame> SeekUtils::GetPendingFrames()
{
    return m_vPendingFrames;
}

/*
 * Filter frame by duplicate PTS and target PTS
*/
bool SeekUtils::ShouldSkipFrame(const NvDecFrame& frame) {
    // For non-seekable streams, PTS is not available, so we don't skip any frames
    if (!m_bSeekable) {
        return false;
    }
    // Filter out frames before target PTS from wrong GOP
    // Now that frames have proper PTS (we pass it to Decode), this filter works correctly
    if (frame.timestamp < m_nTargetFramePTS) {
        return true;  // Skip this frame
    }
    
    return false;  // Don't skip - frame is valid
}

/*
 * Store remaining frames as pending after finding target
*/
void SeekUtils::CacheRemainingFrames(int startIdx, int totalFrames) {
    for (int j = startIdx; j < totalFrames; j++) {
        NvDecFrame pendingFrame = GetFrame(true);
        if (pendingFrame.data != 0) {
            m_uFramesDecodedTillNow++;
            
            // Check if frame was skipped by seekPTS optimization
            if (pendingFrame.isSkipped) {
                UnlockFrame(pendingFrame);
            } else {
                m_vPendingFrames.push_back(pendingFrame);
            }
        }
    }
}

/**
 * @brief Retrieve and process a decoded frame with skipped frame handling
 * @param targetIndex Target frame index to find
 * @param outputFrame Output parameter to store found frame
 * @param skipCheckEnabled If true, checks isSkipped flag and unlocks skipped frames
 * @return FrameProcessStatus indicating result: CONTINUE_DECODE = not target/skip, 
 *         FOUND_TARGET = exact target match, 
 *         STOP_DECODE = passed target
 */
FrameProcessStatus SeekUtils::ProcessDecodedFrame(uint32_t targetIndex, NvDecFrame& outputFrame, bool skipCheckEnabled) {
    // Retrieve frame from decoder
    NvDecFrame frame = GetFrame(true);

    // Handle skipped frames (from seekPTS optimization)
    if (frame.isSkipped && skipCheckEnabled) {
        if (m_uFramesDecodedTillNow == targetIndex) {
            UnlockFrame(frame);
            return FrameProcessStatus::STOP_DECODE;  // Stop decoding - we've passed the target
        }
        m_uFramesDecodedTillNow++;  // Count skipped frames
        UnlockFrame(frame);
        return FrameProcessStatus::CONTINUE_DECODE;  // Skip this frame
    }
    
    // Process frame against target index
    uint32_t currentFrameIndex = m_uFramesDecodedTillNow;
    m_uFramesDecodedTillNow++;  // Increment position counter
    
    // Check for exact match
    if (currentFrameIndex == targetIndex) {
        m_vPreviouslyDecodedFramesPTS.push_back(frame.timestamp);
        outputFrame = frame;
        m_nPreviousTargetIndex = targetIndex;
        return FrameProcessStatus::FOUND_TARGET;  // Found exact target
    }
    
    // Frame is beyond target - store as pending and signal to stop decode loop
    if (currentFrameIndex > targetIndex) {
        m_vPendingFrames.push_back(frame);
        return FrameProcessStatus::STOP_DECODE;  // Stop decoding - we've passed the target
    }
    
    // Frame is before target - unlock it
    UnlockFrame(frame);
    return FrameProcessStatus::CONTINUE_DECODE;  // Not target, continue
}

/**
 * @brief Search pending frames queue for target
 * @param targetIndex Target frame index to find
 * @param outputFrame Output parameter to store found frame
 * @param currentPosOffset Offset to calculate pending frame indices
 * @return true if found, false otherwise
 */
bool SeekUtils::SearchPendingFrames(uint32_t targetIndex, NvDecFrame& outputFrame, uint32_t currentPosOffset) {
    if (m_vPendingFrames.empty()) {
        return false;
    }
    
    
    // Calculate what frame index the first pending frame represents
    uint32_t pendingStartIndex = currentPosOffset;
    
    for (uint32_t i = 0; i < m_vPendingFrames.size(); i++) {
        uint32_t pendingFrameIndex = pendingStartIndex + i;
        
        if (pendingFrameIndex == targetIndex) {
            // Found target in pending queue!
            outputFrame = m_vPendingFrames[i];
            
            // Unlock all frames before the target (we skipped over them)
            for (size_t j = 0; j < i; j++) {
                UnlockFrame(m_vPendingFrames[j]);
            }
            
            // Remove target frame and all earlier frames from pending queue
            m_vPendingFrames.erase(m_vPendingFrames.begin(), m_vPendingFrames.begin() + i + 1);
            
            return true;
        }
    }
    
    return false;
}

/**
 * @brief Flush decoder and process frames to find target
 * @param targetIndex Target frame index to find
 * @param outputFrame Output parameter to store found frame
 * @param context Debug context string
 * @param useProcessDecodedFrame If true, uses ProcessDecodedFrame (GOP-aware); if false, stores all frames in pending (sequential)
 * @return FrameProcessStatus indicating result
 */
FrameProcessStatus SeekUtils::FlushAndProcessFrames(uint32_t targetIndex, NvDecFrame& outputFrame, const char* context, bool useProcessDecodedFrame) {
    
    m_bEOSReached = true;  // Mark that we've reached EOS
    
    // Use DISCONTINUITY flag with empty packet to properly flush DPB
    // This triggers flush_decoded_picture_buffer() in the parser, which flushes all frames from DPB
    int numFrames = m_pDecoder->Decode(nullptr, 0, CUVID_PKT_DISCONTINUITY, 0);
    
    // Process all flushed frames
    for (int i = 0; i < numFrames; i++) {
        FrameProcessStatus result;
        
        if (useProcessDecodedFrame) {
            // GOP-aware mode: use merged ProcessDecodedFrame helper
            result = ProcessDecodedFrame(targetIndex, outputFrame, true);
        } else {
            // Sequential mode: store ALL flushed frames in pending, check if target found
            NvDecFrame frame = GetFrame(true);
            
            // Handle skipped frames (from seekPTS optimization)
            if (frame.isSkipped) {
                m_uFramesDecodedTillNow++;  // Count skipped frames
                UnlockFrame(frame);
                result = FrameProcessStatus::CONTINUE_DECODE;
            } else {
                uint32_t currentFrameIndex = m_uFramesDecodedTillNow;
                m_uFramesDecodedTillNow++;  // Increment position counter
                
                // Apply frame filtering (duplicate PTS + target PTS check) - consistent with GOP-aware mode
                if (ShouldSkipFrame(frame)) {
                    UnlockFrame(frame);
                    m_uFramesDecodedTillNow--;  // Don't count filtered frames
                    result = FrameProcessStatus::CONTINUE_DECODE;
                } else if (currentFrameIndex == targetIndex) {
                    // Found target!
                    outputFrame = frame;
                    CacheRemainingFrames(i + 1, numFrames);
                    return FrameProcessStatus::FOUND_TARGET;
                } else {
                    // Store in pending for later retrieval
                    m_vPendingFrames.push_back(frame);
                    result = FrameProcessStatus::CONTINUE_DECODE;
                }
            }
        }
        
        if (result == FrameProcessStatus::FOUND_TARGET) {
            // Store remaining flushed frames as pending
            CacheRemainingFrames(i + 1, numFrames);
            return result;
        }
        else if (result == FrameProcessStatus::STOP_DECODE) {
            // Passed target without finding it - store remaining and stop
            CacheRemainingFrames(i + 1, numFrames);
            return result;
        }
    }
    
    // Check if target is in pending after flushing (applies to both modes)
    // GOP-aware mode: frames beyond target may have been stored in pending before flush
    // Sequential mode: all non-target flushed frames are stored in pending
    if (!m_vPendingFrames.empty()) {
        uint32_t pendingStartIndex = m_uFramesDecodedTillNow - static_cast<uint32_t>(m_vPendingFrames.size());
        
        if (useProcessDecodedFrame) {
            // GOP-aware mode: use SearchPendingFrames helper for consistency
            if (SearchPendingFrames(targetIndex, outputFrame, pendingStartIndex)) {
                return FrameProcessStatus::FOUND_TARGET;
            }
        } else {
            // Sequential mode: simple index matching
            for (size_t i = 0; i < m_vPendingFrames.size(); i++) {
                if (pendingStartIndex + static_cast<uint32_t>(i) == targetIndex) {
                    outputFrame = m_vPendingFrames[i];
                    m_vPendingFrames.erase(m_vPendingFrames.begin(), m_vPendingFrames.begin() + i + 1);
                    return FrameProcessStatus::FOUND_TARGET;
                }
            }
        }
    }
    
    return FrameProcessStatus::CONTINUE_DECODE;
}

/**
 * @brief Main decode loop - demux packets and process frames until target found
 * @param targetIndex Target frame index to find  
 * @param outputFrame Output parameter to store found frame
 * @param context Debug context string
 * @param useProcessDecodedFrame If true, uses ProcessDecodedFrame helper; if false, uses simple index matching
 * @return true if target found, false otherwise
 */
bool SeekUtils::DecodeUntilTarget(uint32_t targetIndex, NvDecFrame& outputFrame, const char* context, bool useProcessDecodedFrame) {
    int attempt = 0;
    static constexpr int MAX_DECODE_ATTEMPTS = 1000000;
    
    while (attempt < MAX_DECODE_ATTEMPTS) {
        attempt++;
        
        uint8_t* data = nullptr;
        int size = 0;
        int64_t pts = 0;
        
        if (m_pDemuxer->DemuxVideo(&data, &size, &pts)) {
            // Set target PTS on first keyframe after seek (for GOP-aware mode)
            if (m_bDiscontinuityFlag && m_pDemuxer->IsLastPacketKeyframe()) {
                m_nTargetFramePTS = pts;
                m_bDiscontinuityFlag = false;
            }
            int numFrames = m_pDecoder->Decode(data, size, CUVID_PKT_TIMESTAMP, pts);
            
            if (numFrames > 0) {
                // Process ALL decoded frames
                for (int i = 0; i < numFrames; i++) {
                    FrameProcessStatus result;
                    
                    if (useProcessDecodedFrame) {
                        // GOP-aware mode: use merged ProcessDecodedFrame helper
                        result = ProcessDecodedFrame(targetIndex, outputFrame, true);
                    } else {
                        // Simple sequential matching
                        NvDecFrame frame = GetFrame(true);
                        
                        // Handle skipped frames (from seekPTS optimization)
                        if (frame.isSkipped) {
                            m_uFramesDecodedTillNow++;  // Count skipped frames
                            UnlockFrame(frame);
                            result = FrameProcessStatus::CONTINUE_DECODE;
                        } else {
                            uint32_t currentFrameIndex = m_uFramesDecodedTillNow;
                            m_uFramesDecodedTillNow++;  // Increment position counter
                            
                            // Apply frame filtering (duplicate PTS + target PTS check) - consistent with GOP-aware mode
                            if (ShouldSkipFrame(frame)) {
                                UnlockFrame(frame);
                                m_uFramesDecodedTillNow--;  // Don't count filtered frames
                                result = FrameProcessStatus::CONTINUE_DECODE;
                            } else if (currentFrameIndex == targetIndex) {
                                outputFrame = frame;
                                CacheRemainingFrames(i + 1, numFrames);
                                return true;
                            } else if (currentFrameIndex < targetIndex) {
                                UnlockFrame(frame);
                                result = FrameProcessStatus::CONTINUE_DECODE;
                            } else {
                                m_vPendingFrames.push_back(frame);
                                result = FrameProcessStatus::STOP_DECODE;
                            }
                        }
                    }
                    
                    if (result == FrameProcessStatus::FOUND_TARGET) {
                        CacheRemainingFrames(i + 1, numFrames);
                        return true;
                    }
                    else if (result == FrameProcessStatus::STOP_DECODE) {
                        CacheRemainingFrames(i + 1, numFrames);
                        return false;  // Passed target
                    }
                }
            }
        } else {
            // EOS - try flushing
            FrameProcessStatus flushResult = FlushAndProcessFrames(targetIndex, outputFrame, context, useProcessDecodedFrame);
            if (flushResult == FrameProcessStatus::FOUND_TARGET) {
                return true;
            }
            return false;  // EOS reached, target not found
        }
    }
    
    LOG(WARNING) << "DecodeUntilTarget: MAX_DECODE_ATTEMPTS (" << MAX_DECODE_ATTEMPTS << ") reached for target " << targetIndex << " (" << context << ")";
    return false;  // Safety limit reached, target not found
}

/*
 * Sequential Mode Implementation (for non-seekable streams)
 */

bool SeekUtils::GetFrameSequential(uint32_t targetIndex, NvDecFrame& outputFrame) {
    memset(&outputFrame, 0, sizeof(NvDecFrame));

    uint32_t pendingStartIndex = m_uFramesDecodedTillNow - static_cast<uint32_t>(m_vPendingFrames.size());
    // Step 1: Check pending frames first
    if (SearchPendingFrames(targetIndex, outputFrame, pendingStartIndex)) {
        m_nPreviousTargetIndex = targetIndex;
        return true;
    }

    // Handle pending frames not containing target
    if (!m_vPendingFrames.empty()) {
        uint32_t pendingStartIndex = m_uFramesDecodedTillNow - static_cast<uint32_t>(m_vPendingFrames.size());
        
        if (targetIndex < pendingStartIndex) {
            // Clear pending and restart
            for (NvDecFrame frame : m_vPendingFrames) {
                UnlockFrame(frame);
            }
            m_vPendingFrames.clear();
        } else {
            // Skip past pending - update position to reflect frames we're skipping
            for (NvDecFrame frame : m_vPendingFrames) {
                UnlockFrame(frame);
            }
            m_uFramesDecodedTillNow = pendingStartIndex + static_cast<uint32_t>(m_vPendingFrames.size());
            m_vPendingFrames.clear();
        }
    }

    // Step 2: Validate forward seeking
    if (targetIndex < m_uFramesDecodedTillNow) {
        std::string errorMsg = "Cannot seek backward in non-seekable stream without reconfiguration. "
                               "Requested frame " + std::to_string(targetIndex) + 
                               " but current position is " + std::to_string(m_uFramesDecodedTillNow);
        throw std::runtime_error(errorMsg);
    }

    // Step 3: Decode forward using helper
    
    bool found = DecodeUntilTarget(targetIndex, outputFrame, "Sequential", false);
    
    if (found) {
        m_nPreviousTargetIndex = targetIndex;
    } else {
    }
    
    return found;
}

/*
 * GOP-aware implementation (for seekable streams)
 */
bool SeekUtils::GetFrameGOPAware(uint32_t targetIndex, NvDecFrame& outputFrame)
{
    memset(&outputFrame, 0, sizeof(NvDecFrame));
    
    try {
        // Check if we need to seek based on GOP boundaries (same logic as original)
        std::pair<bool, int64_t> result = ShouldSeek(m_nPreviousTargetIndex, targetIndex);
        
        if (result.second == static_cast<int>(SeekStatus::INVALID_INDEX_ENTRY)) {
            return false;
        }
        
        bool needsSeek = result.first;

        // Calculate seekPTS threshold: use PTS of previous frame to get exact boundary
        // The comparison is "timestamp < seekPTS", so frames with PTS < threshold are skipped
        // By using the previous frame's PTS, we ensure only frames strictly before target are skipped
        
        if (m_bEnablePTSFrameSkip && targetIndex > 0) {
            uint64_t seekThreshold = 0;
            // Get PTS of frame (targetIndex - 1) - this is the most robust approach
            int64_t prevFramePTS = m_pDemuxer->FrameToPts(m_pVideoStream, targetIndex - 1);
            if (prevFramePTS > 0 && m_pDemuxer->GetPTSOffset() > 0) {
                // Don't consider the PTS offset for Seek PTS optimization
                prevFramePTS -= m_pDemuxer->GetPTSOffset();
            }
            seekThreshold = (prevFramePTS > 0) ? (uint64_t)prevFramePTS : 0;
            m_pDecoder->setSeekPTS(seekThreshold);
        }
        
        if (needsSeek) {
            
            // ShouldSeek() already determined the correct keyframe:
            // - For Open GOP: nearest IDR frame (to ensure all reference frames available)
            // - For Closed GOP: nearest regular keyframe
            int64_t keyframeIndex = result.second;
            std::cout << "Starting from Key frame idx: " << keyframeIndex << std::endl;
            
            // Seek to keyframe and start decoding
            m_pDemuxer->Seek(static_cast<uint32_t>(keyframeIndex));
            m_uFramesDecodedTillNow = static_cast<uint32_t>(keyframeIndex);

            // Reset target PTS - will be set on first keyframe
            m_nTargetFramePTS = 0;
            m_bDiscontinuityFlag = true;  // Mark that we need to set target PTS on first keyframe
            
            // CRITICAL: Flush decoder with DISCONTINUITY flag when GOP changes
            // This clears any stale frames from the previous GOP
            PacketData emptyPacket = PacketData();
            int numFlushedFrames = m_pDecoder->Decode((uint8_t*)emptyPacket.bsl_data,
                (int)emptyPacket.bsl, CUVID_PKT_DISCONTINUITY, emptyPacket.pts);
            
            // Unlock and discard the flushed frames (they're from the old GOP)
            for (int i = 0; i < numFlushedFrames; i++) {
                NvDecFrame staleFrame = GetFrame(false);
            }
            
            // Clear previously decoded frames cache since we're in a new GOP
            m_vPreviouslyDecodedFramesPTS.clear();
            for (NvDecFrame frame : m_vPendingFrames)
            {
                UnlockFrame(frame);
            }
            m_vPendingFrames.clear();  // Also clear pending frames from old GOP
        } else {
            // We're in the same GOP, so we can potentially reuse decoded frames
            // No need to seek or clear cache
        }
        
        // Now decode frames until we reach our target
        
        if (needsSeek) {
            // For new GOP, decode until we find the frame or reach EOS
            bool found = DecodeUntilTarget(targetIndex, outputFrame, "GOP-change", true);
            if (found) {
                if (m_bEnablePTSFrameSkip) {
                    m_pDecoder->setSeekPTS(0);
                }
                return true;
            }
            // Not found - fall through to return false at end
        } else {
            // Same GOP - we need to continue decoding from current position until we reach target

            if (targetIndex == static_cast<uint32_t>(m_nPreviousTargetIndex) && 
                targetIndex == m_uFramesDecodedTillNow - 1) {
                // The frame was already returned and is locked by the caller
                return false;
            }
            
            // Check pending frames queue first
            uint32_t pendingOffset = m_uFramesDecodedTillNow - static_cast<uint32_t>(m_vPendingFrames.size());
            if (SearchPendingFrames(targetIndex, outputFrame, pendingOffset)) {
                m_nPreviousTargetIndex = targetIndex;
                if (m_bEnablePTSFrameSkip) {
                    m_pDecoder->setSeekPTS(0);
                }
                return true;
            }
            
            // Clear any remaining pending frames
            if (!m_vPendingFrames.empty()) {
                for (NvDecFrame frame : m_vPendingFrames) {
                    UnlockFrame(frame);
                }
                m_vPendingFrames.clear();
            }
            
            // Calculate how many more frames we need to decode to reach target
            uint32_t framesToDecode = (targetIndex > m_uFramesDecodedTillNow) ? (targetIndex - m_uFramesDecodedTillNow + 1) : 0;
            
            // If target is behind current position in same GOP, we can't go backwards efficiently
            if (targetIndex < m_uFramesDecodedTillNow) {
                
                // Force a seek even though it's same GOP
                m_pDemuxer->Seek(targetIndex);
                m_uFramesDecodedTillNow = targetIndex;
                m_vPreviouslyDecodedFramesPTS.clear();
                
                bool found = DecodeUntilTarget(targetIndex, outputFrame, "Forced-seek", true);
                if (found) {
                    if (m_bEnablePTSFrameSkip) {
                        m_pDecoder->setSeekPTS(0);
                    }
                    return true;
                }
            } else {
                // Target is ahead - decode forward using helper
                bool found = DecodeUntilTarget(targetIndex, outputFrame, "Forward", true);
                if (found) {
                    if (m_bEnablePTSFrameSkip) {
                        m_pDecoder->setSeekPTS(0);
                    }
                    return true;
                }
            }
        }
        
        return false;
        
    } catch (const std::exception& e) {
        if (m_bEnablePTSFrameSkip) {
            m_pDecoder->setSeekPTS(0);
        }
        return false;
    } catch (...) {
        if (m_bEnablePTSFrameSkip) {
            m_pDecoder->setSeekPTS(0);
        }
        return false;
    }
}

/*
 * Single frame retrieval - Calls multi-frame version to ensure proper unlock/track
 * Always goes through multi-frame path for proper frame lifecycle management
*/
bool SeekUtils::GetFramesByIdx(uint32_t targetIndex, NvDecFrame& outputFrame)
{
    std::vector<uint32_t> indices = {targetIndex};
    std::vector<NvDecFrame> frames;
    
    bool success = GetFramesByIdxList(indices, frames);
    
    if (success && !frames.empty()) {
        outputFrame = frames[0];
        return true;
    }
    
    return false;
}

bool SeekUtils::IsScanRequired() {
    // Check if stream scanning is required
    // For webm/flv/mov containers, scanning is needed if duration or numFrames is 0
    std::string container = m_pDemuxer->GetContainerName();
    if (container == "matroska,webm" || container == "mov" || container == "flv") {
        StreamMetadata metadata = m_pDemuxer->GetStreamMetadata();
        // Scan if duration or numFrames is 0 (missing metadata)
        return (metadata.duration == 0.0 || metadata.numFrames == 0);
    }
    return false;
}

