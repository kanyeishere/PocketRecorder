/*
* Copyright 2017-2026 NVIDIA Corporation.  All rights reserved.
*
* Please refer to the NVIDIA end user license agreement (EULA) associated
* with this source code for terms and conditions that govern your use of
* this software. Any use, reproduction, disclosure, or distribution of
* this software and related documentation outside the terms of the EULA
* is strictly prohibited.
*
*/

//---------------------------------------------------------------------------
//! \file AppDecVideoDecoder.cpp
//! \brief Video decoder application demonstrating NvVideoDecoder API
//!
//! This application showcases the NvVideoDecoder interface for video decoding
//! with support for:
//! - Random frame access (decode specific frames by index)
//! - Batch decoding (fetch N sequential frames from each index)
//! - Playlist mode (process multiple videos with decoder caching/reuse)
//! - Keyframe scanning for efficient random access
//! - Decode statistics collection
//! - Flexible output formats (semi-planar NV12/P016 or planar I420/I420_16)
//!
//! Key Features:
//! - NvVideoDecoder provides high-level frame access via index operator
//! - Automatic backward seek handling and keyframe management
//! - Decoder caching for efficient playlist processing
//! - Batch operator for efficient multi-frame retrieval
//---------------------------------------------------------------------------

#include <iostream>
#include <algorithm>
#include <chrono>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <string>
#include <cuda.h>
#include "../../NvCodec/NvDecoder/NvDecoder.h"
#include "../../Utils/NvVideoDecoder.h"
#include "../../Utils/NvCodecUtils.h"

simplelogger::Logger *logger = simplelogger::LoggerFactory::CreateConsoleLogger();

/**
 * @brief Convert semi-planar (NV12/P016) to planar (I420/I420_16) format
 */
void ConvertSemiplanarToPlanar(uint8_t *pHostFrame, int nWidth, int nHeight, int nBitDepth, uint8_t nChromaFormat) {
    if (nBitDepth == 8) {
        YuvConverter<uint8_t> converter8(nWidth, nHeight, nChromaFormat);
        converter8.UVInterleavedToPlanar(pHostFrame);
    } else {
        YuvConverter<uint16_t> converter16(nWidth, nHeight, nChromaFormat);
        converter16.UVInterleavedToPlanar((uint16_t *)pHostFrame);
    }
}

/**
 * @brief Convert cudaVideoChromaFormat to user-friendly string
 */
std::string ChromaFormatToString(cudaVideoChromaFormat format) {
    switch (format) {
        case cudaVideoChromaFormat_Monochrome: return "Monochrome";
        case cudaVideoChromaFormat_420: return "4:2:0";
        case cudaVideoChromaFormat_422: return "4:2:2";
        case cudaVideoChromaFormat_444: return "4:4:4";
        default: return "Unknown";
    }
}

/**
 * @brief Parse comma-separated frame indices from a string
 * Supports both individual indices and range syntax:
 * - Individual: "2,3,6,8,99,29900"
 * - Range: "0:10:2" (start:end:step, end is inclusive) or "0:10" (start:end, step=1)
 * - Mixed: "0:10:2,20,30:35"
 * @param szIndices Comma-separated frame indices or ranges
 * @param nTotalFrames Total number of frames in stream (for validation)
 * @return Vector of frame indices
 */

/**
 * @brief Print frame indices to console
 * @param vFrameIndices Vector of frame indices to print
 * @param szPrefix Prefix string to print before indices (e.g., "Parsed 10 frame indices: ")
 * @param bAlwaysPrint If true, always print indices; if false, only print if verbose mode
 * @param bVerbose Verbose mode flag (only used if bAlwaysPrint is false)
 */
void PrintFrameIndices(const std::vector<uint32_t>& vFrameIndices, const std::string& szPrefix, 
                       bool bAlwaysPrint = true, bool bVerbose = false) {
    if (vFrameIndices.empty()) {
        return;
    }
    
    // Only print indices if always print is true, or if verbose mode is enabled
    bool shouldPrintIndices = bAlwaysPrint || bVerbose;
    
    if (shouldPrintIndices) {
        std::cout << szPrefix;
        
        // In verbose mode, print all indices; otherwise print up to 20
        size_t numToPrint = bVerbose ? vFrameIndices.size() : (vFrameIndices.size() < 20 ? vFrameIndices.size() : 20);
        
        for (size_t i = 0; i < numToPrint; i++) {
            std::cout << vFrameIndices[i];
            if (i < numToPrint - 1) {
                std::cout << ", ";
            }
        }
        
        // Only show ellipsis if not in verbose mode and there are more than 20
        if (!bVerbose && vFrameIndices.size() > 20) {
            std::cout << ", ... (+" << (vFrameIndices.size() - 20) << " more)";
        }
        std::cout << std::endl;
    } else {
        // Just print the prefix without indices
        std::cout << szPrefix << std::endl;
    }
}

std::vector<uint32_t> ParseFrameIndicesFromString(const std::string& szIndices, uint32_t nTotalFrames) {
    std::vector<uint32_t> vFrameIndices;
    std::stringstream ss(szIndices);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        // Remove whitespace
        item.erase(std::remove_if(item.begin(), item.end(), ::isspace), item.end());
        if (item.empty()) {
            continue;
        }
        
        try {
            // Check if this is a range (contains ':')
            size_t colonPos = item.find(':');
            if (colonPos != std::string::npos) {
                // Parse range syntax: start:end:step or start:end
                std::vector<std::string> rangeParts;
                std::stringstream rangeStream(item);
                std::string rangePart;
                
                while (std::getline(rangeStream, rangePart, ':')) {
                    // Remove whitespace from each part
                    rangePart.erase(std::remove_if(rangePart.begin(), rangePart.end(), ::isspace), rangePart.end());
                    if (!rangePart.empty()) {
                        rangeParts.push_back(rangePart);
                    }
                }
                
                if (rangeParts.size() < 2 || rangeParts.size() > 3) {
                    std::cout << "Warning: Invalid range syntax '" << item << "' - expected 'start:end' or 'start:end:step' - skipping" << std::endl;
                    continue;
                }
                
                uint32_t start = std::stoul(rangeParts[0]);
                uint32_t end = std::stoul(rangeParts[1]);
                uint32_t step = (rangeParts.size() == 3) ? std::stoul(rangeParts[2]) : 1;
                
                if (step == 0) {
                    std::cout << "Warning: Invalid step value 0 in range '" << item << "' - skipping" << std::endl;
                    continue;
                }
                
                if (start > end) {
                    std::cout << "Warning: Invalid range '" << item << "' - start (" << start 
                             << ") is greater than end (" << end << ") - skipping" << std::endl;
                    continue;
                }
                
                // Generate indices from range ([start:end] - end is inclusive)
                // For example, 0:10:2 generates [0, 2, 4, 6, 8, 10]
                for (uint32_t idx = start; idx <= end; idx += step) {
                    if (nTotalFrames > 0 && idx >= nTotalFrames) {
                        std::cout << "Warning: Frame index " << idx 
                                 << " from range '" << item << "' exceeds total frames (" 
                                 << nTotalFrames << ") - skipping remaining indices" << std::endl;
                        break;  // All subsequent indices will also exceed total frames
                    } else {
                        vFrameIndices.push_back(idx);
                    }
                }
            } else {
                // Single index
                uint32_t nFrameIndex = std::stoul(item);
                
                if (nTotalFrames > 0 && nFrameIndex >= nTotalFrames) {
                    std::cout << "Warning: Frame index " << nFrameIndex 
                             << " exceeds total frames (" << nTotalFrames << ") - skipping" << std::endl;
                } else {
                    vFrameIndices.push_back(nFrameIndex);
                }
            }
        } catch (const std::exception& e) {
            std::cout << "Warning: Invalid frame index or range '" << item << "' - skipping (" << e.what() << ")" << std::endl;
        }
    }
    
    if (!vFrameIndices.empty()) {
        PrintFrameIndices(vFrameIndices, "Parsed " + std::to_string(vFrameIndices.size()) + " frame indices from command line: ");
    }
    
    return vFrameIndices;
}

/**
 * @brief Read frame indices from a text file
 * Format: comma-separated frame numbers, one or more per line
 */
std::vector<uint32_t> ReadFrameIndicesFromFile(const std::string& szFilename, uint32_t nTotalFrames) {
    std::vector<uint32_t> vFrameIndices;
    std::ifstream file(szFilename);
    
    if (!file.is_open()) {
        std::cout << "Error: Could not open frame indices file: " << szFilename << std::endl;
        return vFrameIndices;
    }
    
    std::string line;
    int nLineNumber = 1;
    
    while (std::getline(file, line)) {
        // Remove whitespace and skip empty lines
        line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
        if (line.empty()) {
            nLineNumber++;
            continue;
        }
        
        // Parse comma-separated values in the line
        std::stringstream ss(line);
        std::string item;
        
        while (std::getline(ss, item, ',')) {
            if (item.empty()) continue;
            
            try {
                int32_t nRawIndex = std::stoi(item);
                
                // Reject negative indices
                if (nRawIndex < 0) {
                    std::cout << "Warning: Negative index " << nRawIndex << " (line " << nLineNumber 
                             << ") is invalid - skipping" << std::endl;
                    continue;
                }
                
                uint32_t nFrameIndex = static_cast<uint32_t>(nRawIndex);
                
                // Only validate if nTotalFrames is known (non-zero)
                if (nTotalFrames > 0 && nFrameIndex >= nTotalFrames) {
                    std::cout << "Warning: Frame index " << nFrameIndex << " (line " << nLineNumber 
                             << ") exceeds total frames (" << nTotalFrames << ") - skipping" << std::endl;
                    continue;
                }
                vFrameIndices.push_back(nFrameIndex);
            } catch (const std::exception& e) {
                std::cout << "Warning: Invalid frame index '" << item << "' on line " << nLineNumber 
                         << " - skipping (" << e.what() << ")" << std::endl;
            }
        }
        nLineNumber++;
    }
    
    file.close();
    
    if (vFrameIndices.empty()) {
        std::cout << "Warning: No valid frame indices found in file: " << szFilename << std::endl;
    } else {
        PrintFrameIndices(vFrameIndices, "Successfully loaded " + std::to_string(vFrameIndices.size()) + " frame indices from file: ");
    }
    
    return vFrameIndices;
}

/**
 * @brief Print keyframe and IDR frame indices from scanned metadata
 * @param scanned ScannedStreamMetadata containing keyframe and IDR indices
 * @param bVerbose If true, print all indices; if false, print only counts
 */
void PrintKeyFrameIndices(const ScannedStreamMetadata& scanned, bool bVerbose = false) {
    if (scanned.keyFrameIndices.empty()) {
        return;
    }
    
    // Always print keyframe count
    std::cout << "  Keyframes found: " << scanned.keyFrameIndices.size() << std::endl;
    
    // Print keyframe indices only in verbose mode
    if (bVerbose) {
        std::cout << "  Keyframe indices: ";
        for (size_t i = 0; i < scanned.keyFrameIndices.size(); i++) {
            std::cout << scanned.keyFrameIndices[i];
            if (i < scanned.keyFrameIndices.size() - 1) std::cout << ", ";
        }
        std::cout << std::endl;
    }
    
    // Print IDR frame indices if available
    if (!scanned.idrFrameIndices.empty()) {
        // Always print IDR frame count
        std::cout << "  IDR frames found: " << scanned.idrFrameIndices.size() << std::endl;
        
        // Print IDR frame indices only in verbose mode
        if (bVerbose) {
            std::cout << "  IDR frame indices: ";
            for (size_t i = 0; i < scanned.idrFrameIndices.size(); i++) {
                std::cout << scanned.idrFrameIndices[i];
                if (i < scanned.idrFrameIndices.size() - 1) std::cout << ", ";
            }
            std::cout << std::endl;
        }
    }
}

void PrintUsage(const char* szProgramName) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Video Decoder with NvVideoDecoder API" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nUsage: " << szProgramName << " -i <input_video_file> [options]\n" << std::endl;
    
    std::cout << "Basic Options:" << std::endl;
    std::cout << "  -i <file>             Input video file (required, unless using -playlist)" << std::endl;
    std::cout << "  -o <file>             Output YUV file (default: out.yuv)" << std::endl;
    std::cout << "  -gpu <id>             GPU device ID (default: 0)" << std::endl;
    std::cout << "  -f <file>             Text file with frame indices to decode" << std::endl;
    std::cout << "                        (comma-separated, one or more per line)" << std::endl;
    std::cout << "  -framelist <indices>  Comma-separated frame indices or ranges" << std::endl;
    std::cout << "                        Examples:" << std::endl;
    std::cout << "                          Individual: 2,3,6,8,99" << std::endl;
    std::cout << "                          Range: 0:10:2 (every 2nd frame from 0 to 10)" << std::endl;
    std::cout << "                          Mixed: 0:10:2,20,30:35" << std::endl;
    std::cout << "                        Range syntax: start:end:step or start:end (step=1)" << std::endl;
    std::cout << "                        Alternative to -f for direct command-line input" << std::endl;
    std::cout << "                        Note: -framelist takes priority over -f if both specified" << std::endl;
    std::cout << "  -t <values>           Time values in seconds: single value, comma-separated, or range" << std::endl;
    std::cout << "                        Examples:" << std::endl;
    std::cout << "                          Single: -t 0.5 (one frame at 0.5 seconds)" << std::endl;
    std::cout << "                          Comma-separated: -t \"1,2,5\" (frames at 1s, 2s, 5s)" << std::endl;
    std::cout << "                          Range: -t \"0:10:2\" (every 2 seconds from 0 to 10)" << std::endl;
    std::cout << "                          Mixed: -t \"0:10:2,15.5,20:25:1\"" << std::endl;
    std::cout << "                        Uses GetIndexFromTimeInSeconds to get frame indices" << std::endl;
    std::cout << "                        Priority: -framelist > -f > -t > all frames" << std::endl;
    std::cout << "                        If none specified, decodes entire stream" << std::endl;
    std::cout << "  -outplanar            Convert output to planar format (I420/YV12)" << std::endl;
    std::cout << "                        Default is semi-planar (NV12/P016)" << std::endl;
    std::cout << "  -opengop              Enable open GOP support for seeking (default: disabled)" << std::endl;
    std::cout << "  -disablePTSFrameSkip  Disable PTS-based frame skipping during seek (default: enabled)" << std::endl;
    std::cout << "                        When enabled, decoder detects IDR frames for proper seeking" << std::endl;
    std::cout << "                        in open GOP streams (non-IDR I-frames)" << std::endl;
    std::cout << "  -batch <N>            Batch mode: fetch N sequential frames from each index" << std::endl;
    std::cout << "                        Example: -batch 5 with indices 100,200 fetches:" << std::endl;
    std::cout << "                        100-104, 200-204 (uses batch operator)" << std::endl;
    std::cout << "  -v                    Verbose mode: print keyframe/IDR indices, per-frame details, and stats info etc." << std::endl;
    std::cout << "  -h, -help             Show this help message" << std::endl;
    
    std::cout << "\nAdvanced Options:" << std::endl;
    std::cout << "  -dumpstats <file>     Dump decode statistics to binary file (single video clip only)" << std::endl;
    std::cout << "                        Note: Ignored in playlist mode; use 'stats:' in playlist instead" << std::endl;
    std::cout << "  -playlist <file>      Decode multiple videos from playlist file" << std::endl;
    std::cout << "  -cache <N>            Enable internal decoder cache with size N (0 = disabled, default: 0)" << std::endl;
    std::cout << "                        NvVideoDecoder caches decoders internally by codec properties" << std::endl;
    std::cout << "                        (codec, bitDepth, chromaFormat)" << std::endl;
    std::cout << "                        Stores last N decoders for reuse when calling ReconfigureDecoder()" << std::endl;
    
    std::cout << "\nExamples:" << std::endl;
    std::cout << "  # Decode entire video" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "  # Decode specific frames from command line" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv -framelist 2,3,6,8,99,29900" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "  # Decode frames using range syntax (every 2nd frame from 0 to 10)" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv -framelist 0:10:2" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "  # Decode frames from file" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv -f indices.txt" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "  # Batch mode: decode 5 sequential frames starting at each index" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv -framelist 100,200,300 -batch 5" << std::endl;
    std::cout << "" << std::endl;
    
    std::cout << "\nPlaylist File Format:" << std::endl;
    std::cout << "  input:video1.mp4" << std::endl;
    std::cout << "  output:output1.yuv        # Optional" << std::endl;
    std::cout << "  stats:output1.stats       # Optional" << std::endl;
    std::cout << "  indices:0,10,20,30,40     # Optional (comma-separated frame indices)" << std::endl;
    std::cout << "  indices:0:10:2            # Or use range syntax (every 2nd frame from 0 to 10)" << std::endl;
    std::cout << "  indices:0:10:2,20,30:35   # Mixed: ranges and individual indices" << std::endl;
    std::cout << "  # Empty line separates videos" << std::endl;
    std::cout << "  input:video2.mp4" << std::endl;
    std::cout << "  output:output2.yuv" << std::endl;
    std::cout << "  stats:output2.stats" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "  Notes:" << std::endl;
    std::cout << "  - 'output:', 'stats:', and 'indices:' are all optional" << std::endl;
    std::cout << "  - -dumpstats flag is ignored; use 'stats:' in playlist to control per-video" << std::endl;
    std::cout << "  - Playlist 'indices:' overrides -f/-framelist command-line options" << std::endl;
    
    std::cout << "\nMore Examples:" << std::endl;
    std::cout << "  # Dump decode statistics" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv -dumpstats output.stats" << std::endl;
    std::cout << "\n  # Playlist mode with decoder caching" << std::endl;
    std::cout << "  " << szProgramName << " -playlist videos.txt -cache 4" << std::endl;
    std::cout << "\n  # Playlist with frame indices from file" << std::endl;
    std::cout << "  " << szProgramName << " -playlist videos.txt -f indices.txt -cache 4" << std::endl;
    std::cout << "  Note: Playlist 'indices:' lines override -f/-framelist for those videos" << std::endl;
    std::cout << "\n  # Decode to planar format on specific GPU" << std::endl;
    std::cout << "  " << szProgramName << " -i video.mp4 -o output.yuv -outplanar -gpu 1" << std::endl;
}

/**
 * @brief Initialize decoder parameters after first frame decode
 */
void InitializeDecoder(NvDecoder* pNvDecoder, size_t& nFrameSize, int& nBitDepth,
                       int& nWidth, int& nHeight, uint8_t& nChromaFormat,
                       bool& bSemiPlanar, std::vector<uint8_t>& vHostFrame,
                       bool bOutputPlanar) {
    nFrameSize = pNvDecoder->GetFrameSize();
    nBitDepth = pNvDecoder->GetBitDepth();
    nWidth = pNvDecoder->GetWidth();
    nHeight = pNvDecoder->GetHeight();
    nChromaFormat = pNvDecoder->GetOutputChromaFormat();
    cudaVideoSurfaceFormat outputFormat = pNvDecoder->GetOutputFormat();
    bSemiPlanar = (outputFormat == cudaVideoSurfaceFormat_NV12 || 
                    outputFormat == cudaVideoSurfaceFormat_P016 ||
                    outputFormat == cudaVideoSurfaceFormat_NV16 ||
                    outputFormat == cudaVideoSurfaceFormat_P216);
    vHostFrame.resize(nFrameSize);
    
    std::cout << "Output format: " << (bSemiPlanar ? "Semi-planar" : "Planar");
    if (bOutputPlanar && bSemiPlanar) {
        std::cout << " (will convert to planar)";
    }
    std::cout << std::endl;
}

/**
 * @brief Process a single decoded frame
 */
void ProcessFrame(NvDecFrame& frame, NvDecoder* pNvDecoder, std::vector<uint8_t>& vHostFrame,
                  size_t nFrameSize, int nWidth, int nHeight, int nBitDepth,
                  uint8_t nChromaFormat, bool bSemiPlanar, bool bOutputPlanar,
                  std::ofstream& fpOut, bool bWriteOutput) {
    if (frame.data != 0) {
        // Only copy frame from GPU to host if we need to write output
        if (bWriteOutput) {
            // Copy frame from GPU to host
            ck(cuMemcpyDtoH(vHostFrame.data(), frame.data, nFrameSize));
            
            // Convert to planar if requested and needed
            if (bOutputPlanar && bSemiPlanar) {
                ConvertSemiplanarToPlanar(vHostFrame.data(), nWidth, nHeight, nBitDepth, nChromaFormat);
            }

            // Write to output file
            if (fpOut.is_open()) {
                if (pNvDecoder->GetWidth() == pNvDecoder->GetDecodeWidth()) {
                    fpOut.write(reinterpret_cast<const char*>(vHostFrame.data()), nFrameSize);
                } else {
                    // 4:2:0/4:2:2 output width is 2 byte aligned. If decoded width is odd, luma has 1 pixel padding
                    // Remove padding from luma while dumping it to disk
                    // dump luma
                    uint8_t* pFrame = vHostFrame.data();
                    for (int i = 0; i < nHeight; i++) {
                        fpOut.write(reinterpret_cast<const char*>(pFrame), static_cast<std::streamsize>(pNvDecoder->GetDecodeWidth()) * pNvDecoder->GetBPP());
                        pFrame += pNvDecoder->GetWidth() * pNvDecoder->GetBPP();
                    }
                    // dump Chroma
                    fpOut.write(reinterpret_cast<const char*>(pFrame), pNvDecoder->GetChromaPlaneSize());
                }
            }
        }

        // Unlock frame
        uint8_t* pFramePtr = reinterpret_cast<uint8_t*>(frame.data);
        pNvDecoder->UnlockFrame(&pFramePtr);
    }
}

/**
 * @brief Decode and write frames to output file
 */
void DecodeFrames(NvVideoDecoder& decoder, const StreamMetadata& metadata, 
                  const std::vector<uint32_t>& vFrameIndices,
                  const std::string& szOutputFile, bool bOutputPlanar, uint32_t nBatchSize = 0, bool bVerbose = false) {
    std::ofstream fpOut;
    bool bWriteOutput = !szOutputFile.empty();
    // Decoder info will be obtained after first frame decode
    NvDecoder* pNvDecoder = decoder.GetDecoder();
    size_t nFrameSize = 0;
    int nBitDepth = 8;
    int nWidth = 0, nHeight = 0;
    uint8_t nChromaFormat = 0;
    bool bSemiPlanar = false;
    std::vector<uint8_t> vHostFrame;
    bool bDecoderInitialized = false;
    
    if (bWriteOutput) {
        fpOut.open(szOutputFile, std::ios::out | std::ios::binary);
        if (!fpOut) {
            std::cerr << "Error: Unable to open output file: " << szOutputFile << std::endl;
            return;
        }
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Frame Processing" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    uint32_t nSuccessCount = 0;
    
    if (vFrameIndices.empty()) {
        // Decode entire stream
        uint32_t nNumFramesToDecode = metadata.numFrames;
        bool bDecodeUntilEOS = (nNumFramesToDecode == 0);
        
        if (bDecodeUntilEOS) {
            std::cout << "Decoding entire stream (unknown frame count - will decode until EOS)\n" << std::endl;
        } else {
            std::cout << "Decoding entire stream (" << nNumFramesToDecode << " frames)\n" << std::endl;
        }
        
        uint32_t i = 0;
        while (bDecodeUntilEOS || i < nNumFramesToDecode) {
            try {
                NvDecFrame frame = decoder[i];
                
                // Check for EOS
                if (bDecodeUntilEOS && frame.data == 0) {
                    std::cout << "\nReached end of stream at frame " << i << std::endl;
                    break;
                }
                
                // Initialize decoder info after first successful decode
                if (!bDecoderInitialized) {
                    InitializeDecoder(pNvDecoder, nFrameSize, nBitDepth, nWidth, nHeight, 
                                     nChromaFormat, bSemiPlanar, vHostFrame, bOutputPlanar);
                    bDecoderInitialized = true;
                }
                
                // Process the frame
                ProcessFrame(frame, pNvDecoder, vHostFrame, nFrameSize, nWidth, nHeight,
                           nBitDepth, nChromaFormat, bSemiPlanar, bOutputPlanar, fpOut, bWriteOutput);
                
                nSuccessCount++;
                i++;
                
                // Progress indicator
                if (i % 100 == 0) {
                    std::cout << "Decoded " << i << " frames...\r" << std::flush;
                }
                
            } catch (const std::exception& e) {
                // Check if EOS was actually reached by querying the decoder
                bool bEOS = decoder.IsEOSReached();
                
                if (bDecodeUntilEOS && bEOS) {
                    std::cout << "\nReached end of stream at frame " << i << std::endl;
                    break;
                } else {
                    std::cerr << "Error decoding frame " << i << ": " << e.what() << std::endl;
                    if (bDecodeUntilEOS) {
                        break;
                    }
                    i++;
                }
            }
        }
        
    } else {
        // Decode specific frames
        if (nBatchSize > 0) {
            std::cout << "Decoding in batch mode: " << vFrameIndices.size() << " batches x " << nBatchSize 
                      << " frames = " << (vFrameIndices.size() * nBatchSize) << " total frames\n" << std::endl;
        } else {
            std::cout << "Decoding " << vFrameIndices.size() << " frames from indices file\n" << std::endl;
        }
        
        for (size_t i = 0; i < vFrameIndices.size(); i++) {
            uint32_t nStartIdx = vFrameIndices[i];
            std::vector<NvDecFrame> frames;
            std::vector<uint32_t> batchIndices;
            
            try {
                // Build batch indices or single frame
                bool bStopDecode = false;
                if (nBatchSize > 0) {
                    // Batch mode: create N sequential indices starting from nStartIdx
                    for (uint32_t j = 0; j < nBatchSize; j++) {
                        batchIndices.push_back(nStartIdx + j);
                    }
                    frames = decoder[batchIndices];  // Batch operator
                    
                    // Check if batch operator returned fewer frames than requested (may indicate EOS)
                    if (frames.size() < batchIndices.size() && decoder.IsEOSReached()) {
                        bStopDecode = true;  // Mark to stop after processing received frames
                    }
                } else {
                    // Single frame mode
                    NvDecFrame frame = decoder[nStartIdx];
                    frames.push_back(frame);
                    batchIndices.push_back(nStartIdx);
                }
                
                // Process all frames in the batch
                for (size_t idx = 0; idx < frames.size(); idx++) {
                    NvDecFrame& frame = frames[idx];
                    uint32_t nFrameIdx = batchIndices[idx];
                    
                    // Initialize decoder info after first successful decode
                    if (!bDecoderInitialized) {
                        InitializeDecoder(pNvDecoder, nFrameSize, nBitDepth, nWidth, nHeight, 
                                         nChromaFormat, bSemiPlanar, vHostFrame, bOutputPlanar);
                        bDecoderInitialized = true;
                    }
                    
                    // Display batch info if in batch mode (verbose only)
                    if (bVerbose) {
                        if (nBatchSize > 0) {
                            std::cout << "Frame " << nFrameIdx << " [Batch " << (i+1) << "/" << vFrameIndices.size() 
                                      << ", Frame " << (idx+1) << "/" << nBatchSize << "]";
                        } else {
                            std::cout << "Frame " << nFrameIdx << " (" << (i+1) << "/" << vFrameIndices.size() << ")";
                        }
                        
                        // Print decoded frame information
                        if (frame.data != 0) {
                            std::cout << " [Decoded: " << nWidth << "x" << nHeight << ", " << nBitDepth << "-bit";
                            if (frame.isSkipped) {
                                std::cout << ", skipped";
                            }
                            if (frame.timestamp > 0) {
                                std::cout << ", PTS=" << frame.timestamp;
                            }
                            std::cout << "]";
                        }
                        std::cout << std::endl;
                    }
                    
                    // Process the frame
                    ProcessFrame(frame, pNvDecoder, vHostFrame, nFrameSize, nWidth, nHeight,
                               nBitDepth, nChromaFormat, bSemiPlanar, bOutputPlanar, fpOut, bWriteOutput);
                    
                    nSuccessCount++;
                }
                
                // After processing frames, check if we should stop decoding
                if (bStopDecode) {
                    if (nSuccessCount > 0) {
                        uint32_t lastFrameIdx = frames.empty() ? nStartIdx : batchIndices[frames.size() - 1];
                        std::cout << "\nReached end of stream at frame " << lastFrameIdx << std::endl;
                        std::cout << "Stopping frame decoding (remaining " << (vFrameIndices.size() - i - 1) 
                                  << " batch(es) skipped)" << std::endl;
                    }
                    break;  // Stop processing remaining batches
                }
                
            } catch (const std::exception& e) {
                // Check if EOS was actually reached by querying the decoder
                bool bEOS = decoder.IsEOSReached();
                
                if (bEOS) {
                    // First EOS error - print message and stop processing remaining frames
                    if (nSuccessCount > 0) {
                        std::cout << "\nReached end of stream at frame " << nStartIdx << std::endl;
                        std::cout << "Stopping frame decoding (remaining " << (vFrameIndices.size() - i - 1) 
                                  << " frame(s) skipped)" << std::endl;
                    } else {
                        // EOS reached before any successful decode
                        std::cout << "\nReached end of stream (no frames decoded)" << std::endl;
                    }
                    break;  // Stop processing remaining frames
                } else {
                    // Non-EOS error - continue processing
                    if (nBatchSize > 0) {
                        std::cerr << "Error decoding batch starting at frame " << nStartIdx << ": " << e.what() << std::endl;
                    } else {
                        std::cerr << "Error decoding frame " << nStartIdx << ": " << e.what() << std::endl;
                    }
                }
            }
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Decoding Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Successfully decoded: " << nSuccessCount << " frames" << std::endl;
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    if (nSuccessCount > 0) {
        std::cout << "Average: " << (duration.count() / static_cast<double>(nSuccessCount)) << " ms per frame" << std::endl;
        if (duration.count() > 0) {
            double dFps = (nSuccessCount * 1000.0) / duration.count();
            std::cout << "Throughput: " << dFps << " fps" << std::endl;
        }
    }
    std::cout << "========================================\n" << std::endl;
    
    if (fpOut.is_open()) {
        fpOut.close();
        std::cout << "Output written to: " << szOutputFile << std::endl;
    }
}

// ============================================================================
// ADVANCED FEATURES
// ============================================================================
// The following functions implement advanced decoding features:
// - Decode statistics dumping
// - Batch processing
// - Playlist support with decoder reconfiguration
// - Decoder caching across multiple videos (LRU cache with N entries)
// ============================================================================

/**
 * @brief Parse time string supporting comma-separated values and range syntax
 * @param szTimeValues Time string (e.g., "1.0,2.5,5.0" or "0:10:2" or "0:10:2,15.5,20:25:1")
 * @param duration Video duration in seconds (optional, for validation)
 * @return Vector of time values in seconds
 */
std::vector<double> ParseTimeValues(const std::string& szTimeValues, double duration = 0.0) {
    std::vector<double> timeValues;
    std::stringstream ss(szTimeValues);
    std::string item;
    
    while (std::getline(ss, item, ',')) {
        // Remove whitespace
        item.erase(std::remove_if(item.begin(), item.end(), ::isspace), item.end());
        if (item.empty()) {
            continue;
        }
        
        try {
            // Check if this is a range (contains ':')
            size_t colonPos = item.find(':');
            if (colonPos != std::string::npos) {
                // Parse range syntax: start:end:step or start:end
                std::vector<std::string> rangeParts;
                std::stringstream rangeStream(item);
                std::string rangePart;
                
                while (std::getline(rangeStream, rangePart, ':')) {
                    // Remove whitespace from each part
                    rangePart.erase(std::remove_if(rangePart.begin(), rangePart.end(), ::isspace), rangePart.end());
                    if (!rangePart.empty()) {
                        rangeParts.push_back(rangePart);
                    }
                }
                
                if (rangeParts.size() < 2 || rangeParts.size() > 3) {
                    std::cout << "Warning: Invalid time range syntax '" << item << "' - expected 'start:end' or 'start:end:step' - skipping" << std::endl;
                    continue;
                }
                
                double start = std::stod(rangeParts[0]);
                double end = std::stod(rangeParts[1]);
                double step = (rangeParts.size() == 3) ? std::stod(rangeParts[2]) : 1.0;
                
                if (step == 0.0) {
                    std::cout << "Warning: Invalid step value 0.0 in range '" << item << "' - skipping" << std::endl;
                    continue;
                }
                
                // Cap end time to duration if duration is provided and valid
                if (duration > 0.0 && end > duration) {
                    std::cout << "Warning: Range end time " << end << " exceeds duration " << duration << " - capping to " << duration << std::endl;
                    end = duration;
                }
                
                // Generate time values from range ([start:end] - end is inclusive)
                // Safety check: prevent excessive iterations
                const uint32_t maxRangeIterations = 1000000; // Maximum 1 million time values
                uint32_t iterationCount = 0;
                
                if (step > 0.0) {
                    for (double t = start; t <= end && iterationCount < maxRangeIterations; t += step) {
                        ++iterationCount;
                        timeValues.push_back(t);
                    }
                } else {
                    // Negative step: descending range
                    for (double t = start; t >= end && iterationCount < maxRangeIterations; t += step) {
                        ++iterationCount;
                        timeValues.push_back(t);
                    }
                }
                
                if (iterationCount >= maxRangeIterations) {
                    std::cout << "Warning: Reached iteration limit (" << maxRangeIterations << ") for time range '" << item << "'. Stopping to prevent excessive computation." << std::endl;
                }
            } else {
                // Single time value
                double timeVal = std::stod(item);
                timeValues.push_back(timeVal);
            }
        } catch (const std::exception& e) {
            std::cout << "Warning: Invalid time value or range '" << item << "' - skipping (" << e.what() << ")" << std::endl;
        }
    }
    
    // Sort and remove duplicates
    std::sort(timeValues.begin(), timeValues.end());
    timeValues.erase(std::unique(timeValues.begin(), timeValues.end()), timeValues.end());
    
    return timeValues;
}

/**
 * @brief Generate frame indices from a list of time values using GetIndexFromTimeInSeconds
 * @param decoder Decoder instance to use for time-to-index conversion
 * @param timeValues Vector of time values in seconds
 * @param numFrames Total number of frames in the video (for bounds checking)
 * @return Vector of frame indices
 */
std::vector<uint32_t> GenerateIndicesFromTimeValues(NvVideoDecoder* decoder, const std::vector<double>& timeValues, uint32_t numFrames) {
    std::vector<uint32_t> indices;
    if (!decoder || timeValues.empty()) {
        return indices;
    }
    
    // Get duration from metadata for validation (if numFrames > 0)
    StreamMetadata metadata = decoder->GetStreamMetadata();
    double duration = (metadata.averageFPS > 0 && numFrames > 0) ? (double)numFrames / metadata.averageFPS : 0.0;
    
    std::unordered_set<uint32_t> seenIndices;  // Use unordered_set to avoid duplicates
    for (double t : timeValues) {
        if (t < 0.0) {
            continue;
        }
        
        // Validate time against duration if available
        if (duration > 0.0 && t > duration) {
            std::cout << "Warning: Time value " << t << " seconds exceeds video duration " << duration << " seconds - skipping" << std::endl;
            continue;
        }
        
        try {
            uint32_t frameIndex = decoder->GetIndexFromTimeInSeconds(static_cast<float>(t));
            // Ensure frame index is within valid range (skip check if numFrames is 0/unknown)
            if (numFrames == 0 || frameIndex < numFrames) {
                // Avoid duplicates
                if (seenIndices.find(frameIndex) == seenIndices.end()) {
                    indices.push_back(frameIndex);
                    seenIndices.insert(frameIndex);
                }
            }
        } catch (const std::exception& e) {
            // Handle errors from GetIndexFromTimeInSeconds
            std::cout << "Warning: Failed to get frame index for time " << t << " seconds: " << e.what() << " - skipping" << std::endl;
            continue;
        }
    }
    
    return indices;
}

/**
 * @brief Parse frame indices from -t option (time values or interval)
 * @param decoder Decoder instance to use for time-to-index conversion
 * @param szTimeValues Time values string (can be interval like "1.0" or specific times like "0:10:2" or "1,2,5")
 * @param numFrames Total number of frames in the video (for bounds checking)
 * @param averageFPS Average frame rate (for duration calculation)
 * @param bVerbose Whether to print verbose output
 * @return Vector of frame indices
 */
std::vector<uint32_t> ParseFrameIndicesFromTimeOption(NvVideoDecoder* decoder, const std::string& szTimeValues, uint32_t numFrames, double averageFPS, bool bVerbose) {
    std::vector<uint32_t> vFrameIndices;
    
    if (!decoder || szTimeValues.empty()) {
        return vFrameIndices;
    }
    
    // Calculate duration from numFrames and fps (more reliable than container duration for WebM)
    // If numFrames is 0, duration will be 0.0, but we can still process single time values
    double duration = (averageFPS > 0 && numFrames > 0) ? 
                      (double)numFrames / averageFPS : 0.0;
    
    // For ranges/comma-separated values, we need duration for parsing
    // For single values, we can work without duration
    bool needsDuration = (szTimeValues.find(':') != std::string::npos || szTimeValues.find(',') != std::string::npos);
    
    if (needsDuration && (duration <= 0.0 || duration > 86400.0)) {
        // More than 24 hours is suspicious
        std::cout << "Warning: Invalid or missing duration. Cannot parse time range/comma-separated values from -t option." << std::endl;
        std::cout << "  Duration: " << duration << " seconds, FPS: " << averageFPS 
                 << ", Frames: " << numFrames << std::endl;
        std::cout << "  Single time values (e.g., -t 0.5) can work without duration." << std::endl;
        return vFrameIndices;
    }
    
    // Parse time values similar to -framelist: single value, comma-separated, or range
    // If it contains ':' or ',', parse as comma-separated values or range
    if (szTimeValues.find(':') != std::string::npos || szTimeValues.find(',') != std::string::npos) {
        // Parse as comma-separated values or range
        std::vector<double> timeValues = ParseTimeValues(szTimeValues, duration);
        if (!timeValues.empty()) {
            vFrameIndices = GenerateIndicesFromTimeValues(decoder, timeValues, numFrames);
            PrintFrameIndices(vFrameIndices, "Using frame indices from -t '" + szTimeValues + "': " + std::to_string(vFrameIndices.size()) + " frame(s): ", false, bVerbose);
        }
    } else {
        // Single value: treat as a single time point (like -framelist with single index)
        try {
            double timeValue = std::stod(szTimeValues);
            if (timeValue >= 0.0) {
                // Single time value: get one frame at that time
                std::vector<double> singleTime = {timeValue};
                vFrameIndices = GenerateIndicesFromTimeValues(decoder, singleTime, numFrames);
                PrintFrameIndices(vFrameIndices, "Using frame indices from -t '" + szTimeValues + "': " + std::to_string(vFrameIndices.size()) + " frame(s): ", false, bVerbose);
            }
        } catch (const std::exception& e) {
            std::cout << "Warning: Invalid time value '" << szTimeValues << "' - " << e.what() << std::endl;
        }
    }
    
    return vFrameIndices;
}

/**
 * @brief Decode task specification for playlist mode
 */
struct DecodeTask {
    std::string inputFile;                  // Mandatory
    std::string outputFile;                 // Optional: if empty, skip output
    std::string statsFile;                  // Optional: if empty, skip stats
    std::vector<uint32_t> frameIndices;     // Optional: overrides -framelist and -f; if empty, uses -framelist or -f if available
};


/**
 * @brief Parse playlist file
 * Format:
 *   input:video1.mp4
 *   output:video1.yuv  (optional)
 *   stats:video1.stats (optional)
 *   indices:0,10,20,30  (optional, comma-separated frame indices or ranges)
 *                      Supports range syntax: 0:10:2 (every 2nd frame from 0 to 10)
 *                      Mixed: 0:10:2,20,30:35
 *   
 *   input:video2.mp4
 *   output:video2.yuv
 *   stats:video2.stats
 */
std::vector<DecodeTask> ParsePlaylistFile(const std::string& szFilename) {
    std::vector<DecodeTask> sources;
    std::ifstream file(szFilename);
    
    if (!file.is_open()) {
        std::cerr << "Error: Could not open playlist file: " << szFilename << std::endl;
        return sources;
    }
    
    DecodeTask currentSource;
    bool hasInput = false;
    int lineNumber = 0;
    std::string line;
    
    while (std::getline(file, line)) {
        lineNumber++;
        
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            // Empty line signals end of current source
            if (hasInput && line.empty()) {
                sources.push_back(currentSource);
                currentSource = DecodeTask();
                hasInput = false;
            }
            continue;
        }
        
        // Parse key:value
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            std::cerr << "Warning: Invalid line " << lineNumber << " (no colon): " << line << std::endl;
            continue;
        }
        
        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);
        
        // Trim key and value
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        if (key == "input") {
            if (hasInput) {
                sources.push_back(currentSource);
                currentSource = DecodeTask();
            }
            currentSource.inputFile = value;
            hasInput = true;
        } else if (key == "output") {
            if (hasInput) {
                currentSource.outputFile = value;
            } else {
                std::cerr << "Warning: 'output' specified before 'input' at line " << lineNumber << std::endl;
            }
        } else if (key == "stats") {
            if (hasInput) {
                currentSource.statsFile = value;
            } else {
                std::cerr << "Warning: 'stats' specified before 'input' at line " << lineNumber << std::endl;
            }
        } else if (key == "indices") {
            if (hasInput) {
                // Pass 0 for nTotalFrames to skip validation (will be validated later when decoding)
                currentSource.frameIndices = ParseFrameIndicesFromString(value, 0);
            } else {
                std::cerr << "Warning: 'indices' specified before 'input' at line " << lineNumber << std::endl;
            }
        }
    }
    
    // Add last source if exists
    if (hasInput) {
        sources.push_back(currentSource);
    }
    
    file.close();
    
    // Display parsed playlist
    if (!sources.empty()) {
        std::cout << "\nPlaylist parsed successfully: " << sources.size() << " video(s)" << std::endl;
        for (size_t i = 0; i < sources.size(); i++) {
            std::cout << "  [" << (i+1) << "] Input: " << sources[i].inputFile;
            if (!sources[i].outputFile.empty()) {
                std::cout << ", Output: " << sources[i].outputFile;
            }
            std::cout << std::endl;
        }
    }
    
    return sources;
}

/**
 * @brief Decode frames with stats dumping support
 */
void DecodeFramesWithStats(NvVideoDecoder& decoder, const StreamMetadata& metadata, 
                           const std::vector<uint32_t>& vFrameIndices,
                           const std::string& szOutputFile, const std::string& szStatsFile,
                           bool bOutputPlanar, uint32_t nBatchSize = 0, bool bVerbose = false) {
    std::ofstream fpOut, fpStats;
    bool bWriteOutput = !szOutputFile.empty();
    bool bDumpStats = !szStatsFile.empty();
    
    // Decoder info will be obtained after first frame decode
    NvDecoder* pNvDecoder = decoder.GetDecoder();
    size_t nFrameSize = 0;
    int nBitDepth = 8;
    int nWidth = 0, nHeight = 0;
    uint8_t nChromaFormat = 0;
    bool bSemiPlanar = false;
    std::vector<uint8_t> vHostFrame;
    bool bDecoderInitialized = false;
    
    if (bWriteOutput) {
        fpOut.open(szOutputFile, std::ios::out | std::ios::binary);
        if (!fpOut) {
            std::cerr << "Error: Unable to open output file: " << szOutputFile << std::endl;
            return;
        }
    }
    
    if (bDumpStats) {
        fpStats.open(szStatsFile, std::ios::out | std::ios::binary);
        if (!fpStats) {
            std::cerr << "Error: Unable to open stats file: " << szStatsFile << std::endl;
            if (fpOut.is_open()) fpOut.close();
            return;
        }
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Frame Processing" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    uint32_t nSuccessCount = 0;
    uint32_t nStatsFrameCount = 0;
    
    if (vFrameIndices.empty()) {
        // Decode entire stream
        uint32_t nNumFramesToDecode = metadata.numFrames;
        bool bDecodeUntilEOS = (nNumFramesToDecode == 0);
        
        if (bDecodeUntilEOS) {
            std::cout << "Decoding entire stream (unknown frame count - will decode until EOS)\n" << std::endl;
        } else {
            std::cout << "Decoding entire stream (" << nNumFramesToDecode << " frames)\n" << std::endl;
        }
        
        uint32_t i = 0;
        while (bDecodeUntilEOS || i < nNumFramesToDecode) {
            try {
                NvDecFrame frame = decoder[i];
                
                // Check for EOS
                if (bDecodeUntilEOS && frame.data == 0) {
                    std::cout << "\nReached end of stream at frame " << i << std::endl;
                    break;
                }
                
                // Initialize decoder info after first successful decode
                if (!bDecoderInitialized) {
                    InitializeDecoder(pNvDecoder, nFrameSize, nBitDepth, nWidth, nHeight, 
                                     nChromaFormat, bSemiPlanar, vHostFrame, bOutputPlanar);
                    bDecoderInitialized = true;
                }
                
                // Dump decode stats if enabled
                if (bDumpStats && frame.decodeStatsSize > 0 && frame.decodeStats) {
                    fpStats.write(reinterpret_cast<const char*>(frame.decodeStats), frame.decodeStatsSize);
                    nStatsFrameCount++;
                }
                
                // Process the frame
                ProcessFrame(frame, pNvDecoder, vHostFrame, nFrameSize, nWidth, nHeight,
                           nBitDepth, nChromaFormat, bSemiPlanar, bOutputPlanar, fpOut, bWriteOutput);
                
                nSuccessCount++;
                i++;
                
                // Progress indicator
                if (i % 100 == 0) {
                    std::cout << "Decoded " << i << " frames...\r" << std::flush;
                }
                
            } catch (const std::exception& e) {
                // Check if EOS was actually reached by querying the decoder
                bool bEOS = decoder.IsEOSReached();
                
                if (bDecodeUntilEOS && bEOS) {
                    std::cout << "\nReached end of stream at frame " << i << std::endl;
                    break;
                } else {
                    std::cerr << "Error decoding frame " << i << ": " << e.what() << std::endl;
                    if (bDecodeUntilEOS) {
                        break;
                    }
                    i++;
                }
            }
        }
        
    } else {
        // Decode specific frames
        if (nBatchSize > 0) {
            std::cout << "Decoding in batch mode: " << vFrameIndices.size() << " batches x " << nBatchSize 
                      << " frames = " << (vFrameIndices.size() * nBatchSize) << " total frames\n" << std::endl;
        } else {
            std::cout << "Decoding " << vFrameIndices.size() << " frames from indices file\n" << std::endl;
        }
        
        for (size_t i = 0; i < vFrameIndices.size(); i++) {
            uint32_t nStartIdx = vFrameIndices[i];
            std::vector<NvDecFrame> frames;
            std::vector<uint32_t> batchIndices;
            
            try {
                // Build batch indices or single frame
                bool bStopDecode = false;
                if (nBatchSize > 0) {
                    // Batch mode: create N sequential indices starting from nStartIdx
                    for (uint32_t j = 0; j < nBatchSize; j++) {
                        batchIndices.push_back(nStartIdx + j);
                    }
                    frames = decoder[batchIndices];  // Batch operator
                    
                    // Check if batch operator returned fewer frames than requested (may indicate EOS)
                    if (frames.size() < batchIndices.size() && decoder.IsEOSReached()) {
                        bStopDecode = true;  // Mark to stop after processing received frames
                    }
                } else {
                    // Single frame mode
                    NvDecFrame frame = decoder[nStartIdx];
                    frames.push_back(frame);
                    batchIndices.push_back(nStartIdx);
                }
                
                // Process all frames in the batch
                for (size_t idx = 0; idx < frames.size(); idx++) {
                    NvDecFrame& frame = frames[idx];
                    uint32_t nFrameIdx = batchIndices[idx];
                    
                    // Initialize decoder info after first successful decode
                    if (!bDecoderInitialized) {
                        InitializeDecoder(pNvDecoder, nFrameSize, nBitDepth, nWidth, nHeight, 
                                         nChromaFormat, bSemiPlanar, vHostFrame, bOutputPlanar);
                        bDecoderInitialized = true;
                    }
                    
                    // Dump decode stats if enabled
                    bool bStatsDumped = false;
                    if (bDumpStats && frame.decodeStatsSize > 0 && frame.decodeStats) {
                        fpStats.write(reinterpret_cast<const char*>(frame.decodeStats), frame.decodeStatsSize);
                        nStatsFrameCount++;
                        bStatsDumped = true;
                    }
                    
                    // Display batch info if in batch mode (verbose only)
                    if (bVerbose) {
                        if (nBatchSize > 0) {
                            std::cout << "Frame " << nFrameIdx << " [Batch " << (i+1) << "/" << vFrameIndices.size() 
                                      << ", Frame " << (idx+1) << "/" << nBatchSize << "]";
                        } else {
                            std::cout << "Frame " << nFrameIdx << " (" << (i+1) << "/" << vFrameIndices.size() << ")";
                        }
                        
                        // Print decoded frame information
                        if (frame.data != 0) {
                            std::cout << " [Decoded: " << nWidth << "x" << nHeight << ", " << nBitDepth << "-bit";
                            if (frame.isSkipped) {
                                std::cout << ", skipped";
                            }
                            if (frame.timestamp > 0) {
                                std::cout << ", PTS=" << frame.timestamp;
                            }
                            std::cout << "]";
                        }
                        
                        if (bStatsDumped) {
                            std::cout << " [Stats dumped]";
                        }
                        std::cout << std::endl;
                    }
                    
                    // Process the frame
                    ProcessFrame(frame, pNvDecoder, vHostFrame, nFrameSize, nWidth, nHeight,
                               nBitDepth, nChromaFormat, bSemiPlanar, bOutputPlanar, fpOut, bWriteOutput);
                    
                    nSuccessCount++;
                }
                
                // After processing frames, check if we should stop decoding
                if (bStopDecode) {
                    if (nSuccessCount > 0) {
                        uint32_t lastFrameIdx = frames.empty() ? nStartIdx : batchIndices[frames.size() - 1];
                        std::cout << "\nReached end of stream at frame " << lastFrameIdx << std::endl;
                        std::cout << "Stopping frame decoding (remaining " << (vFrameIndices.size() - i - 1) 
                                  << " batch(es) skipped)" << std::endl;
                    }
                    break;  // Stop processing remaining batches
                }
                
            } catch (const std::exception& e) {
                // Check if EOS was actually reached by querying the decoder
                bool bEOS = decoder.IsEOSReached();
                
                if (bEOS) {
                    // First EOS error - print message and stop processing remaining frames
                    if (nSuccessCount > 0) {
                        std::cout << "\nReached end of stream at frame " << nStartIdx << std::endl;
                        std::cout << "Stopping frame decoding (remaining " << (vFrameIndices.size() - i - 1) 
                                  << " frame(s) skipped)" << std::endl;
                    } else {
                        // EOS reached before any successful decode
                        std::cout << "\nReached end of stream (no frames decoded)" << std::endl;
                    }
                    break;  // Stop processing remaining frames
                } else {
                    // Non-EOS error - continue processing
                    if (nBatchSize > 0) {
                        std::cerr << "Error decoding batch starting at frame " << nStartIdx << ": " << e.what() << std::endl;
                    } else {
                        std::cerr << "Error decoding frame " << nStartIdx << ": " << e.what() << std::endl;
                    }
                }
            }
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Decoding Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Successfully decoded: " << nSuccessCount << " frames" << std::endl;
    if (bDumpStats) {
        std::cout << "Stats dumped: " << nStatsFrameCount << " frames" << std::endl;
    }
    std::cout << "Total time: " << duration.count() << " ms" << std::endl;
    if (nSuccessCount > 0) {
        std::cout << "Average: " << (duration.count() / static_cast<double>(nSuccessCount)) << " ms per frame" << std::endl;
        if (duration.count() > 0) {
            double dFps = (nSuccessCount * 1000.0) / duration.count();
            std::cout << "Throughput: " << dFps << " fps" << std::endl;
        }
    }
    std::cout << "========================================\n" << std::endl;
    
    if (fpOut.is_open()) {
        fpOut.close();
        std::cout << "Output written to: " << szOutputFile << std::endl;
    }
    
    if (fpStats.is_open()) {
        fpStats.close();
        std::cout << "Stats written to: " << szStatsFile << std::endl;
    }
}

/**
 * @brief Decode playlist using NvVideoDecoder's internal cache
 * 
 * NOTE: This function now uses a single NvVideoDecoder instance and calls
 * ReconfigureDecoder() for each video. The internal decoder cache
 * automatically reuses decoders based on codec, bitDepth, and chromaFormat.
 * This eliminates the need for app-level caching.
 */
void DecodePlaylist(const std::vector<DecodeTask>& sources, CUcontext cuContext,
                    bool bEnableScan, bool bOutputPlanar,
                    const std::string& szFrameFile,
                    const std::string& szFrameList,
                    const std::string& szTimeValues,
                    size_t nCacheSize = 0,
                    uint32_t nBatchSize = 0,
                    bool bEnableOpenGOP = false,
                    bool bEnablePTSFrameSkip = true,
                    bool bVerbose = false) 
{
    if (sources.empty()) {
        std::cerr << "Error: No video sources to process" << std::endl;
        return;
    }
    
    std::cout << "\nInternal Decoder Cache: " << (nCacheSize > 0 ? "Enabled" : "Disabled");
    if (nCacheSize > 0) {
        std::cout << " (max " << nCacheSize << " decoders)";
    }
    std::cout << std::endl;
    std::cout << "Note: Decoder reuse is managed internally by NvVideoDecoder" << std::endl;
    
    NvVideoDecoder* pDecoder = nullptr;
    
    for (size_t sourceIdx = 0; sourceIdx < sources.size(); sourceIdx++) {
        const DecodeTask& source = sources[sourceIdx];
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "Processing video " << (sourceIdx + 1) << "/" << sources.size() << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Input: " << source.inputFile << std::endl;
        if (!source.outputFile.empty()) {
            std::cout << "Output: " << source.outputFile << std::endl;
        }
        std::cout << "========================================\n" << std::endl;
        
        try {
            if (pDecoder == nullptr) {
                // Create decoder for first video
                std::cout << "  Creating NvVideoDecoder..." << std::endl;
                auto createStart = std::chrono::high_resolution_clock::now();
                
                // Enable stats if any video in playlist has stats file
                bool bEnableStats = false;
                for (const auto& src : sources) {
                    if (!src.statsFile.empty()) {
                        bEnableStats = true;
                        break;
                    }
                }
                
                pDecoder = new NvVideoDecoder(
                    source.inputFile,
                    static_cast<CUcontext>(cuContext),  // cudaContext
                    nullptr,                            // cudaStream (nullptr = use default)
                    true,                               // useDeviceMemory
                    0,                                  // maxWidth (0 = auto)
                    0,                                  // maxHeight (0 = auto)
                    bEnableScan,                        // needScannedStreamMetadata
                    nCacheSize,                         // decoderCacheSize (0 = disabled)
                    bEnableStats,                       // enableDecodeStats (from playlist)
                    bEnableOpenGOP                      // enableOpenGOP
                );
                
                auto createEnd = std::chrono::high_resolution_clock::now();
                auto createDuration = std::chrono::duration_cast<std::chrono::milliseconds>(createEnd - createStart);
                std::cout << "  Decoder creation time: " << createDuration.count() << " ms" << std::endl;
            } else {
                // Reconfigure decoder
                // Internal cache will automatically reuse decoders when possible
                std::cout << "  Reconfiguring decoder..." << std::endl;
                auto reconfigStart = std::chrono::high_resolution_clock::now();
                
                ReconfigureStatus status = pDecoder->ReconfigureDecoder(source.inputFile);
                
                auto reconfigEnd = std::chrono::high_resolution_clock::now();
                auto reconfigDuration = std::chrono::duration_cast<std::chrono::microseconds>(reconfigEnd - reconfigStart);
                std::cout << "  Reconfigure time: " << (reconfigDuration.count() / 1000.0) << " ms";
                if (status == ReconfigureStatus::ResetState) {
                    std::cout << " (RESET STATE - same source, state reset only)" << std::endl;
                } else if (status == ReconfigureStatus::CacheHit) {
                    std::cout << " (Decoder REUSED from cache)" << std::endl;
                } else {
                    std::cout << " (New decoder CREATED)" << std::endl;
                }
            }
            
            // Get stream metadata
            StreamMetadata metadata = pDecoder->GetStreamMetadata();
            int nBitDepth = pDecoder->GetBitDepth();
            cudaVideoChromaFormat chromaFormat = pDecoder->GetChromaFormat();
            
            // Enable or disable PTS-based frame skipping based on the flag and container name
            pDecoder->SetPTSFrameSkip(bEnablePTSFrameSkip && pDecoder->GetContainerName() != "matroska,webm");

            std::cout << std::endl;
            std::cout << "Stream Information:" << std::endl;
            std::cout << "------------------" << std::endl;
            std::cout << "  Resolution: " << metadata.width << "x" << metadata.height << std::endl;
            std::cout << "  Frame rate: " << metadata.averageFPS << " fps" << std::endl;
            double durationSec = (metadata.averageFPS > 0) ? (double)metadata.numFrames / metadata.averageFPS : 0.0;
            std::cout << "  Duration: " << durationSec << " seconds" << std::endl;
            std::cout << "  Total frames: " << metadata.numFrames << std::endl;
            std::cout << "  Codec: " << metadata.codecName << std::endl;
            std::cout << "  Bit depth: " << nBitDepth << "-bit" << std::endl;
            std::cout << "  Chroma format: " << ChromaFormatToString(chromaFormat) << std::endl;
            std::cout << "  Bitrate: " << (metadata.bitrate / 1000000.0) << " Mbps" << std::endl;
            std::cout << "  Container: " << pDecoder->GetContainerName() << std::endl;
            std::cout << "  Seekable: " << (pDecoder->IsSeekable() ? "Yes" : "No") << std::endl;
            std::cout << "  PTS-based frame skipping: " << (pDecoder->IsPTSFrameSkipEnabled() ? "Enabled" : "Disabled") << std::endl;
            
            if (bEnableScan && pDecoder->IsSeekable()) {
                try {
                    ScannedStreamMetadata scanned = pDecoder->GetScannedStreamMetadata();
                    
                    if (!scanned.keyFrameIndices.empty()) {
                        std::cout << "\nKeyframe/IDR Info:" << std::endl;
                        std::cout << "---------------------" << std::endl;
                        PrintKeyFrameIndices(scanned, bVerbose);
                        std::cout << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Warning: Failed to get keyframe/IDR Info: " << e.what() << std::endl;
                }
            }
            
            // Determine frame indices to use
            // Priority: 1. Playlist indices (source.frameIndices), 2. Command-line -framelist, 3. Command-line -f file, 4. -t time interval, 5. All frames
            std::vector<uint32_t> vFrameIndices;
            if (!source.frameIndices.empty()) {
                // Use indices from playlist (preferred)
                vFrameIndices = source.frameIndices;
                std::cout << "Using frame indices from playlist: " << vFrameIndices.size() << " frame(s)" << std::endl;
            } else if (!szFrameList.empty()) {
                // Fall back to command-line -framelist (takes priority over -f)
                vFrameIndices = ParseFrameIndicesFromString(szFrameList, metadata.numFrames);
                std::cout << "Using frame indices from -framelist: " << vFrameIndices.size() << " frame(s)" << std::endl;
            } else if (!szFrameFile.empty()) {
                // Fall back to command-line indices file
                vFrameIndices = ReadFrameIndicesFromFile(szFrameFile, metadata.numFrames);
                std::cout << "Using frame indices from -f file: " << vFrameIndices.size() << " frame(s)" << std::endl;
            } else if (!szTimeValues.empty()) {
                // Fall back to time values option (-t)
                vFrameIndices = ParseFrameIndicesFromTimeOption(pDecoder, szTimeValues, metadata.numFrames, metadata.averageFPS, bVerbose);
            }
            
            // Use stats file from playlist (ignore -dumpstats in playlist mode)
            DecodeFramesWithStats(*pDecoder, metadata, vFrameIndices, 
                                 source.outputFile, source.statsFile, bOutputPlanar, nBatchSize, bVerbose);
            
        } catch (const std::exception& e) {
            std::cerr << "Error processing video " << (sourceIdx + 1) << ": " << e.what() << std::endl;
            
            // Clean up on error and continue
            if (pDecoder) {
                delete pDecoder;
                pDecoder = nullptr;
            }
            continue;
        }
    }
    
    // Cleanup decoder
    if (pDecoder) {
        delete pDecoder;
        pDecoder = nullptr;
    }
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Playlist processing completed!" << std::endl;
    std::cout << "========================================" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return -1;
    }
    
    // Check for help flag first
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "-help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }
    
    // Initialize CUDA context variable
    CUcontext cuContext = nullptr;
    
    try {
        // Parse command line arguments
        std::string szInputFile = "";
        std::string szOutputFile = "";
        std::string szFrameFile = "";
        std::string szFrameList = "";
        std::string szStatsFile = "";
        std::string szPlaylistFile = "";
        std::string szTimeValues;  // Time values string for -t option (empty = disabled)
        int32_t iGpuId = 0;
        size_t nCacheSize = 0;  // Default cache size (0 = disabled)
        uint32_t nBatchSize = 0;  // Default batch size (0 = disabled)
        bool bEnableScan = true;
        bool bOutputPlanar = false;
        bool bEnableStats = false;
        bool bDumpStatsAuto = false;  // Auto-generate stats filename
        bool bEnableOpenGOP = false;
        bool bEnablePTSFrameSkip = true;
        bool bVerbose = false;
        
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "-i" && i + 1 < argc) {
                szInputFile = argv[++i];
            } else if (arg == "-o" && i + 1 < argc) {
                szOutputFile = argv[++i];
            } else if (arg == "-gpu" && i + 1 < argc) {
                iGpuId = std::atoi(argv[++i]);
                if (iGpuId < 0) {
                    std::cerr << "Warning: Invalid GPU ID " << iGpuId << ", using default (0)" << std::endl;
                    iGpuId = 0;
                }
            } else if (arg == "-f" && i + 1 < argc) {
                szFrameFile = argv[++i];
            } else if (arg == "-framelist" && i + 1 < argc) {
                szFrameList = argv[++i];
            } else if (arg == "-dumpstats") {
                bEnableStats = true;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    szStatsFile = argv[++i];
                } else {
                    bDumpStatsAuto = true;
                }
            } else if (arg == "-playlist" && i + 1 < argc) {
                szPlaylistFile = argv[++i];
            } else if (arg == "-cache" && i + 1 < argc) {
                int tempSize = std::atoi(argv[++i]);
                if (tempSize >= 0 && tempSize <= 100) {
                    nCacheSize = static_cast<size_t>(tempSize);
                } else {
                    std::cerr << "Warning: Invalid cache size " << tempSize << ", using default (0 = disabled)" << std::endl;
                }
            } else if (arg == "-outplanar") {
                bOutputPlanar = true;
            } else if (arg == "-opengop") {
                bEnableOpenGOP = true;
            } else if (arg == "-disablePTSFrameSkip") {
                bEnablePTSFrameSkip = false;
            } else if (arg == "-v") {
                bVerbose = true;
            } else if (arg == "-batch" && i + 1 < argc) {
                int tempSize = std::atoi(argv[++i]);
                if (tempSize > 0 && tempSize <= 64) {
                    nBatchSize = static_cast<uint32_t>(tempSize);
                } else {
                    std::cerr << "Warning: Invalid batch size " << tempSize << ", must be 1-64" << std::endl;
                }
            } else if (arg == "-t" && i + 1 < argc) {
                szTimeValues = argv[++i];
                if (szTimeValues.empty()) {
                    std::cerr << "Warning: Empty time values string for -t option" << std::endl;
                }
            }
        }

        // Determine mode: playlist or single video
        bool bPlaylistMode = !szPlaylistFile.empty();

        if (!bPlaylistMode && szInputFile.empty()) {
            std::cerr << "Error: Input file (-i) or playlist file (-playlist) is required" << std::endl;
            PrintUsage(argv[0]);
            return -1;
        }

        if (bPlaylistMode && !szInputFile.empty()) {
            std::cerr << "Error: Cannot specify both -i and -playlist" << std::endl;
            PrintUsage(argv[0]);
            return -1;
        }

        // Print configuration
        std::cout << "\n========================================" << std::endl;
        std::cout << "AppDecVideoDecoder" << std::endl;
        std::cout << "========================================" << std::endl;
        
        if (bPlaylistMode) {
            std::cout << "Mode: Playlist" << std::endl;
            std::cout << "Playlist file: " << szPlaylistFile << std::endl;
            std::cout << "Decoder caching: " << (nCacheSize > 0 ? "Enabled" : "Disabled");
            if (nCacheSize > 0) {
                std::cout << " (size: " << nCacheSize << ")";
            }
            std::cout << std::endl;
        } else {
            std::cout << "Mode: Single video" << std::endl;
            std::cout << "Input file: " << szInputFile << std::endl;
            std::cout << "Output file: " << szOutputFile << std::endl;
            if (!szFrameFile.empty()) {
                std::cout << "Frame indices file: " << szFrameFile << std::endl;
            }
        }
        
        std::cout << "GPU ID: " << iGpuId << std::endl;
        std::cout << "Output format: " << (bOutputPlanar ? "Planar (I420/YV12)" : "Semi-planar (NV12/P016)") << std::endl;
        std::cout << "Stream scan: " << (bEnableScan ? "Enabled" : "Disabled") << std::endl;
        std::cout << "Open GOP support: " << (bEnableOpenGOP ? "Enabled" : "Disabled") << std::endl;
        if (nBatchSize > 0) {
            std::cout << "Batch mode: Enabled (size=" << nBatchSize << " frames per index)" << std::endl;
        }
        
        if (bEnableStats) {
            if (bDumpStatsAuto) {
                std::cout << "Decode stats: Auto-dump enabled" << std::endl;
            } else if (!szStatsFile.empty()) {
                std::cout << "Decode stats: Dumping to " << szStatsFile << std::endl;
            }
        }
        std::cout << "========================================\n" << std::endl;
        
        // Initialize CUDA
        ck(cuInit(0));
        
        // Get CUDA device
        CUdevice cuDevice = 0;
        ck(cuDeviceGet(&cuDevice, iGpuId));
        
        // Create CUDA context
        ck(NVCODEC_CUDA_CTX_CREATE(&cuContext, 0, cuDevice));
        
        if (bPlaylistMode) {
            // ========================================
            // PLAYLIST MODE
            // ========================================
            std::vector<DecodeTask> sources = ParsePlaylistFile(szPlaylistFile);
            
            if (sources.empty()) {
                std::cerr << "Error: No valid sources in playlist file" << std::endl;
                cuCtxDestroy(cuContext);
                return -1;
            }
            
            // Note: -dumpstats flag is ignored in playlist mode
            // Stats are controlled per-video via 'stats:' lines in the playlist file
            if (bEnableStats) {
                std::cout << "Note: -dumpstats flag is ignored in playlist mode." << std::endl;
                std::cout << "      Use 'stats:<filename>' in the playlist file to dump stats per video." << std::endl;
            }
            
            DecodePlaylist(sources, cuContext, bEnableScan, bOutputPlanar, 
                          szFrameFile, szFrameList, szTimeValues, nCacheSize, nBatchSize, bEnableOpenGOP, bEnablePTSFrameSkip, bVerbose);
            
        } else {
            // ========================================
            // SINGLE VIDEO MODE (Basic functionality)
            // ========================================
            
            // Auto-generate stats filename if needed
            if (bEnableStats && bDumpStatsAuto) {
                szStatsFile = szOutputFile + ".stats";
            }
            
            // Create NvVideoDecoder
            auto decoderCreateStart = std::chrono::high_resolution_clock::now();
            NvVideoDecoder decoder(
                szInputFile,
                cuContext,                      // cudaContext (mandatory)
                nullptr,                        // cudaStream (nullptr = use default)
                true,                           // useDeviceMemory
                0,                              // maxWidth (0 = auto)
                0,                              // maxHeight (0 = auto)
                bEnableScan,                    // needScannedStreamMetadata
                nCacheSize,                     // decoderCacheSize (0 = disabled)
                bEnableStats,                   // enableDecodeStats
                bEnableOpenGOP                  // enableOpenGOP
            );
            auto decoderCreateEnd = std::chrono::high_resolution_clock::now();
            auto decoderCreateDuration = std::chrono::duration_cast<std::chrono::milliseconds>(decoderCreateEnd - decoderCreateStart);
            std::cout << "Decoder creation time: " << decoderCreateDuration.count() << " ms" << std::endl;
        
            
            // Get and display stream metadata
            StreamMetadata metadata = decoder.GetStreamMetadata();
            int nBitDepth = decoder.GetBitDepth();
            cudaVideoChromaFormat chromaFormat = decoder.GetChromaFormat();
            
            // Enable or disable PTS-based frame skipping based on the flag and container name
            decoder.SetPTSFrameSkip(bEnablePTSFrameSkip && decoder.GetContainerName() != "matroska,webm");

            std::cout << "Stream Information:" << std::endl;
            std::cout << "------------------" << std::endl;
            std::cout << "  Resolution: " << metadata.width << "x" << metadata.height << std::endl;
            std::cout << "  Frame rate: " << metadata.averageFPS << " fps" << std::endl;
            double durationSec = (metadata.averageFPS > 0) ? (double)metadata.numFrames / metadata.averageFPS : 0.0;
            std::cout << "  Duration: " << durationSec << " seconds" << std::endl;
            std::cout << "  Total frames: " << metadata.numFrames << std::endl;
            std::cout << "  Codec: " << metadata.codecName << std::endl;
            std::cout << "  Bit depth: " << nBitDepth << "-bit" << std::endl;
            std::cout << "  Chroma format: " << ChromaFormatToString(chromaFormat) << std::endl;
            std::cout << "  Bitrate: " << (metadata.bitrate / 1000000.0) << " Mbps" << std::endl;
            std::cout << "  Container: " << decoder.GetContainerName() << std::endl;
            std::cout << "  Seekable: " << (decoder.IsSeekable() ? "Yes" : "No") << std::endl;
            std::cout << "  PTS-based frame skipping: " << (decoder.IsPTSFrameSkipEnabled() ? "Enabled" : "Disabled") << std::endl;
            
            if (bEnableScan && decoder.IsSeekable()) {
                try {
                    std::cout << "\nScanning stream for keyframes..." << std::endl;
                    ScannedStreamMetadata scanned = decoder.GetScannedStreamMetadata();
                    
                    std::cout << "Scanned Stream Metadata:" << std::endl;
                    std::cout << "------------------------" << std::endl;
                    std::cout << "  Scanned frames: " << scanned.streamMetadata.numFrames << std::endl;
                    PrintKeyFrameIndices(scanned, bVerbose);
                    std::cout << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "Warning: Failed to get scanned metadata: " << e.what() << std::endl;
                }
            }
            
            // Load frame indices from framelist, file, or time interval
            // Priority: 1. -framelist, 2. -f file, 3. -t time interval, 4. All frames
            std::vector<uint32_t> vFrameIndices;
            if (!szFrameList.empty()) {
                // -framelist takes priority over -f
                vFrameIndices = ParseFrameIndicesFromString(szFrameList, metadata.numFrames);
                std::cout << "Using frame indices from -framelist: " << vFrameIndices.size() << " frame(s)" << std::endl;
            } else if (!szFrameFile.empty()) {
                vFrameIndices = ReadFrameIndicesFromFile(szFrameFile, metadata.numFrames);
                std::cout << "Using frame indices from -f file: " << vFrameIndices.size() << " frame(s)" << std::endl;
            } else if (!szTimeValues.empty()) {
                // Use time values option (-t)
                vFrameIndices = ParseFrameIndicesFromTimeOption(&decoder, szTimeValues, metadata.numFrames, metadata.averageFPS, bVerbose);
            }
            
            // Decode frames (with or without stats)
            if (bEnableStats) {
                DecodeFramesWithStats(decoder, metadata, vFrameIndices, szOutputFile, 
                                     szStatsFile, bOutputPlanar, nBatchSize, bVerbose);
            } else {
                DecodeFrames(decoder, metadata, vFrameIndices, szOutputFile, bOutputPlanar, nBatchSize, bVerbose);
            }
        }
        
        // Success message
        std::cout << "\nDecoding completed successfully!" << std::endl;
        
        // Cleanup CUDA context
        if (cuContext) {
            cuCtxDestroy(cuContext);
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        
        // Cleanup CUDA context on error
        if (cuContext) {
            cuCtxDestroy(cuContext);
        }
        
        return -1;
    }
}

