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

#pragma once
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include "NvEncoder/NvEncoder.h"
#include "../Utils/NvEncoderCLIOptions.h"
#include "NvEncoder/NvEncoderCuda.h"
#include "../Utils/NvCodecUtils.h"

// Ensure NVCODEC_CUDA_CTX_CREATE is available if not already defined
#ifndef NVCODEC_CUDA_CTX_CREATE
#if CUDA_VERSION >= 13000
    #define NVCODEC_CUDA_CTX_CREATE(pctx, flags, dev) \
        cuCtxCreate_v4(pctx, 0, flags, dev)
#else
    #define NVCODEC_CUDA_CTX_CREATE(pctx, flags, dev) \
        cuCtxCreate_v2(pctx, flags, dev)
#endif
#endif

// Summary-only function (table + legend only)
inline void ShowEncoderCapabilitySummary()
{
    ck(cuInit(0));
    int nGpu = 0;
    ck(cuDeviceGetCount(&nGpu));
    std::cout << "Encoder Capability Summary" << std::endl;
    for (int iGpu = 0; iGpu < nGpu; iGpu++) {
        CUdevice cuDevice = 0;
        ck(cuDeviceGet(&cuDevice, iGpu));
        char szDeviceName[80];
        ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
        CUcontext cuContext = NULL;
        ck(NVCODEC_CUDA_CTX_CREATE(&cuContext, 0, cuDevice));
        NvEncoderCuda enc(cuContext, 1280, 720, NV_ENC_BUFFER_FORMAT_NV12);

        std::cout << "GPU " << iGpu << " - " << szDeviceName << std::endl;

        // ====== CODEC SUPPORT SUMMARY ======
        std::cout << std::endl << "=== CODEC SUPPORT SUMMARY ===" << std::endl;
        std::cout << "\tH264:\t\t\t" << (enc.GetCapabilityValue(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) ? "yes" : "no") << std::endl;
        std::cout << "\tHEVC:\t\t\t" << (enc.GetCapabilityValue(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) ? "yes" : "no") << std::endl;
        std::cout << "\tAV1:\t\t\t" << (enc.GetCapabilityValue(NV_ENC_CODEC_AV1_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) ? "yes" : "no") << std::endl;

        // ====== ENCODER CAPABILITIES SUMMARY TABLE ======
        std::cout << std::endl << "=== ENCODER CAPABILITIES SUMMARY TABLE ===" << std::endl;
        std::cout << std::endl;

        // Table header
        std::cout << std::left
                  << std::setw(8) << "Codec"
                  << std::setw(8) << "Engines"
                  << std::setw(12) << "MaxRes"
                  << std::setw(10) << "MinRes"
                  << std::setw(8) << "MaxB"
                  << std::setw(8) << "TempL"
                  << std::setw(8) << "LTR"
                  << std::setw(8) << "YUV444"
                  << std::setw(8) << "YUV422"
                  << std::setw(8) << "10bit"
                  << std::setw(10) << "Lossless"
                  << std::setw(8) << "SAO" << std::endl;
        std::cout << std::string(98, '-') << std::endl;

        // Define major codecs to query
        struct { const char* name; GUID guid; } codecs[] = {
            {"H264", NV_ENC_CODEC_H264_GUID},
            {"HEVC", NV_ENC_CODEC_HEVC_GUID},
            {"AV1", NV_ENC_CODEC_AV1_GUID}
        };

        for (int i = 0; i < sizeof(codecs)/sizeof(codecs[0]); i++) {
            if (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES)) {

                // Format resolution strings
                std::string maxRes = std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_WIDTH_MAX)) + "*" +
                                   std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_HEIGHT_MAX));
                std::string minRes = std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_WIDTH_MIN)) + "*" +
                                   std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_HEIGHT_MIN));

                // Get key capabilities
                int engines = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_ENCODER_ENGINES);
                int maxBFrames = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                int tempLayers = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS);
                int ltrFrames = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_MAX_LTR_FRAMES);
                bool yuv444 = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
                bool yuv422 = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_YUV422_ENCODE);
                bool tenBit = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
                bool lossless = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE);

                // SAO support (HEVC-specific)
                std::string saoSupport = "N/A";
                if (strcmp(codecs[i].name, "HEVC") == 0) {
                    saoSupport = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_SAO) ? "yes" : "no";
                }


                // Print table row
                std::cout << std::left
                          << std::setw(8) << codecs[i].name
                          << std::setw(8) << engines
                          << std::setw(12) << maxRes
                          << std::setw(10) << minRes
                          << std::setw(8) << maxBFrames
                          << std::setw(8) << tempLayers
                          << std::setw(8) << ltrFrames
                          << std::setw(8) << (yuv444 ? "yes" : "no")
                          << std::setw(8) << (yuv422 ? "yes" : "no")
                          << std::setw(8) << (tenBit ? "yes" : "no")
                          << std::setw(10) << (lossless ? "yes" : "no")
                          << std::setw(8) << saoSupport << std::endl;
            }
        }

        std::cout << std::endl << "Legend:" << std::endl;
        std::cout << "  Engines   = Number of hardware encoder engines" << std::endl;
        std::cout << "  MaxB      = Maximum B-frames supported" << std::endl;
        std::cout << "  TempL     = Maximum temporal layers" << std::endl;
        std::cout << "  LTR       = Maximum long-term reference frames" << std::endl;
        std::cout << "  SAO       = Sample Adaptive Offset (HEVC-specific)" << std::endl;

        std::cout << std::endl;
        enc.DestroyEncoder();
        ck(cuCtxDestroy(cuContext));
    }
}

// Detailed function (table + comprehensive details)
inline void ShowEncoderCapabilityDetailed()
{
    ck(cuInit(0));
    int nGpu = 0;
    ck(cuDeviceGetCount(&nGpu));
    std::cout << "Encoder Capability (Detailed)" << std::endl;
    for (int iGpu = 0; iGpu < nGpu; iGpu++) {
        CUdevice cuDevice = 0;
        ck(cuDeviceGet(&cuDevice, iGpu));
        char szDeviceName[80];
        ck(cuDeviceGetName(szDeviceName, sizeof(szDeviceName), cuDevice));
        CUcontext cuContext = NULL;
        ck(NVCODEC_CUDA_CTX_CREATE(&cuContext, 0, cuDevice));
        NvEncoderCuda enc(cuContext, 1280, 720, NV_ENC_BUFFER_FORMAT_NV12);

        std::cout << "GPU " << iGpu << " - " << szDeviceName << std::endl;

        // ====== CODEC SUPPORT SUMMARY ======
        std::cout << std::endl << "=== CODEC SUPPORT SUMMARY ===" << std::endl;
        std::cout << "\tH264:\t\t\t" << (enc.GetCapabilityValue(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) ? "yes" : "no") << std::endl;
        std::cout << "\tHEVC:\t\t\t" << (enc.GetCapabilityValue(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) ? "yes" : "no") << std::endl;
        std::cout << "\tAV1:\t\t\t" << (enc.GetCapabilityValue(NV_ENC_CODEC_AV1_GUID, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES) ? "yes" : "no") << std::endl;

        // ====== ENCODER CAPABILITIES SUMMARY TABLE ======
        std::cout << std::endl << "=== ENCODER CAPABILITIES SUMMARY TABLE ===" << std::endl;
        std::cout << std::endl;

        // Table header
        std::cout << std::left
                  << std::setw(8) << "Codec"
                  << std::setw(8) << "Engines"
                  << std::setw(12) << "MaxRes"
                  << std::setw(10) << "MinRes"
                  << std::setw(8) << "MaxB"
                  << std::setw(8) << "TempL"
                  << std::setw(8) << "LTR"
                  << std::setw(8) << "YUV444"
                  << std::setw(8) << "YUV422"
                  << std::setw(8) << "10bit"
                  << std::setw(10) << "Lossless"
                  << std::setw(8) << "SAO" << std::endl;
        std::cout << std::string(98, '-') << std::endl;

        // Define major codecs to query
        struct { const char* name; GUID guid; } codecs[] = {
            {"H264", NV_ENC_CODEC_H264_GUID},
            {"HEVC", NV_ENC_CODEC_HEVC_GUID},
            {"AV1", NV_ENC_CODEC_AV1_GUID}
        };

        for (int i = 0; i < sizeof(codecs)/sizeof(codecs[0]); i++) {
            if (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES)) {

                // Format resolution strings
                std::string maxRes = std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_WIDTH_MAX)) + "*" +
                                   std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_HEIGHT_MAX));
                std::string minRes = std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_WIDTH_MIN)) + "*" +
                                   std::to_string(enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_HEIGHT_MIN));

                // Get key capabilities
                int engines = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_ENCODER_ENGINES);
                int maxBFrames = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                int tempLayers = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS);
                int ltrFrames = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_NUM_MAX_LTR_FRAMES);
                bool yuv444 = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
                bool yuv422 = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_YUV422_ENCODE);
                bool tenBit = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
                bool lossless = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE);

                // SAO support (HEVC-specific)
                std::string saoSupport = "N/A";
                if (strcmp(codecs[i].name, "HEVC") == 0) {
                    saoSupport = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_SAO) ? "yes" : "no";
                }


                // Print table row
                std::cout << std::left
                          << std::setw(8) << codecs[i].name
                          << std::setw(8) << engines
                          << std::setw(12) << maxRes
                          << std::setw(10) << minRes
                          << std::setw(8) << maxBFrames
                          << std::setw(8) << tempLayers
                          << std::setw(8) << ltrFrames
                          << std::setw(8) << (yuv444 ? "yes" : "no")
                          << std::setw(8) << (yuv422 ? "yes" : "no")
                          << std::setw(8) << (tenBit ? "yes" : "no")
                          << std::setw(10) << (lossless ? "yes" : "no")
                          << std::setw(8) << saoSupport << std::endl;
            }
        }

        std::cout << std::endl << "Legend:" << std::endl;
        std::cout << "  Engines   = Number of hardware encoder engines" << std::endl;
        std::cout << "  MaxB      = Maximum B-frames supported" << std::endl;
        std::cout << "  TempL     = Maximum temporal layers" << std::endl;
        std::cout << "  LTR       = Maximum long-term reference frames" << std::endl;
        std::cout << "  SAO       = Sample Adaptive Offset (HEVC-specific)" << std::endl;

        // ====== DETAILED CAPABILITIES (ALL REMAINING CAPABILITIES) ======
        std::cout << std::endl << "=== DETAILED CAPABILITIES (COMPREHENSIVE) ===" << std::endl;

        for (int i = 0; i < sizeof(codecs)/sizeof(codecs[0]); i++) {
            if (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES)) {
                std::cout << std::endl << "--- " << codecs[i].name << " DETAILED CAPABILITIES ---" << std::endl;

                // Get format capabilities for this codec
                bool yuv444 = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
                bool yuv422 = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_YUV422_ENCODE);
                bool tenBit = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
                bool lossless = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE);

                // === HARDWARE & PERFORMANCE (not in table) ===
                std::cout << "  HARDWARE & PERFORMANCE:" << std::endl;
                std::cout << "\tAsync_Encode_Support:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT) ? "yes" : "no") << std::endl;
                std::cout << "\tMax_MB_Count:\t\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_MB_NUM_MAX) << std::endl;
                std::cout << "\tMax_MB_Per_Sec:\t\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_MB_PER_SEC_MAX) << std::endl;
                std::cout << "\tPreprocessing_Support:\t\t0x" << std::hex << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_PREPROC_SUPPORT) << std::dec << " (bitmask of preproc flags)" << std::endl;
                std::cout << "\tDynamic_Query_Encoder_Capacity:\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_DYNAMIC_QUERY_ENCODER_CAPACITY) << "%" << std::endl;

                // === LEVELS & STANDARDS ===
                std::cout << "  LEVELS & STANDARDS:" << std::endl;
                std::cout << "\tLevel_Max:\t\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_LEVEL_MAX) << std::endl;
                std::cout << "\tLevel_Min:\t\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_LEVEL_MIN) << std::endl;

                // === RATE CONTROL & ENCODING ===
                std::cout << "  RATE CONTROL & ENCODING:" << std::endl;
                // Parse and display rate control modes in readable format
                uint32_t rcModes = enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES);
                std::cout << "\tSupported_Ratecontrol_Modes:\t";
                std::vector<std::string> supportedModes;
                // Note: CONSTQP is always supported if any mode is supported
                if (rcModes > NV_ENC_PARAMS_RC_CONSTQP) supportedModes.push_back("CONSTQP");
                if (rcModes & NV_ENC_PARAMS_RC_VBR) supportedModes.push_back("VBR");
                if (rcModes & NV_ENC_PARAMS_RC_CBR) supportedModes.push_back("CBR");
                
                for (size_t j = 0; j < supportedModes.size(); j++) {
                    std::cout << supportedModes[j];
                    if (j < supportedModes.size() - 1) std::cout << ", ";
                }
                std::cout << " (0x" << std::hex << rcModes << std::dec << ")" << std::endl;
                std::cout << "\tField_Encoding_Support:\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_FIELD_ENCODING) << " (0=no,1=field,2=both)" << std::endl;
                std::cout << "\tMonochrome_Support:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_MONOCHROME) ? "yes" : "no") << std::endl;
                std::cout << "\tBDirect_Mode_Support:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_BDIRECT_MODE) ? "yes" : "no") << std::endl;
                std::cout << "\tCABAC_Support:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_CABAC) ? "yes" : "no") << std::endl;
                std::cout << "\tFMO_Support:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_FMO) ? "yes" : "no") << std::endl;
                std::cout << "\tQPEL_MV_Support:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_QPELMV) ? "yes" : "no") << std::endl;
                std::cout << "\tSeparate_Colour_Plane:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SEPARATE_COLOUR_PLANE) ? "yes" : "no") << std::endl;
                std::cout << "\tME_Only_Mode:\t\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_MEONLY_MODE) << " (0=not supported,1=I+P,2=I+P+B)" << std::endl;

                // === ADVANCED FEATURES (detailed breakdown) ===
                std::cout << "  ADVANCED FEATURES (DETAILED):" << std::endl;
                std::cout << "\tLookahead_Support:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) ? "yes" : "no") << std::endl;
                std::cout << "\tLookahead_Level:\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_LOOKAHEAD_LEVEL) << std::endl;
                std::cout << "\tTemporal_AQ:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) ? "yes" : "no") << std::endl;
                std::cout << "\tWeighted_Prediction:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION) ? "yes" : "no") << std::endl;
                std::cout << "\tAdaptive_Transform:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM) ? "yes" : "no") << std::endl;
                std::cout << "\tTemporal_Filter:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_TEMPORAL_FILTER) ? "yes" : "no") << std::endl;
                std::cout << "\tStereo_MVC:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_STEREO_MVC) ? "yes" : "no") << std::endl;
                std::cout << "\tHierarchical_PFrames:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES) ? "yes" : "no") << std::endl;
                std::cout << "\tHierarchical_BFrames:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES) ? "yes" : "no") << std::endl;
                std::cout << "\tTemporal_SVC:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC) ? "yes" : "no") << std::endl;
                std::cout << "\tBFrame_Ref_Mode:\t\t" << enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE) << " (0=disabled,1=all B-frames as ref,2=middle B-frame only)" << std::endl;
                std::cout << "\tUnidirectional_B:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_UNIDIRECTIONAL_B) ? "yes" : "no") << std::endl;
                std::cout << "\tMultiple_Ref_Frames:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES) ? "yes" : "no") << std::endl;

                // === FORMAT & QUALITY (additional details) ===
                std::cout << "  FORMAT & QUALITY (ADDITIONAL):" << std::endl;
                std::cout << "\tYUV444_Encoding_Support:\t" << (yuv444 ? "yes" : "no") << std::endl;
                std::cout << "\tYUV422_Encoding_Support:\t" << (yuv422 ? "yes" : "no") << std::endl;
                std::cout << "\t10bit_Encoding_Support:\t\t" << (tenBit ? "yes" : "no") << std::endl;
                std::cout << "\tLossless_Encoding_Support:\t" << (lossless ? "yes" : "no") << std::endl;
                std::cout << "\tAlpha_Layer_Encoding:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_ALPHA_LAYER_ENCODING) ? "yes" : "no") << std::endl;

                // === DYNAMIC FEATURES ===
                std::cout << "  DYNAMIC FEATURES:" << std::endl;
                std::cout << "\tDynamic_Resolution_Change:\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE) ? "yes" : "no") << std::endl;
                std::cout << "\tDynamic_Bitrate_Change:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE) ? "yes" : "no") << std::endl;
                std::cout << "\tDynamic_RC_Mode_Change:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE) ? "yes" : "no") << std::endl;
                std::cout << "\tDynamic_Force_CQP:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP) ? "yes" : "no") << std::endl;

                // === ENCODING CONTROL & OUTPUT ===
                std::cout << "  ENCODING CONTROL & OUTPUT:" << std::endl;
                std::cout << "\tSubframe_Readback:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK) ? "yes" : "no") << std::endl;
                std::cout << "\tConstrained_Encoding:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING) ? "yes" : "no") << std::endl;
                std::cout << "\tIntra_Refresh:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_INTRA_REFRESH) ? "yes" : "no") << std::endl;
                std::cout << "\tSingle_Slice_Intra_Refresh:\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH) ? "yes" : "no") << std::endl;
                std::cout << "\tCustom_VBV_Buffer_Size:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE) ? "yes" : "no") << std::endl;
                std::cout << "\tDynamic_Slice_Mode:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE) ? "yes" : "no") << std::endl;
                std::cout << "\tRef_Pic_Invalidation:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION) ? "yes" : "no") << std::endl;
                std::cout << "\tEmphasis_Level_Map:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP) ? "yes" : "no") << std::endl;
                std::cout << "\tDisable_Enc_State_Advance:\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_DISABLE_ENC_STATE_ADVANCE) ? "yes" : "no") << std::endl;
                std::cout << "\tOutput_Recon_Surface:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_OUTPUT_RECON_SURFACE) ? "yes" : "no") << std::endl;
                std::cout << "\tOutput_Block_Stats:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_OUTPUT_BLOCK_STATS) ? "yes" : "no") << std::endl;
                std::cout << "\tOutput_Row_Stats:\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_OUTPUT_ROW_STATS) ? "yes" : "no") << std::endl;

                // === CODEC-SPECIFIC FEATURES ===
                if (strcmp(codecs[i].name, "HEVC") == 0) {
                    std::cout << "  HEVC-SPECIFIC FEATURES:" << std::endl;
                    std::cout << "\tMVHEVC_Encode:\t\t\t" << (enc.GetCapabilityValue(codecs[i].guid, NV_ENC_CAPS_SUPPORT_MVHEVC_ENCODE) ? "yes" : "no") << std::endl;
                }
            }
        }

        std::cout << std::endl;
        enc.DestroyEncoder();
        ck(cuCtxDestroy(cuContext));
    }
    exit(0);
}

// Main capability function (shows summary + hint for detailed)
inline void ShowEncoderCapability()
{
    ShowEncoderCapabilitySummary();
    std::cout << "\nFor detailed information about all capabilities, use -ec-detail" << std::endl;
    exit(0);
}


inline void ShowEncoderD3DBriefHelp()
{
    std::ostringstream oss;
    oss << "NVIDIA Video Encoder AppEncD3D Sample Application\n";
    oss << "============================================\n\n";

    oss << "Usage: AppEncD3D -i <input_file> [options]\n\n";

    // Brief table of core arguments
    oss << "Common Arguments:\n";
    oss << std::left << std::setw(25) << "Argument"
        << std::setw(12) << "Type"
        << "Default Value\n";
    oss << std::string(50, '-') << "\n";

    oss << std::left << std::setw(25) << "-i <path>"
        << std::setw(12) << "Required"
        << "N/A\n";
    oss << std::left << std::setw(25) << "-o <path>"
        << std::setw(12) << "Optional"
        << "codec-based (out.h264/hevc/av1)\n";
    oss << std::left << std::setw(25) << "-s <WxH>"
        << std::setw(12) << "Required"
        << "N/A\n";
    oss << std::left << std::setw(25) << "-gpu <n>"
        << std::setw(12) << "Optional"
        << "0\n";
    oss << std::left << std::setw(25) << "-nv12"
        << std::setw(12) << "Optional"
        << "false\n";

    oss << "\nFor detailed help, use -A/--advanced-options\n";
    oss << "To view encoder capabilities:\n";
    oss << "  -ec        : Print encoder capabilities summary\n";
    oss << "  -ec-detail : Print detailed encoder capabilities\n";
    std::cout << oss.str();
    exit(0);
}

inline void ShowEncoderD3DDetailedHelp(bool bOutputInVidMem = false, bool bIsD3D12Encode = false)
{
    std::ostringstream oss;
    oss << "NVIDIA Video Encoder AppEncD3D Sample Application - Detailed Help\n";
    oss << "=======================================================\n\n";

    oss << "Usage: AppEncD3D -i <input_file> [options]\n\n";

    // Full table of all arguments
    oss << "All Arguments:\n";
    oss << std::left << std::setw(25) << "Argument"
        << std::setw(12) << "Type"
        << std::setw(20) << "Default Value"
        << "Example\n";
    oss << std::string(80, '-') << "\n";

    oss << std::left << std::setw(25) << "-i <path>"
        << std::setw(12) << "Required"
        << std::setw(20) << "N/A"
        << "-i input.bgra\n";
    oss << std::left << std::setw(25) << "-o <path>"
        << std::setw(12) << "Optional"
        << std::setw(20) << "codec-based"
        << "-o output.h264\n";
    oss << std::left << std::setw(25) << "-s <WxH>"
        << std::setw(12) << "Required"
        << std::setw(20) << "N/A"
        << "-s 1920x1080\n";
    oss << std::left << std::setw(25) << "-gpu <n>"
        << std::setw(12) << "Optional"
        << std::setw(20) << "0"
        << "-gpu 1\n";
    oss << std::left << std::setw(25) << "-nv12"
        << std::setw(12) << "Optional"
        << std::setw(20) << "false"
        << "-nv12\n";

    if (bOutputInVidMem)
    {
        oss << std::left << std::setw(25) << "-outputInVidMem"
            << std::setw(12) << "Optional"
            << std::setw(20) << "0"
            << "-outputInVidMem 1\n";
    }

    // Detailed descriptions
    oss << "\nDetailed Descriptions:\n";
    oss << "-------------------\n";
    oss << std::left << std::setw(25) << "-i" << ": Input file path (must be in BGRA format)\n";
    oss << std::left << std::setw(25) << "-o" << ": Output file path\n";
    oss << std::left << std::setw(25) << "-s" << ": Input resolution in WxH format\n";
    oss << std::left << std::setw(25) << "-gpu" << ": Ordinal of GPU to use\n";
    if (!bIsD3D12Encode)
        oss << std::left << std::setw(25) << "-nv12" << ": Convert to NV12 before encoding\n";
    if (bOutputInVidMem)
        oss << std::left << std::setw(25) << "-outputInVidMem" << ": Enable output in Video Memory (0/1)\n";
    oss << std::left << std::setw(25) << "-h/--help" << ": Print basic usage information\n";
    oss << std::left << std::setw(25) << "-A/--advanced-options" << ": Print detailed usage information\n";
    oss << std::left << std::setw(25) << "-ec" << ": Print encoder capabilities summary\n";
    oss << std::left << std::setw(25) << "-ec-detail" << ": Print detailed encoder capabilities\n";

    // Important notes
    oss << "\nNotes:\n";
    oss << "------\n";
    oss << "* Input file must be in BGRA format\n";
    oss << "* Width and height must be specified for encoding\n";
    if (bOutputInVidMem)
    {
        oss << "* When outputInVidMem is enabled, CRC of encoded frames will be computed\n";
        oss << "  and dumped to file with suffix '_crc.txt' added to output file\n";
    }
    oss << std::endl;

    oss << NvEncoderInitParam().GetHelpMessage(false, false, true, false, bOutputInVidMem, !bIsD3D12Encode, bIsD3D12Encode, false) << std::endl;
    oss << "\nTo view encoder capabilities:\n";
    oss << "  -ec        : Print encoder capabilities summary\n";
    oss << "  -ec-detail : Print detailed encoder capabilities\n";
    std::cout << oss.str();
    exit(0);
}

inline void ShowHelpAndExit_AppEncD3D(const char *szBadOption = NULL, bool bOutputInVidMem = false, bool bIsD3D12Encode = false)
{
    if (szBadOption)
    {
        std::ostringstream oss;
        oss << "Error parsing \"" << szBadOption << "\"\n";
        oss << "Use -h/--help for basic usage or -A/--advanced-options for detailed information\n";
        throw std::invalid_argument(oss.str());
    }
}

inline void ParseCommandLine_AppEncD3D(int argc, char *argv[], char *szInputFileName, int &nWidth, int &nHeight,
    char *szOutputFileName, NvEncoderInitParam &initParam, int &iGpu, bool &bForceNv12, int *outputInVidMem = NULL, bool bEnableOutputInVidMem = false, bool bIsD3D12Encode = false)
{
    std::ostringstream oss;
    int i;

    if (argc == 1) {
        std::cout << "No Arguments provided! Please refer to the following for options:\n";
        ShowEncoderD3DBriefHelp();
    }

    for (i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help")) {
            ShowEncoderD3DBriefHelp();
        }
        if (!_stricmp(argv[i], "-A") || !_stricmp(argv[i], "--advanced-options")) {
            ShowEncoderD3DDetailedHelp(bEnableOutputInVidMem, bIsD3D12Encode);
        }
        if (!_stricmp(argv[i], "-ec-detail")) {
            ShowEncoderCapabilityDetailed();
        }
        if (!_stricmp(argv[i], "-ec")) {
            ShowEncoderCapability();
        }
        if (!_stricmp(argv[i], "-i"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit_AppEncD3D("-i", bEnableOutputInVidMem, bIsD3D12Encode);
            }
            sprintf(szInputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-o")) {
            if (++i == argc) {
                ShowHelpAndExit_AppEncD3D("-o", bEnableOutputInVidMem, bIsD3D12Encode);
            }
            sprintf(szOutputFileName, "%s", argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-s")) {
            if (++i == argc || 2 != sscanf(argv[i], "%dx%d", &nWidth, &nHeight)) {
                ShowHelpAndExit_AppEncD3D("-s", bEnableOutputInVidMem, bIsD3D12Encode);
            }
            continue;
        }
        if (!_stricmp(argv[i], "-gpu")) {
            if (++i == argc) {
                ShowHelpAndExit_AppEncD3D("-gpu", bEnableOutputInVidMem, bIsD3D12Encode);
            }
            iGpu = atoi(argv[i]);
            continue;
        }
        if (!_stricmp(argv[i], "-nv12")) {
            bForceNv12 = true;
            continue;
        }
        if (!_stricmp(argv[i], "-outputInVidMem"))
        {
            if (++i == argc)
            {
                ShowHelpAndExit_AppEncD3D("-outputInVidMem", bEnableOutputInVidMem, bIsD3D12Encode);
            }
            if (outputInVidMem != NULL)
            {
                *outputInVidMem = (atoi(argv[i]) != 0) ? 1 : 0;
            }
            continue;
        }

        // Regard as encoder parameter
        if (argv[i][0] != '-') {
            ShowHelpAndExit_AppEncD3D(argv[i], bEnableOutputInVidMem, bIsD3D12Encode);
        }
        oss << argv[i] << " ";
        while (i + 1 < argc && argv[i + 1][0] != '-') {
            oss << argv[++i] << " ";
        }
    }
    initParam = NvEncoderInitParam(oss.str().c_str());
}
