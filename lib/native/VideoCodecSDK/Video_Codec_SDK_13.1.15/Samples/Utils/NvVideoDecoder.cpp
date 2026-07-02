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

#include "NvVideoDecoder.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>


NvVideoDecoder::NvVideoDecoder(const std::string& encSource,
                             CUcontext cudaContext,
                             CUstream cudaStream,
                             bool useDeviceMemory,
                             uint32_t maxWidth,
                             uint32_t maxHeight,
                             bool needScannedStreamMetadata,
                             uint32_t decoderCacheSize,
                             bool enableDecodeStats,
                             bool enableOpenGOP)
    : m_strEncSource(encSource)
    , m_cuContext(cudaContext)
    , m_cuStream(cudaStream)
    , m_bUseDeviceMemory(useDeviceMemory)
    , m_uMaxWidth(maxWidth)
    , m_uMaxHeight(maxHeight)
    , m_bEnableDecodeStats(enableDecodeStats)
    , m_bNeedScannedMetadata(needScannedStreamMetadata)
    , m_bEnableOpenGOP(enableOpenGOP)
    , m_bEnablePTSFrameSkip(true)
    , m_bMetadataCached(false)
    , m_decoderCache(decoderCacheSize)
{
    // Check if CUDA context is provided (mandatory)
    if (!m_cuContext) {
        throw std::runtime_error("NvVideoDecoder: CUDA context cannot be null");
    }
    
    try {
        Initialize(needScannedStreamMetadata, enableOpenGOP);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("NvVideoDecoder constructor failed: ") + e.what());
    }
}

NvVideoDecoder::~NvVideoDecoder() {
    try {
        // Clear seek state and unlock frames
        if (m_pSeekUtils) {
            m_pSeekUtils->ClearState(true);
        }
        
        // Reset seek utils first
        m_pSeekUtils.reset();
        
        // Release current decoder from unique_ptr ownership only if it's in the cache
        // If cache size is 0, the decoder was never added to cache and must be deleted by unique_ptr
        if (m_decoderCache.Size() > 0) {
            m_pDecoder.release();
        }
        
        // Clean up all cached decoders
        NvDecoder* decoder = nullptr;
        while ((decoder = m_decoderCache.RemoveElement()) != nullptr) {
            delete decoder;
        }
        
        // Reset demuxer (this also deletes m_pDecoder if cache was empty)
        m_pDemuxer.reset();
    } catch (...) {
        // Suppress exceptions in destructor
    }
}

void NvVideoDecoder::Initialize(bool needScannedStreamMetadata, bool enableOpenGOP) {
    // Create demuxer
    m_pDemuxer.reset(new FFmpegDemuxer(m_strEncSource.c_str()));
    
    if (!m_pDemuxer || !m_pDemuxer->GetVideoStream()) {
        throw std::runtime_error("Failed to create demuxer or video stream is invalid");
    }
    // Note: We support both seekable and non-seekable streams
    // Seekable streams use GOP-aware mode, non-seekable use sequential mode
    
    // Get video codec
    cudaVideoCodec codec = FFmpeg2NvCodecId(m_pDemuxer->GetVideoCodec());
    
    // Get codec properties from demuxer (more efficient than creating decoder first)
    int bitDepth = m_pDemuxer->GetBitDepth();
    cudaVideoChromaFormat chromaFormat = AVPixelFormat2NvChromaFormat(m_pDemuxer->GetChromaFormat());
    
    // Create decoder
    m_pDecoder.reset(new NvDecoder(
        m_cuContext,
        m_bUseDeviceMemory,   // bUseDeviceFrame
        codec,
        false,              // bLowLatency
        false,              // bDeviceFramePitched
        nullptr,            // cropRect
        nullptr,            // resizeDim
        false,              // extract_user_SEI_Message
        m_uMaxWidth,
        m_uMaxHeight,
        1000,               // clkRate
        false,              // force_zero_latency
        0,                  // initial_dec_surfaces (auto-calculate)
        m_bEnableDecodeStats,
        m_cuStream
    ));
    
    // Add decoder to cache (stores raw pointer, see DecoderCache.h for ownership model)
    CacheKey key = std::make_tuple(bitDepth, codec, chromaFormat);
    m_decoderCache.PushDecoder(key, m_pDecoder.get());
    
    // Create seek utils
    m_pSeekUtils.reset(new SeekUtils(m_pDemuxer.get(), m_pDecoder.get()));
    
    // Enable open GOP support BEFORE scanning (so IDR indices are collected)
    if (enableOpenGOP) {
        m_pSeekUtils->EnableOpenGOPSupport(true);
    }
    
    // Set PTS-based frame skipping state
    m_pSeekUtils->SetPTSFrameSkip(m_bEnablePTSFrameSkip);
    
    // Determine if scanning is required
    // Use explicit flag if provided, otherwise auto-detect
    bool bScanRequired = needScannedStreamMetadata || m_pSeekUtils->IsScanRequired();
    
    // Remember if we need scanning (for later reconfiguration after backward seeks)
    if (bScanRequired) {
        m_pSeekUtils->SetNeedScannedMetadata(true);  // Propagate to SeekUtils for ReconfigureDecoder
    }
    
    // If scanned metadata needed, scan for keyframes
    if (bScanRequired) {
        ScanAndSetKeyFrameIndices();
    }
}

NvDecFrame NvVideoDecoder::operator[](uint32_t index) {
    ValidateFrameIndex(index);
    ResetDecoderIfRequired(index);
    
    NvDecFrame frame;
    bool success = m_pSeekUtils->GetFramesByIdx(index, frame);
    
    if (!success || frame.data == 0) {
        throw std::runtime_error("Failed to retrieve frame at index " + std::to_string(index));
    }
    
    return frame;
}

std::vector<NvDecFrame> NvVideoDecoder::operator[](const std::vector<uint32_t>& indices) {
    if (indices.empty()) {
        return {};
    }
    
    // Limit number of indices to prevent out of memory issues
    if (indices.size() > 64) {
        throw std::runtime_error("Too many frame indices requested (" + std::to_string(indices.size()) + 
                                "). Maximum allowed is 64 to prevent out of memory issues.");
    }
    
    // Optimize: Get metadata once instead of per-index
    StreamMetadata metadata = GetStreamMetadata();
    
    // Validate all indices and find min in single pass
    uint32_t minIndex = UINT32_MAX;
    for (uint32_t idx : indices) {
        if (metadata.numFrames > 0 && idx >= metadata.numFrames) {
            throw std::out_of_range("Frame index " + std::to_string(idx) + 
                                   " is out of range [0, " + std::to_string(metadata.numFrames - 1) + "]");
        }
        if (idx < minIndex) {
            minIndex = idx;
        }
    }
    
    ResetDecoderIfRequired(minIndex);
    
    // Use multi-frame version for optimal retrieval
    std::vector<NvDecFrame> frames;
    bool success = m_pSeekUtils->GetFramesByIdxList(indices, frames);
    
    if (!success || frames.empty()) {
        throw std::runtime_error("Failed to retrieve any frames from the requested indices");
    }
    
    return frames;
}

std::vector<NvDecFrame> NvVideoDecoder::GetBatchFrames(uint32_t batchSize) {
    if (batchSize == 0) {
        throw std::invalid_argument("Batch size must be greater than 0");
    }
    
    std::vector<NvDecFrame> frames;
    bool success = m_pSeekUtils->GetFramesByBatch(batchSize, frames);
    
    if (!success && frames.empty()) {
        throw std::runtime_error("Failed to retrieve batch of frames");
    }
    
    return frames;
}


void NvVideoDecoder::SeekToIndex(uint32_t index) {
    ValidateFrameIndex(index);
    ResetDecoderIfRequired(index);
    m_pSeekUtils->SeekToIndex(index);
}

uint32_t NvVideoDecoder::GetIndexFromTimeInSeconds(float timeInSeconds) {
    if (timeInSeconds < 0) {
        throw std::invalid_argument("Time in seconds cannot be negative");
    }
    
    return m_pSeekUtils->GetIndexFromTimeInSeconds(timeInSeconds);
}

StreamMetadata NvVideoDecoder::GetStreamMetadata() {
    // Return cached metadata if available
    if (m_bMetadataCached) {
        return m_cachedMetadata;
    }
    
    // Fetch and cache metadata
    m_cachedMetadata = m_pDemuxer->GetStreamMetadata();
    m_bMetadataCached = true;
    return m_cachedMetadata;
}

ScannedStreamMetadata NvVideoDecoder::GetScannedStreamMetadata() {
    if (!m_pDemuxer) {
        throw std::runtime_error("Demuxer not initialized");
    }
    // Scan for IDR frames if Open GOP support is enabled
    bool bScanForIDR = m_pSeekUtils && m_pSeekUtils->IsOpenGOPSupportEnabled();
    return m_pDemuxer->GetScannedStreamMetadata(bScanForIDR);
}

ReconfigureStatus NvVideoDecoder::ReconfigureDecoder(const std::string& newSource, bool isSameSource, uint32_t maxWidth, uint32_t maxHeight) {
    try {
        // Fast path: If same source and no dimension changes, just reset state
        if (isSameSource && maxWidth == 0 && maxHeight == 0) {
            // Clear cached metadata (will be refreshed on next access)
            m_bMetadataCached = false;
            
            // Reset SeekUtils state (clears decoded frames, resets position)
            if (m_pSeekUtils) {
                m_pSeekUtils->ClearState(true);
            }
            
            if (m_pDemuxer) {
                if (m_pDemuxer->IsSeekable()) {
                    // Seekable streams: use Seek() to reset position
                    m_pDemuxer->Seek(0);
                } else {
                    // Non-seekable streams: try avio_seek to reset to beginning, else fall back to recreating demuxer
                    bool bResetAvio = m_pDemuxer->TryResetViaAvioSeek();
                    if (!bResetAvio) {
                        std::unique_ptr<FFmpegDemuxer> newDemuxer(new FFmpegDemuxer(m_strEncSource.c_str()));
                        if (!newDemuxer || !newDemuxer->GetVideoStream()) {
                            throw std::runtime_error("Failed to recreate demuxer for reset");
                        }
                        if (m_pSeekUtils) {
                            m_pSeekUtils->UpdateDemuxer(newDemuxer.get());
                        }
                        m_pDemuxer = std::move(newDemuxer);
                    }
                }
            }
            
            // Keyframe indices are preserved (no re-scanning needed)
            return ReconfigureStatus::ResetState;
        }
        
        // Store old source in case we need to rollback
        std::string oldSource = m_strEncSource;
        uint32_t oldMaxWidth = m_uMaxWidth;
        uint32_t oldMaxHeight = m_uMaxHeight;
        
        // Save keyframe indices to avoid expensive re-scanning if same source
        std::vector<uint32_t> savedKeyFrameIndices;
        std::vector<uint32_t> savedIDRFrameIndices;
        bool hadKeyFrameIndices = false;
        
        if (m_pSeekUtils && isSameSource) {
            savedKeyFrameIndices = m_pSeekUtils->GetKeyFrameIndices();
            savedIDRFrameIndices = m_pSeekUtils->GetIDRFrameIndices();
            hadKeyFrameIndices = !savedKeyFrameIndices.empty();
        }
        
        // Update max dimensions if provided (0 = keep current setting)
        if (maxWidth > 0) {
            m_uMaxWidth = maxWidth;
        }
        if (maxHeight > 0) {
            m_uMaxHeight = maxHeight;
        }
        
        // Clear cached metadata since we're switching sources
        m_bMetadataCached = false;
        
        // Update source
        m_strEncSource = newSource;
        
        // Clear seek state
        if (m_pSeekUtils) {
            m_pSeekUtils->ClearState(true);
        }
        
        // Create new demuxer for the new source
        std::unique_ptr<FFmpegDemuxer> oldDemuxer;
        try {
            std::unique_ptr<FFmpegDemuxer> newDemuxer(new FFmpegDemuxer(newSource.c_str()));
            
            if (!newDemuxer || !newDemuxer->GetVideoStream()) {
                throw std::runtime_error("Demuxer created but video stream is invalid");
            }
            
            oldDemuxer = std::move(m_pDemuxer);
            m_pDemuxer = std::move(newDemuxer);
            
        } catch (const std::exception& e) {
            // Rollback all changes on failure
            if (oldDemuxer) {
                m_pDemuxer = std::move(oldDemuxer);
            }
            m_strEncSource = oldSource;
            m_uMaxWidth = oldMaxWidth;
            m_uMaxHeight = oldMaxHeight;
            throw std::runtime_error(std::string("Failed to create new demuxer: ") + e.what());
        }
        
        // Get codec properties for the new source from demuxer
        cudaVideoCodec codec = FFmpeg2NvCodecId(m_pDemuxer->GetVideoCodec());
        int width = m_pDemuxer->GetWidth();
        int height = m_pDemuxer->GetHeight();
        int bitDepth = m_pDemuxer->GetBitDepth();
        cudaVideoChromaFormat chromaFormat = AVPixelFormat2NvChromaFormat(m_pDemuxer->GetChromaFormat());
        
        // Calculate required decoder dimensions (considering both video and user-specified max)
        uint32_t requiredWidth = (uint32_t)width;
        uint32_t requiredHeight = (uint32_t)height;
        if (m_uMaxWidth > 0) {
            requiredWidth = std::max<uint32_t>(requiredWidth, m_uMaxWidth);
        }
        if (m_uMaxHeight > 0) {
            requiredHeight = std::max<uint32_t>(requiredHeight, m_uMaxHeight);
        }
        
        // Build cache key (bitDepth, codec, chromaFormat)
        CacheKey key = std::make_tuple(bitDepth, codec, chromaFormat);
        
        // Check cache for matching decoder
        NvDecoder* cachedDecoder = m_decoderCache.GetDecoder(key);
        
        if (cachedDecoder != nullptr) {
            // Found matching decoder in cache - check if it can handle required dimensions
            if (requiredWidth > (uint32_t)cachedDecoder->GetMaxWidth() || requiredHeight > (uint32_t)cachedDecoder->GetMaxHeight()) {
                // Cached decoder is too small, create new one with larger dimensions
                
                // Take the maximum of required dimensions and cached decoder dimensions
                uint32_t newMaxWidth = std::max<uint32_t>(requiredWidth, (uint32_t)cachedDecoder->GetMaxWidth());
                uint32_t newMaxHeight = std::max<uint32_t>(requiredHeight, (uint32_t)cachedDecoder->GetMaxHeight());
                
                // Create new decoder with larger dimensions
                NvDecoder* newDecoder = new NvDecoder(
                    m_cuContext,
                    m_bUseDeviceMemory,
                    codec,
                    false,              // bLowLatency
                    false,              // bDeviceFramePitched
                    nullptr,            // cropRect
                    nullptr,            // resizeDim
                    false,              // extract_user_SEI_Message
                    newMaxWidth,
                    newMaxHeight,
                    1000,               // clkRate
                    false,              // force_zero_latency
                    0,                  // initial_dec_surfaces
                    m_bEnableDecodeStats,
                    m_cuStream
                );
                
                // Transfer ownership: old decoder -> cache, new decoder -> unique_ptr
                NvDecoder* oldDecoder = m_pDecoder.release();
                m_pDecoder.reset(newDecoder);
                
                // Add to cache (may evict LRU)
                NvDecoder* evictedDecoder = m_decoderCache.PushDecoder(key, m_pDecoder.get());
                if (evictedDecoder) {
                    // Cache was full, evicted decoder needs to be deleted
                    HandleEvictedDecoder(evictedDecoder);
                }
                // If cache is disabled (capacity == 0), PushDecoder returns nullptr and doesn't store anything
                // In that case, old decoder was never transferred to cache and must be deleted
                if (oldDecoder && evictedDecoder == nullptr && m_decoderCache.GetDecoder(key) == nullptr) {
                    // Cache is disabled, delete old decoder explicitly
                    delete oldDecoder;
                }
                
                // Reinitialize SeekUtils with new demuxer and decoder
                m_pSeekUtils.reset(new SeekUtils(m_pDemuxer.get(), m_pDecoder.get()));
                
                // Restore OpenGOP state
                if (m_bEnableOpenGOP) {
                    m_pSeekUtils->EnableOpenGOPSupport(true);
                }
                
                // Restore PTS-based frame skipping state
                m_pSeekUtils->SetPTSFrameSkip(m_bEnablePTSFrameSkip);
                
                // Restore keyframe indices if same source (avoids expensive re-scanning)
                if (hadKeyFrameIndices && isSameSource) {
                    m_pSeekUtils->SetKeyFrameIndices(savedKeyFrameIndices);
                    if (m_bEnableOpenGOP && !savedIDRFrameIndices.empty()) {
                        m_pSeekUtils->SetIDRFrameIndices(savedIDRFrameIndices);
                    }
                } else if (!isSameSource) {
                    // New source - check if scanning is needed for this specific video
                    // Use explicit flag if set, otherwise auto-detect for this video
                    bool bScanRequired = m_bNeedScannedMetadata || m_pSeekUtils->IsScanRequired();
                    if (bScanRequired) {
                        m_pSeekUtils->SetNeedScannedMetadata(true);  // Propagate to SeekUtils for consistency
                        ScanAndSetKeyFrameIndices();
                    }
                }
                
                return ReconfigureStatus::CacheMiss;
            } else {
                // Reuse cached decoder
                // Swap: current decoder -> cache, cached decoder -> unique_ptr
                m_pDecoder.release();
                m_pDecoder.reset(cachedDecoder);
                
                // Set reconfiguration parameters (no crop, resize to current dimensions)
                Dim dim;
                dim.w = (unsigned int)width;
                dim.h = (unsigned int)height;
                m_pDecoder->setReconfigParams(nullptr, &dim);
                
                // Reinitialize SeekUtils with new demuxer and decoder
                m_pSeekUtils.reset(new SeekUtils(m_pDemuxer.get(), m_pDecoder.get()));
                
                // Restore OpenGOP state
                if (m_bEnableOpenGOP) {
                    m_pSeekUtils->EnableOpenGOPSupport(true);
                }
                
                // Restore PTS-based frame skipping state
                m_pSeekUtils->SetPTSFrameSkip(m_bEnablePTSFrameSkip);
                
                // Restore keyframe indices if same source (avoids expensive re-scanning)
                if (hadKeyFrameIndices && isSameSource) {
                    m_pSeekUtils->SetKeyFrameIndices(savedKeyFrameIndices);
                    if (m_bEnableOpenGOP && !savedIDRFrameIndices.empty()) {
                        m_pSeekUtils->SetIDRFrameIndices(savedIDRFrameIndices);
                    }
                } else if (!isSameSource) {
                    // New source - check if scanning is needed for this specific video
                    // Use explicit flag if set, otherwise auto-detect for this video
                    bool bScanRequired = m_bNeedScannedMetadata || m_pSeekUtils->IsScanRequired();
                    if (bScanRequired) {
                        m_pSeekUtils->SetNeedScannedMetadata(true);  // Propagate to SeekUtils for consistency
                        ScanAndSetKeyFrameIndices();
                    }
                }
                
                return ReconfigureStatus::CacheHit;
            }
        } else {
            // No matching decoder in cache, create new one
            NvDecoder* newDecoder = new NvDecoder(
                m_cuContext,
                m_bUseDeviceMemory,
                codec,
                false,              // bLowLatency
                false,              // bDeviceFramePitched
                nullptr,            // cropRect
                nullptr,            // resizeDim
                false,              // extract_user_SEI_Message
                requiredWidth,
                requiredHeight,
                1000,               // clkRate
                false,              // force_zero_latency
                0,                  // initial_dec_surfaces
                m_bEnableDecodeStats,
                m_cuStream
            );
            
            // Transfer ownership: old decoder -> cache, new decoder -> unique_ptr
            NvDecoder* oldDecoder = m_pDecoder.release();
            m_pDecoder.reset(newDecoder);
            
            // Add to cache (may evict LRU)
            NvDecoder* evictedDecoder = m_decoderCache.PushDecoder(key, m_pDecoder.get());
            if (evictedDecoder) {
                // Cache was full, evicted decoder needs to be deleted
                HandleEvictedDecoder(evictedDecoder);
            }
            // If cache is disabled (capacity == 0), PushDecoder returns nullptr and doesn't store anything
            // In that case, old decoder was never transferred to cache and must be deleted
            if (oldDecoder && evictedDecoder == nullptr && m_decoderCache.GetDecoder(key) == nullptr) {
                // Cache is disabled, delete old decoder explicitly
                delete oldDecoder;
            }
            
            // Reinitialize SeekUtils with new demuxer and decoder
            m_pSeekUtils.reset(new SeekUtils(m_pDemuxer.get(), m_pDecoder.get()));
            
            // Restore OpenGOP state
            if (m_bEnableOpenGOP) {
                m_pSeekUtils->EnableOpenGOPSupport(true);
            }
            
            // Restore keyframe indices if same source (avoids expensive re-scanning)
            if (hadKeyFrameIndices && isSameSource) {
                m_pSeekUtils->SetKeyFrameIndices(savedKeyFrameIndices);
                if (m_bEnableOpenGOP && !savedIDRFrameIndices.empty()) {
                    m_pSeekUtils->SetIDRFrameIndices(savedIDRFrameIndices);
                }
            } else if (!isSameSource) {
                // New source - check if scanning is needed for this specific video
                // Use explicit flag if set, otherwise auto-detect for this video
                bool bScanRequired = m_bNeedScannedMetadata || m_pSeekUtils->IsScanRequired();
                if (bScanRequired) {
                    m_pSeekUtils->SetNeedScannedMetadata(true);  // Propagate to SeekUtils for consistency
                    ScanAndSetKeyFrameIndices();
                }
            }
            
            return ReconfigureStatus::CacheMiss;
        }
        
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("ReconfigureDecoder failed: ") + e.what());
    }
}

bool NvVideoDecoder::IsSeekable() const {
    return m_pDemuxer && m_pDemuxer->IsSeekable();
}

int NvVideoDecoder::GetBitDepth() const {
    if (!m_pDemuxer) {
        throw std::runtime_error("Demuxer not initialized");
    }
    return m_pDemuxer->GetBitDepth();
}

std::string NvVideoDecoder::GetContainerName() const {
    if (!m_pDemuxer) {
        return "unknown";
    }
    return m_pDemuxer->GetContainerName();
}

cudaVideoChromaFormat NvVideoDecoder::GetChromaFormat() const {
    if (!m_pDemuxer) {
        throw std::runtime_error("Demuxer not initialized");
    }
    return AVPixelFormat2NvChromaFormat(m_pDemuxer->GetChromaFormat());
}

void NvVideoDecoder::ResetDecoderIfRequired(uint32_t targetIndex) {
    if (IsBackwardSeek(targetIndex)) {
        // Backward seek detected - need to reset decoder
        int64_t currentIndex = m_pSeekUtils->GetPreviousTargetIndex();
        std::cout << "NvVideoDecoder: Backward seek detected (from frame " << currentIndex 
                  << " to frame " << targetIndex << ") - reconfiguring decoder" << std::endl;
        
        // For both seekable and sequential mode:
        // - Seekable: Will seek to target GOP
        // - Sequential: Will decode forward from beginning to target
        // Note: Sequential mode backward seeks only work if source is re-openable
        m_pSeekUtils->setEOS(true);
        ReconfigureDecoder(m_strEncSource, true);
    }
}

bool NvVideoDecoder::IsBackwardSeek(uint32_t targetIndex) {
    return m_pSeekUtils->IsSeekBackwards(static_cast<int64_t>(targetIndex));
}

void NvVideoDecoder::ValidateFrameIndex(uint32_t index) {
    StreamMetadata metadata = GetStreamMetadata();
    
    // Only validate if we have frame count metadata
    // For raw elementary streams (numFrames == 0), validation happens during decode
    if (metadata.numFrames > 0 && index >= metadata.numFrames) {
        throw std::out_of_range("Frame index " + std::to_string(index) + 
                               " is out of range [0, " + std::to_string(metadata.numFrames - 1) + "]");
    }
}

void NvVideoDecoder::HandleEvictedDecoder(NvDecoder* decoder) {
    if (decoder != nullptr) {
        // Cache capacity exceeded - need to delete the LRU decoder
        std::cout << "NvVideoDecoder: Cache capacity exceeded. Deleting LRU decoder." << std::endl;
        
        // Delete the evicted decoder directly
        delete decoder;
    }
}

void NvVideoDecoder::ScanAndSetKeyFrameIndices() {
    if (!m_pDemuxer || !m_pSeekUtils) {
        return;
    }
    
    std::string container = m_pDemuxer->GetContainerName();
    
    // For containers that benefit from keyframe scanning
    // Check if container string contains known seekable formats (use substring match for flexibility)
    bool isSeekableContainer = (container.find("matroska") != std::string::npos) || 
                                (container.find("webm") != std::string::npos) ||
                                (container.find("mov") != std::string::npos) ||
                                (container.find("mp4") != std::string::npos) ||
                                (container.find("flv") != std::string::npos) ||
                                (container.find("avi") != std::string::npos);
    
    if (isSeekableContainer) {
        try {
            // Scan for IDR frames only if Open GOP support is enabled
            // This avoids expensive NAL parsing when not needed
            bool bScanForIDR = m_pSeekUtils->IsOpenGOPSupportEnabled();
            ScannedStreamMetadata scanned = m_pDemuxer->GetScannedStreamMetadata(bScanForIDR);
            
            // Update demuxer's scanned metadata so GetStreamMetadata() returns correct values
            m_pDemuxer->UpdateScannedStreamMetadata(scanned);
            
            if (!scanned.keyFrameIndices.empty()) {
                m_pSeekUtils->SetKeyFrameIndices(scanned.keyFrameIndices);
            }
            
            // Set IDR frame indices for open GOP support (if enabled)
            if (bScanForIDR && !scanned.idrFrameIndices.empty()) {
                m_pSeekUtils->SetIDRFrameIndices(scanned.idrFrameIndices);
            }
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to get scanned stream metadata: " << e.what() << std::endl;
        }
    }
}

