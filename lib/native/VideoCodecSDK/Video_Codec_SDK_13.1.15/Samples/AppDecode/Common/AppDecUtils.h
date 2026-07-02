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
//! \file AppDecUtils.h
//! \brief Header file containing definitions of miscellaneous functions used by Decode samples
//---------------------------------------------------------------------------

#pragma once
#include <sstream>
#include <iostream>
#include <algorithm>

static void ShowDecoderCapability();
static void ShowDecoderCapabilitySummary();
static void ShowDecoderCapabilityDetailed();
static void getOutputFormatNames(unsigned short nOutputFormatMask, char *OutputFormats);
static void createCudaContext(CUcontext* cuContext, int iGpu, unsigned int flags);

static void ShowBriefHelp(char *szOutputFileName, bool *pbVerbose, int *piD3d, bool *pbForce_zero_latency)
{
    std::ostringstream oss;
    oss << "NVIDIA Video Decoder Sample Application\n";
    oss << "====================================\n\n";
    
    oss << "Usage: AppDec -i <input_file> [options]\n\n";

    // Brief table of core arguments
    oss << "Common Arguments:\n";
    oss << std::left << std::setw(25) << "Argument" 
        << std::setw(12) << "Type"
        << "Default Value\n";
    oss << std::string(50, '-') << "\n";

    oss << std::left << std::setw(25) << "-i <path>" 
        << std::setw(12) << "Required"
        << "N/A\n";

    if (szOutputFileName) {
        oss << std::left << std::setw(25) << "-o <path>" 
            << std::setw(12) << "Optional"
            << "out.native/.planar\n";
    }

    oss << std::left << std::setw(25) << "-gpu <n>" 
        << std::setw(12) << "Optional"
        << "0\n";

    if (pbVerbose) {
        oss << std::left << std::setw(25) << "-v" 
            << std::setw(12) << "Optional"
            << "false\n";
    }

    if (piD3d) {
        oss << std::left << std::setw(25) << "-d3d <n>" 
            << std::setw(12) << "Optional"
            << "9\n";
    }

    if (pbForce_zero_latency) {
        oss << std::left << std::setw(25) << "-force_zero_latency" 
            << std::setw(12) << "Optional"
            << "false\n";
    }

    oss << "\nFor detailed help, use -A/--advanced-options\n";
    oss << "To view decode capabilities:\n";
    oss << "  -dc        : Print decoder capabilities summary\n";
    oss << "  -dc-detail : Print detailed decoder capabilities\n";
    std::cout << oss.str();
    exit(0);
}

static void ShowDetailedHelp(char *szOutputFileName, bool *pbVerbose, int *piD3d, bool *pbForce_zero_latency)
{
    std::ostringstream oss;
    oss << "NVIDIA Video Decoder Sample Application - Detailed Help\n";
    oss << "================================================\n\n";
    
    oss << "Usage: AppDec -i <input_file> [options]\n\n";

    // Full table of all arguments
    oss << "All Arguments:\n";
    oss << std::left << std::setw(25) << "Argument" 
        << std::setw(12) << "Type"
        << std::setw(20) << "Default Value"
        << "Example\n";
    oss << std::string(80, '-') << "\n";

    // Required arguments
    oss << std::left << std::setw(25) << "-i <path>" 
        << std::setw(12) << "Required"
        << std::setw(20) << "N/A"
        << "-i input.h264\n";

    // Optional arguments
    if (szOutputFileName) {
        oss << std::left << std::setw(25) << "-o <path>" 
            << std::setw(12) << "Optional"
            << std::setw(20) << "out.native/.planar"
            << "-o output.yuv\n";
    }

    oss << std::left << std::setw(25) << "-gpu <n>" 
        << std::setw(12) << "Optional"
        << std::setw(20) << "0"
        << "-gpu 1\n";

    if (pbVerbose) {
        oss << std::left << std::setw(25) << "-v" 
            << std::setw(12) << "Optional"
            << std::setw(20) << "false"
            << "-v\n";
    }

    if (piD3d) {
        oss << std::left << std::setw(25) << "-d3d <n>" 
            << std::setw(12) << "Optional"
            << std::setw(20) << "9"
            << "-d3d 11\n";
    }

    if (pbForce_zero_latency) {
        oss << std::left << std::setw(25) << "-force_zero_latency" 
            << std::setw(12) << "Optional"
            << std::setw(20) << "false"
            << "-force_zero_latency\n";
    }

    // Detailed descriptions
    oss << "\nDetailed Descriptions:\n";
    oss << "-------------------\n";
    oss << std::left << std::setw(25) << "-i" << ": Input file path\n";
    if (szOutputFileName) {
        oss << std::left << std::setw(25) << "-o" << ": Output file path\n";
    }
    oss << std::left << std::setw(25) << "-gpu" << ": Ordinal of GPU to use\n";
    if (pbVerbose) {
        oss << std::left << std::setw(25) << "-v" << ": Enable verbose message output\n";
    }
    if (piD3d) {
        oss << std::left << std::setw(25) << "-d3d" << ": DirectX version to use (9 or 11)\n";
    }
    if (pbForce_zero_latency) {
        oss << std::left << std::setw(25) << "-force_zero_latency" << ": Enable zero latency for All-Intra/IPPP streams\n";
    }
    oss << std::left << std::setw(25) << "-h/--help" << ": Print usage information for common commandline options\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print usage information for common and advanced commandline options\n";
    oss << std::left << std::setw(25) << "-dc" << ": Print decoder capabilities summary\n";
    oss << std::left << std::setw(25) << "-dc-detail" << ": Print detailed decoder capabilities\n";

    // Important notes
    oss << "\nNotes:\n";
    oss << "------\n";
    if (pbForce_zero_latency) {
        oss << "* Do not use -force_zero_latency if the stream contains B-frames\n";
    }
    if (piD3d) {
        oss << "* D3D version 9 is used by default\n";
    }
    oss << std::endl;
    oss << "To view decode capabilities:\n";
    oss << "  -dc        : Print decoder capabilities summary\n";
    oss << "  -dc-detail : Print detailed decoder capabilities\n";

    std::cout << oss.str();
    exit(0);
}

static void ShowHelpAndExit(const char *szBadOption, char *szOutputFileName, bool *pbVerbose, int *piD3d, bool *pbForce_zero_latency)
{
    if (szBadOption) 
    {
        std::ostringstream oss;
        oss << "Error parsing \"" << szBadOption << "\"\n";
        oss << "Use -h/--help for basic usage or -A/--advanced-options for detailed information\n";
        throw std::invalid_argument(oss.str());
    }
}

static void ParseCommandLine(int argc, char *argv[], char *szInputFileName,
    char *szOutputFileName, int &iGpu, bool *pbVerbose = NULL, int *piD3d = NULL,
    bool *pbForce_zero_latency = NULL)
{
    std::ostringstream oss;
    if (argc == 1) {
        std::cout << "No Arguments provided! Please refer to the following for options:" << "\"\n";
        ShowBriefHelp(szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
    }
    int i;
    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help")) {
            ShowBriefHelp(szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
        }
        if (!_stricmp(argv[i], "-A") || !_stricmp(argv[i], "--advanced-options")) {
            ShowDetailedHelp(szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
        }
        if (!_stricmp(argv[i], "-dc-detail")) {
            ShowDecoderCapabilityDetailed();
        }
        if (!_stricmp(argv[i], "-dc")) {
            ShowDecoderCapability();
        }
        if (!_stricmp(argv[i], "-i")) {
            if (++i == argc) {
                ShowHelpAndExit("-i", szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
            }
            sprintf(szInputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-o")) {
            if (++i == argc || !szOutputFileName) {
                ShowHelpAndExit("-o", szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
            }
            sprintf(szOutputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-gpu")) {
            if (++i == argc) {
                ShowHelpAndExit("-gpu", szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
            }
            iGpu = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-v")) {
            if (!pbVerbose) {
                ShowHelpAndExit("-v", szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
            }
            *pbVerbose = true;
            continue;
        }
        if (!_stricmp(argv[i], "-d3d")) {
            if (++i == argc || !piD3d) {
                ShowHelpAndExit("-d3d", szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
            }
            *piD3d = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-force_zero_latency")) {
            if (!pbForce_zero_latency) {
                ShowHelpAndExit("-force_zero_latency", szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
            }
            *pbForce_zero_latency = true;
            continue;
        }
        ShowHelpAndExit(argv[i], szOutputFileName, pbVerbose, piD3d, pbForce_zero_latency);
    }
}

// Generate space-separated list of supported surface formats
static void getOutputFormatNames(unsigned short nOutputFormatMask, char *OutputFormats)
{
    if (nOutputFormatMask == 0) {
        strcpy(OutputFormats, "N/A");
        return;
    }

    if (nOutputFormatMask & (1U << cudaVideoSurfaceFormat_NV12)) {
        strcat(OutputFormats, "NV12 ");
    }

    if (nOutputFormatMask & (1U << cudaVideoSurfaceFormat_P016)) {
        strcat(OutputFormats, "P016 ");
    }

    if (nOutputFormatMask & (1U << cudaVideoSurfaceFormat_YUV444)) {
        strcat(OutputFormats, "YUV444 ");
    }

    if (nOutputFormatMask & (1U << cudaVideoSurfaceFormat_YUV444_16Bit)) {
        strcat(OutputFormats, "YUV444_16Bit ");
    }

    if (nOutputFormatMask & (1U << cudaVideoSurfaceFormat_NV16)) {
        strcat(OutputFormats, "NV16 ");
    }

    if (nOutputFormatMask & (1U << cudaVideoSurfaceFormat_P216)) {
        strcat(OutputFormats, "P216 ");
    }
    return;
}

// Create CUDA context
static void createCudaContext(CUcontext* cuContext, int iGpu, unsigned int flags)
{
    CUdevice cuDevice = 0;
    ck(cuDeviceGet(&cuDevice, iGpu));
    char szDeviceName[80];
    ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
    std::cout << "GPU in use: " << szDeviceName << std::endl;
    ck(NVCODEC_CUDA_CTX_CREATE(cuContext, flags, cuDevice));
}

// Print decoder capabilities summary
static void ShowDecoderCapabilitySummary()
{
    ck(cuInit(0));
    int nGpu = 0;
    ck(cuDeviceGetCount(&nGpu));
    
    for (int iGpu = 0; iGpu < nGpu; iGpu++) {
        CUcontext cuContext = NULL;
        createCudaContext(&cuContext, iGpu, 0);
        
        char deviceName[256];
        CUdevice cuDevice;
        ck(cuCtxGetDevice(&cuDevice));
        ck(cuDeviceGetName(deviceName, sizeof(deviceName), cuDevice));
        
        std::cout << "GPU Decoder Capabilities" << std::endl;
        std::cout << "GPU " << iGpu << " - " << deviceName << std::endl << std::endl;
        
        // Get hardware decoder count
        CUVIDDECODECAPS hwCaps = {};
        hwCaps.eCodecType = cudaVideoCodec_H264;
        hwCaps.eChromaFormat = cudaVideoChromaFormat_420;
        hwCaps.nBitDepthMinus8 = 0;
        cuvidGetDecoderCaps(&hwCaps);
        
        // Summary table for major codecs
        std::cout << "Codec Support Summary" << std::endl;
        std::cout << std::left << std::setw(12) << "Codec"
                  << std::setw(10) << "8-bit"
                  << std::setw(10) << "10-bit"
                  << std::setw(10) << "12-bit"
                  << std::setw(15) << "Max Resolution"
                  << std::setw(12) << "Engines" << std::endl;
        std::cout << std::string(79, '-') << std::endl;
        
        struct codecInfo {
            const char* name;
            cudaVideoCodec codec;
        };
        
        codecInfo majorCodecs[] = {
            {"H264", cudaVideoCodec_H264},
            {"HEVC", cudaVideoCodec_HEVC},
            {"VP8", cudaVideoCodec_VP8},
            {"VP9", cudaVideoCodec_VP9},
            {"AV1", cudaVideoCodec_AV1},
            {"MPEG2", cudaVideoCodec_MPEG2},
            {"VC1", cudaVideoCodec_VC1},
            {"JPEG", cudaVideoCodec_JPEG}
        };
        
        for (auto& codec : majorCodecs) {
            CUVIDDECODECAPS caps8 = {}, caps10 = {}, caps12 = {};
            
            // 8-bit
            caps8.eCodecType = codec.codec;
            caps8.eChromaFormat = cudaVideoChromaFormat_420;
            caps8.nBitDepthMinus8 = 0;
            cuvidGetDecoderCaps(&caps8);
            
            // 10-bit
            caps10.eCodecType = codec.codec;
            caps10.eChromaFormat = cudaVideoChromaFormat_420;
            caps10.nBitDepthMinus8 = 2;
            cuvidGetDecoderCaps(&caps10);
            
            // 12-bit
            caps12.eCodecType = codec.codec;
            caps12.eChromaFormat = cudaVideoChromaFormat_420;
            caps12.nBitDepthMinus8 = 4;
            cuvidGetDecoderCaps(&caps12);
            
            std::cout << std::left << std::setw(12) << codec.name
                      << std::setw(10) << (caps8.bIsSupported ? "yes" : "no")
                      << std::setw(10) << (caps10.bIsSupported ? "yes" : "no")
                      << std::setw(10) << (caps12.bIsSupported ? "yes" : "no");
            
            // Show resolution and engines from the best supported configuration
            if (caps8.bIsSupported || caps10.bIsSupported || caps12.bIsSupported) {
                // Use the highest resolution and engine count available
                unsigned int maxWidth = 0, maxHeight = 0, maxEngines = 0;
                if (caps8.bIsSupported) {
                    if (caps8.nMaxWidth > maxWidth) maxWidth = caps8.nMaxWidth;
                    if (caps8.nMaxHeight > maxHeight) maxHeight = caps8.nMaxHeight;
                    if ((unsigned int)caps8.nNumNVDECs > maxEngines) maxEngines = (unsigned int)caps8.nNumNVDECs;
                }
                if (caps10.bIsSupported) {
                    if (caps10.nMaxWidth > maxWidth) maxWidth = caps10.nMaxWidth;
                    if (caps10.nMaxHeight > maxHeight) maxHeight = caps10.nMaxHeight;
                    if ((unsigned int)caps10.nNumNVDECs > maxEngines) maxEngines = (unsigned int)caps10.nNumNVDECs;
                }
                if (caps12.bIsSupported) {
                    if (caps12.nMaxWidth > maxWidth) maxWidth = caps12.nMaxWidth;
                    if (caps12.nMaxHeight > maxHeight) maxHeight = caps12.nMaxHeight;
                    if ((unsigned int)caps12.nNumNVDECs > maxEngines) maxEngines = (unsigned int)caps12.nNumNVDECs;
                }
                std::cout << std::setw(15) << (std::to_string(maxWidth) + "*" + std::to_string(maxHeight))
                          << std::setw(12) << maxEngines;
            } else {
                std::cout << std::setw(15) << "N/A" << std::setw(12) << "N/A";
            }
            std::cout << std::endl;
        }
        
        ck(cuCtxDestroy(cuContext));
    }
    
    std::cout << "\nFor detailed information about all capabilities, use -dc-detail" << std::endl;
    exit(0);
}

// Print detailed decoder capabilities
static void ShowDecoderCapabilityDetailed()
{
    ck(cuInit(0));
    int nGpu = 0;
    ck(cuDeviceGetCount(&nGpu));
    
    struct caps {
        const char *aszCodecName;
        cudaVideoCodec aeCodec;
        cudaVideoChromaFormat aeChromaFormat;
        int anBitDepthMinus8;
    };
    
    caps queryList[] = {
        {"H264",     cudaVideoCodec_H264,     cudaVideoChromaFormat_420,          0},
        {"H264",     cudaVideoCodec_H264,     cudaVideoChromaFormat_420,          2},
        {"H264",     cudaVideoCodec_H264,     cudaVideoChromaFormat_422,          0},
        {"H264",     cudaVideoCodec_H264,     cudaVideoChromaFormat_422,          2},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_420,          0},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_420,          2},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_420,          4},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_422,          0},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_422,          2},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_422,          4},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_444,          0},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_444,          2},
        {"HEVC",     cudaVideoCodec_HEVC,     cudaVideoChromaFormat_444,          4},
        {"VP8",      cudaVideoCodec_VP8,      cudaVideoChromaFormat_420,          0},
        {"VP9",      cudaVideoCodec_VP9,      cudaVideoChromaFormat_420,          0},
        {"VP9",      cudaVideoCodec_VP9,      cudaVideoChromaFormat_420,          2},
        {"VP9",      cudaVideoCodec_VP9,      cudaVideoChromaFormat_420,          4},
        {"AV1",      cudaVideoCodec_AV1,      cudaVideoChromaFormat_420,          0},
        {"AV1",      cudaVideoCodec_AV1,      cudaVideoChromaFormat_420,          2},
        {"AV1",      cudaVideoCodec_AV1,      cudaVideoChromaFormat_Monochrome,   0},
        {"AV1",      cudaVideoCodec_AV1,      cudaVideoChromaFormat_Monochrome,   2},
        {"JPEG",     cudaVideoCodec_JPEG,     cudaVideoChromaFormat_420,          0},
        {"MPEG1",    cudaVideoCodec_MPEG1,    cudaVideoChromaFormat_420,          0},
        {"MPEG2",    cudaVideoCodec_MPEG2,    cudaVideoChromaFormat_420,          0},
        {"MPEG4",    cudaVideoCodec_MPEG4,    cudaVideoChromaFormat_420,          0},
        {"VC1",      cudaVideoCodec_VC1,      cudaVideoChromaFormat_420,          0},
    };

    const char *aszChromaFormat[] = { "4:0:0", "4:2:0", "4:2:2", "4:4:4" };
    char strOutputFormats[64];

    for (int iGpu = 0; iGpu < nGpu; iGpu++) {
        CUcontext cuContext = NULL;
        createCudaContext(&cuContext, iGpu, 0);
        
        char deviceName[256];
        CUdevice cuDevice;
        ck(cuCtxGetDevice(&cuDevice));
        ck(cuDeviceGetName(deviceName, sizeof(deviceName), cuDevice));
        
        std::cout << "GPU Decoder Capabilities" << std::endl;
        std::cout << "GPU " << iGpu << " - " << deviceName << std::endl << std::endl;

        std::cout << "Detailed Codec Capabilities" << std::endl;

        for (int i = 0; i < sizeof(queryList) / sizeof(queryList[0]); i++) {
            CUVIDDECODECAPS decodeCaps = {};
            decodeCaps.eCodecType = queryList[i].aeCodec;
            decodeCaps.eChromaFormat = queryList[i].aeChromaFormat;
            decodeCaps.nBitDepthMinus8 = queryList[i].anBitDepthMinus8;

            cuvidGetDecoderCaps(&decodeCaps);

            if (!decodeCaps.bIsSupported) continue;

            strOutputFormats[0] = '\0';
            getOutputFormatNames(decodeCaps.nOutputFormatMask, strOutputFormats);

            std::cout << "======== " << queryList[i].aszCodecName << "_" 
                      << (decodeCaps.nBitDepthMinus8 + 8) << "bit_" 
                      << aszChromaFormat[decodeCaps.eChromaFormat] << " DECODER CAPABILITIES ========" << std::endl;
            
            std::cout << "Hardware:" << std::endl;
            std::cout << "  Num_Decoder_Engines: " << (int)decodeCaps.nNumNVDECs << std::endl;
            std::cout << "  Codec_Support: yes" << std::endl;
            std::cout << "  Max_MB_Count: " << decodeCaps.nMaxMBCount << std::endl;
            
            std::cout << "Resolution:" << std::endl;
            std::cout << "  Max_Resolution: " << decodeCaps.nMaxWidth << "*" << decodeCaps.nMaxHeight << std::endl;
            std::cout << "  Min_Resolution: " << decodeCaps.nMinWidth << "*" << decodeCaps.nMinHeight << std::endl;
            
            std::cout << "Histogram:" << std::endl;
            std::cout << "  Histogram_Supported: " << (decodeCaps.bIsHistogramSupported ? "yes" : "no") << std::endl;
            if (decodeCaps.bIsHistogramSupported) {
                std::cout << "  Histogram_Counter_BitDepth: " << (decodeCaps.nCounterBitDepth > 0 ? std::to_string(decodeCaps.nCounterBitDepth) + " bits" : "N/A") << std::endl;
                std::cout << "  Max_Histogram_Bins: " << decodeCaps.nMaxHistogramBins << std::endl;
            } else {
                std::cout << "  Histogram_Counter_BitDepth: N/A" << std::endl;
                std::cout << "  Max_Histogram_Bins: N/A" << std::endl;
            }
            
            std::cout << "Statistics:" << std::endl;
            std::cout << "  Decode_Stats_Supported: " << (decodeCaps.bIsDecodeStatsSupported ? "yes" : "no") << std::endl;
            
            std::cout << "Output Formats:" << std::endl;
            std::cout << "  Supported_Output_Formats: " << strOutputFormats << std::endl;
            std::cout << std::endl;
        }

        std::cout << "Surface Format Legend" << std::endl;
        std::cout << "NV12     : Semi-Planar YUV [Y plane followed by interleaved UV plane]" << std::endl;
        std::cout << "P016     : 16 bit Semi-Planar YUV [Y plane followed by interleaved UV plane]." << std::endl;
        std::cout << "           Can be used for 10 bit(6LSB bits 0), 12 bit (4LSB bits 0)" << std::endl;
        std::cout << "YUV444   : Planar YUV [Y plane followed by U and V planes]" << std::endl;
        std::cout << "YUV444_16Bit: 16 bit Planar YUV [Y plane followed by U and V planes]." << std::endl;
        std::cout << "              Can be used for 10 bit(6LSB bits 0), 12 bit (4LSB bits 0)" << std::endl;
        std::cout << "NV16     : Semi-Planar YUV 422 [Y plane followed by interleaved UV plane]" << std::endl;
        std::cout << "P216     : 16 bit Semi-Planar YUV 422[Y plane followed by interleaved UV plane]." << std::endl;
        std::cout << "           Can be used for 10 bit(6LSB bits 0), 12 bit (4LSB bits 0)" << std::endl;
        std::cout << std::endl;

        ck(cuCtxDestroy(cuContext));
    }
    exit(0);
}

// Show decoder capabilities
static void ShowDecoderCapability()
{
    ShowDecoderCapabilitySummary();
    std::cout << "\nFor detailed information about all capabilities, use -dc-detail" << std::endl;
    exit(0);
}
