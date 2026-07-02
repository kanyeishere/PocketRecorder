# NativeRecorder

NativeRecorder is the GPU-surface recording backend for PocketRecorder.

The goal is to avoid the current `ffmpeg.exe` rawvideo stdin path:

```text
D3D11 texture -> GPU color conversion -> hardware encoder -> muxer
```

The current checked-in prototype is intentionally small and dependency-light:

```text
Present hook GPU copy -> shared D3D11 texture -> native D3D11 video processor NV12 conversion -> Media Foundation HEVC/H.264 sink writer -> MP4
```

It is enabled only when PocketRecorder detects an NVIDIA DXGI adapter, the NVIDIA runtime is present, and `NativeRecorder.dll` loads successfully. Other systems, missing DLLs, unsupported codecs, runtime failures, or first-frame submission failures automatically fall back to the existing FFmpeg rawvideo stdin path.

The long-term encoder backend can still be swapped to NVIDIA Video Codec SDK's `NvEncoderD3D11` sample/wrapper behind the same C ABI. The SDK is not vendored in this repository; install it separately and point CMake at it when that backend is implemented.

```powershell
cmake -S NativeRecorder -B NativeRecorder/build -G "Visual Studio 17 2022" -A x64 `
  -DPOCKETRECORDER_ENABLE_NVENC=ON `
  -DPOCKETRECORDER_VIDEO_CODEC_SDK_DIR=C:\SDKs\Video_Codec_SDK
cmake --build NativeRecorder/build --config Release
```

If CMake is not available, the checked-in Visual Studio project builds the runnable Media Foundation native DLL:

```powershell
msbuild NativeRecorder\NativeRecorder.vcxproj /p:Configuration=Release /p:Platform=x64
```

Current status:

- Stable C ABI v6 is defined in `include/pocket_recorder_native.h`.
- `pr_probe` reports D3D11 texture support on NVIDIA machines with `nvEncodeAPI64.dll` and Media Foundation available.
- `pr_submit_d3d11_shared_texture` accepts the source D3D11 device plus a D3D11 shared texture handle, creates the native device on the same adapter, performs GPU NV12 conversion, and writes HEVC or H.264 MP4 through Media Foundation.
- The legacy `pr_submit_d3d11_texture` export is retained only as a non-implemented compatibility stub; PocketRecorder no longer passes the game backbuffer pointer to native code.
- `pr_submit_audio` accepts PCM/float audio packets and writes AAC when audio is enabled.
- PocketRecorder keeps `FFmpegWriter` as the compatibility fallback for non-NVIDIA systems, missing DLLs, ABI mismatches, and failed probes.

Implementation plan:

1. Replace the internal encoder with NVIDIA Video Codec SDK `NvEncoderD3D11`.
2. Mux encoded packets in-process with libavformat, avoiding `ffmpeg.exe` rawvideo stdin.
3. Keep HEVC as the default NVIDIA codec; add AV1 only when the GPU capability probe confirms Ada-or-newer support.
4. Add AMD AMF / Intel oneVPL backends after the NVIDIA path is proven.
