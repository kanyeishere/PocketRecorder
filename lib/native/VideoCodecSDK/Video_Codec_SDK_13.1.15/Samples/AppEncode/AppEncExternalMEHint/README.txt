AppExternalMEHint
=================

This sample application demonstrates passing external ME hints to NVENC.
The application uses the CUDA interface to demonstrate the above feature, but can also be used with the D3D or OpenGL interfaces.

Usage
-----
To pass external ME hints, set -enableExternalMEHint option to 1.
When external ME hints are to be passed to NVENC, the application expects the following options :
     -maxMEHintCountsPerBlock  : specifies maximum number of hints per block (used during encoder initialization)
     -externalMEHintConfigFile : specifies a config file which specifies the following at a frame level
                                  1) The hint counts per block (members of NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE struct, 
                                      the values specified will be used to fill NV_ENC_PIC_PARAMS::meHintCountsPerBlock) 
                                  2) Path to input hint file for current frame

An external ME hint file should be provided for each frame (if external ME hints file is missing for a frame, the application will pass invalid hints for that frame)
and the path to this files should be specified through the externalMEHintConfigFile, the content of this file is interpreted as an array of NVENC_EXTERNAL_ME_HINT structs.
The app expects the file to contain valid number of NVENC_EXTERNAL_ME_HINT structs for each block as per the specified options.

Sample Tests
------------
1) IPP test 
Motion hints provided for Frame1 and Frame2.
The block in Frame1 is displaced by a vector of (300, 0) in fullpel units wrt the block in Frame0
The block in Frame2 is displaced by a vector of (0, -192) in fullpel units wrt the block in Frame1

Run the following CLIs and compare the bitstreams, IPPWithMEHints.av1 should have the correct motion vectors for the blocks in Frame1 and Frame2.
./AppEncExternalMEHint  -i IPP_1920x1280.yuv -o NoMEHints.av1 -s 1920x1280 -bf 0 -codec av1
./AppEncExternalMEHint  -i IPP_1920x1280.yuv -o IPPWithMEHints.av1 -s 1920x1280 -bf 0 -enableExternalMEHint 1 -maxMEHintCountsPerBlock 1 -externalMEHintConfigFile AV1ExternalMEHintConfigFileIPP.txt -codec av1

2) IBP test
Motion hints provided only for Frame1 (B-frame)
The upper block in Frame1 (B-frame) is displaced by a vector of (300, 0) in fullpel units wrt the upper block in Frame0 (I-frame).
The lower block in Frame1 (B-frame) is displaced by a vector of (-300, 0) in fullpel units wrt the lower block in Frame2 (P-frame).

Run the following CLIs and compare the bitstreams, IBPWithMEHints.265 should have correct motion vectors for the upper and lower blocks for the B-frame.
./AppEncExternalMEHint -i IBP_1920x1280.yuv -o NoMEHints.265 -s 1920x1280 -bf 1 -codec hevc
./AppEncExternalMEHint -i IBP_1920x1280.yuv -o IBPWithMEHints.265 -s 1920x1280 -bf 1 -enableExternalMEHint 1 -maxMEHintCountsPerBlock 1 -externalMEHintConfigFile HEVCExternalMEHintConfigFileIBP.txt -codec hevc