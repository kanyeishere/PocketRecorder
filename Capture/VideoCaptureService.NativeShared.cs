using Recorder.Diagnostics;
using System;
using System.Diagnostics;
using TerraFX.Interop.DirectX;
using DXGI_FORMAT = TerraFX.Interop.DirectX.DXGI_FORMAT;

namespace Recorder.Capture;

internal sealed unsafe partial class VideoCaptureService
{
    private bool TryProcessTextureAsD3D11(
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* srcTexture,
        uint width,
        uint height,
        DXGI_FORMAT format,
        uint sampleCount,
        long timestampTicks)
    {
        if (!PreferD3D11TextureFrames)
            return false;

        if (_nativeSharedDisabled)
            return false;

        if (_device == null || ctx == null || srcTexture == null)
            return false;

        if (sampleCount != 1)
        {
            _skipCount++;
            string reason = $"Native D3D11 texture path does not support MSAA sampleCount={sampleCount}; falling back to readback.";
            if (_skipCount <= 3)
                Plugin.Log!.Warning($"[Video] {reason}");
            RecordingDiagnosticLog.WriteNativeFailure("Video", reason);
            PreferD3D11TextureFrames = false;
            return false;
        }

        if (!VideoCaptureFormats.IsSupportedReadbackFormat(format))
        {
            string reason = $"Native D3D11 texture path does not support source format={format}; falling back to readback.";
            Plugin.Log!.Warning($"[Video] {reason}");
            RecordingDiagnosticLog.WriteNativeFailure("Video", reason);
            PreferD3D11TextureFrames = false;
            return false;
        }

        DXGI_FORMAT sharedFormat = VideoCaptureFormats.GetNativeSharedFormat(format);
        if (!EnsureNativeSharedTexture(width, height, sharedFormat))
            return false;

        D3D11SharedTextureMailbox? mailbox = _nativeSharedMailbox;
        if (mailbox == null)
            return false;

        IntPtr keyedMutex = _nativeSharedKeyedMutex;
        if (keyedMutex == IntPtr.Zero)
        {
            DisableNativeSharedPath("Native shared texture has no IDXGIKeyedMutex interface.");
            return false;
        }

        _nativeSharedAcquireAttempts++;
        long acquireStartTicks = Stopwatch.GetTimestamp();
        int acquireHr = D3D11InteropHelpers.AcquireKeyedMutex(
            keyedMutex,
            NativeSharedGameWriteKey,
            NativeSharedAcquireTimeoutMs);
        long acquireTicks = Math.Max(0, Stopwatch.GetTimestamp() - acquireStartTicks);
        if (acquireHr == WaitTimeout || acquireHr == DxgiErrorWaitTimeout)
        {
            SkipNativeSharedFrameForBusySlot();
            return true;
        }
        if (acquireHr != 0)
        {
            DisableNativeSharedPath($"IDXGIKeyedMutex::AcquireSync(game-write) failed: 0x{acquireHr:X8}.");
            return false;
        }

        _nativeSharedAcquireSuccesses++;
        _nativeSharedAcquireTotalTicks += acquireTicks;
        _nativeSharedAcquireMaxTicks = Math.Max(_nativeSharedAcquireMaxTicks, acquireTicks);

        ID3D11Texture2D* sharedTexture = (ID3D11Texture2D*)_nativeSharedTexture;
        if (_nativeSharedGameBridgeTexture != IntPtr.Zero)
        {
            if (_nativeSharedHelperBridgeTexture == IntPtr.Zero || _nativeSharedOwnerContext == IntPtr.Zero)
            {
                DisableNativeSharedPath("Native keyed texture GPU bridge resources were incomplete.");
                return false;
            }

            ID3D11Texture2D* gameBridgeTexture = (ID3D11Texture2D*)_nativeSharedGameBridgeTexture;
            ID3D11Texture2D* helperBridgeTexture = (ID3D11Texture2D*)_nativeSharedHelperBridgeTexture;
            ID3D11DeviceContext* helperContext = (ID3D11DeviceContext*)_nativeSharedOwnerContext;

            // The game wrapper only supports legacy plain-shared textures. Flush is required
            // before the same resource is consumed by a different D3D11 device.
            ctx->CopyResource((ID3D11Resource*)gameBridgeTexture, (ID3D11Resource*)srcTexture);
            ctx->Flush();
            helperContext->CopyResource((ID3D11Resource*)sharedTexture, (ID3D11Resource*)helperBridgeTexture);
            helperContext->Flush();
            _nativeSharedBridgeCopies++;
        }
        else
        {
            ctx->CopyResource((ID3D11Resource*)sharedTexture, (ID3D11Resource*)srcTexture);
            _nativeSharedDirectCopies++;
        }

        int releaseHr = D3D11InteropHelpers.ReleaseKeyedMutex(keyedMutex, NativeSharedEncoderReadKey);
        if (releaseHr != 0)
        {
            DisableNativeSharedPath($"IDXGIKeyedMutex::ReleaseSync(encoder-read) failed: 0x{releaseHr:X8}.");
            return false;
        }
        _nativeSharedReleaseSuccesses++;

        long timestampHns = timestampTicks * 10_000_000L / Stopwatch.Frequency;
        mailbox.Publish(timestampHns);
        _nativeSharedPublishedFrames++;

        VideoFrame frame = new(mailbox, timestampHns);
        bool delivered = false;
        try
        {
            _onFrame(frame);
            delivered = true;
            _frameCount++;

            if (_frameCount % 300 == 0)
                Plugin.Log!.Info($"[Video] {CurrentWidth}x{CurrentHeight} D3D11 shared texture update #{_frameCount}, method={_captureMethod}");

            return true;
        }
        finally
        {
            if (!delivered)
                frame.ReturnBuffer();
        }
    }

    private void ReleaseNativeSharedTextures()
    {
        _nativeSharedMailbox?.Dispose();
        _nativeSharedMailbox = null;

        if (_nativeSharedKeyedMutex != IntPtr.Zero)
        {
            System.Runtime.InteropServices.Marshal.Release(_nativeSharedKeyedMutex);
            _nativeSharedKeyedMutex = IntPtr.Zero;
        }

        if (_nativeSharedOwnerContext != IntPtr.Zero)
        {
            ((ID3D11DeviceContext*)_nativeSharedOwnerContext)->Release();
            _nativeSharedOwnerContext = IntPtr.Zero;
        }

        if (_nativeSharedHelperBridgeTexture != IntPtr.Zero)
        {
            ((ID3D11Texture2D*)_nativeSharedHelperBridgeTexture)->Release();
            _nativeSharedHelperBridgeTexture = IntPtr.Zero;
        }

        if (_nativeSharedGameBridgeTexture != IntPtr.Zero)
        {
            ((ID3D11Texture2D*)_nativeSharedGameBridgeTexture)->Release();
            _nativeSharedGameBridgeTexture = IntPtr.Zero;
        }

        if (_nativeSharedTexture != IntPtr.Zero)
        {
            ((ID3D11Texture2D*)_nativeSharedTexture)->Release();
            _nativeSharedTexture = IntPtr.Zero;
        }

        if (_nativeSharedOwnerDevice != IntPtr.Zero)
        {
            ((ID3D11Device*)_nativeSharedOwnerDevice)->Release();
            _nativeSharedOwnerDevice = IntPtr.Zero;
        }

        if (_nativeSharedHandleIsNt)
            D3D11InteropHelpers.CloseSharedNtHandle(_nativeSharedHandle);
        _nativeSharedHandle = IntPtr.Zero;
        _nativeSharedHandleIsNt = false;

        if (_nativeSharedDevice != IntPtr.Zero)
        {
            ((ID3D11Device*)_nativeSharedDevice)->Release();
            _nativeSharedDevice = IntPtr.Zero;
        }

        _nativeSharedWidth = 0;
        _nativeSharedHeight = 0;
        _nativeSharedFormat = 0;
    }

    private bool EnsureNativeSharedTexture(uint width, uint height, DXGI_FORMAT format)
    {
        if (_nativeSharedTexture != IntPtr.Zero &&
            _nativeSharedKeyedMutex != IntPtr.Zero &&
            _nativeSharedMailbox != null &&
            width == _nativeSharedWidth &&
            height == _nativeSharedHeight &&
            format == _nativeSharedFormat &&
            _nativeSharedDevice == (IntPtr)_device)
        {
            return true;
        }

        ReleaseNativeSharedTextures();

        if (!TryCreateNativeSharedTexture(
                width,
                height,
                format,
                out ID3D11Texture2D* texture,
                out IntPtr sharedHandle,
                out bool sharedHandleIsNt,
                out IntPtr keyedMutex,
                out IntPtr ownerDevice,
                out IntPtr ownerContext,
                out IntPtr gameBridgeTexture,
                out IntPtr helperBridgeTexture,
                out string error))
        {
            DisableNativeSharedPath(error);
            ReleaseNativeSharedTextures();
            return false;
        }

        _device->AddRef();
        _nativeSharedDevice = (IntPtr)_device;
        _nativeSharedTexture = (IntPtr)texture;
        _nativeSharedHandle = sharedHandle;
        _nativeSharedHandleIsNt = sharedHandleIsNt;
        _nativeSharedKeyedMutex = keyedMutex;
        _nativeSharedOwnerDevice = ownerDevice;
        _nativeSharedOwnerContext = ownerContext;
        _nativeSharedGameBridgeTexture = gameBridgeTexture;
        _nativeSharedHelperBridgeTexture = helperBridgeTexture;
        _nativeSharedWidth = width;
        _nativeSharedHeight = height;
        _nativeSharedFormat = format;
        IntPtr mailboxDevice = _nativeSharedOwnerDevice != IntPtr.Zero
            ? _nativeSharedOwnerDevice
            : _nativeSharedDevice;
        _nativeSharedMailbox = new D3D11SharedTextureMailbox(
            mailboxDevice,
            _nativeSharedTexture,
            _nativeSharedHandle,
            (int)format,
            (int)width,
            (int)height);

        string transfer = _nativeSharedGameBridgeTexture != IntPtr.Zero
            ? "game-plain-shared-gpu-bridge->helper-keyed-mutex"
            : "direct-keyed-mutex";
        Plugin.Log!.Info($"[Video] native shared texture ready: {width}x{height}, format={format}, slots=1, sync=keyed-mutex, transfer={transfer}");
        return true;
    }

    private bool TryCreateNativeSharedTexture(
        uint width,
        uint height,
        DXGI_FORMAT format,
        out ID3D11Texture2D* texture,
        out IntPtr sharedHandle,
        out bool sharedHandleIsNt,
        out IntPtr keyedMutex,
        out IntPtr ownerDevice,
        out IntPtr ownerContext,
        out IntPtr gameBridgeTexture,
        out IntPtr helperBridgeTexture,
        out string error)
    {
        texture = null;
        sharedHandle = IntPtr.Zero;
        sharedHandleIsNt = false;
        keyedMutex = IntPtr.Zero;
        ownerDevice = IntPtr.Zero;
        ownerContext = IntPtr.Zero;
        gameBridgeTexture = IntPtr.Zero;
        helperBridgeTexture = IntPtr.Zero;

        if (TryCreateKeyedTextureOnDevice(
                _device,
                useNtHandle: false,
                width,
                height,
                format,
                out ID3D11Texture2D* directTexture,
                out IntPtr directHandle,
                out IntPtr directMutex,
                out string directMode,
                out uint directBindFlags,
                out string directAttempts))
        {
            Plugin.Log!.Info(
                $"[Video] native keyed texture accepted: owner=game-device, handle=legacy, mode={directMode}, " +
                $"bind=0x{directBindFlags:X}, misc=0x{D3D11ResourceMiscSharedKeyedMutex:X}");
            texture = directTexture;
            sharedHandle = directHandle;
            keyedMutex = directMutex;
            error = string.Empty;
            return true;
        }

        if (TryCreateKeyedTextureOnDevice(
                _device,
                useNtHandle: true,
                width,
                height,
                format,
                out directTexture,
                out directHandle,
                out directMutex,
                out directMode,
                out directBindFlags,
                out string directNtAttempts))
        {
            uint ntMiscFlags = D3D11ResourceMiscSharedKeyedMutex | D3D11ResourceMiscSharedNtHandle;
            Plugin.Log!.Info(
                $"[Video] native keyed texture accepted: owner=game-device, handle=nt, mode={directMode}, " +
                $"bind=0x{directBindFlags:X}, misc=0x{ntMiscFlags:X}");
            texture = directTexture;
            sharedHandle = directHandle;
            sharedHandleIsNt = true;
            keyedMutex = directMutex;
            error = string.Empty;
            return true;
        }

        if (!D3D11InteropHelpers.TryCreateDeviceOnSameAdapter(
                (IntPtr)_device,
                out IntPtr helperDevicePtr,
                out string helperDeviceError))
        {
            error =
                $"Game device rejected all keyed textures and a same-adapter owner device could not be created; " +
                $"gameLegacyAttempts:{directAttempts} gameNtAttempts:{directNtAttempts} helper={helperDeviceError}.";
            return false;
        }

        ID3D11Texture2D* helperTexture = null;
        IntPtr helperHandle = IntPtr.Zero;
        bool helperHandleIsNt = false;
        IntPtr helperMutex = IntPtr.Zero;
        ID3D11DeviceContext* helperContext = null;
        ID3D11Texture2D* gameBridge = null;
        IntPtr helperBridge = IntPtr.Zero;
        try
        {
            bool helperCreated = TryCreateKeyedTextureOnDevice(
                    (ID3D11Device*)helperDevicePtr,
                    useNtHandle: false,
                    width,
                    height,
                    format,
                    out helperTexture,
                    out helperHandle,
                    out helperMutex,
                    out string helperMode,
                    out uint helperBindFlags,
                    out string helperLegacyAttempts);
            string helperNtAttempts = string.Empty;
            if (!helperCreated)
            {
                helperCreated = TryCreateKeyedTextureOnDevice(
                    (ID3D11Device*)helperDevicePtr,
                    useNtHandle: true,
                    width,
                    height,
                    format,
                    out helperTexture,
                    out helperHandle,
                    out helperMutex,
                    out helperMode,
                    out helperBindFlags,
                    out helperNtAttempts);
                helperHandleIsNt = helperCreated;
            }

            if (!helperCreated)
            {
                error =
                    $"CreateTexture2D(keyed shared native texture) failed on both devices; " +
                    $"desc={width}x{height}, format={format}; " +
                    $"gameLegacyAttempts:{directAttempts} gameNtAttempts:{directNtAttempts} " +
                    $"helperLegacyAttempts:{helperLegacyAttempts} helperNtAttempts:{helperNtAttempts}";
                return false;
            }

            ((ID3D11Device*)helperDevicePtr)->GetImmediateContext(&helperContext);
            if (helperContext == null)
            {
                error =
                    $"Same-adapter owner created a keyed texture but returned no immediate context; " +
                    $"handle={(helperHandleIsNt ? "nt" : "legacy")}, mode={helperMode}, " +
                    $"bind=0x{helperBindFlags:X}.";
                return false;
            }

            if (!TryCreatePlainSharedGpuBridge(
                    (ID3D11Device*)helperDevicePtr,
                    width,
                    height,
                    format,
                    out gameBridge,
                    out helperBridge,
                    out string bridgeError))
            {
                error =
                    $"Same-adapter owner created a keyed texture, but the game-to-helper GPU bridge failed; " +
                    $"handle={(helperHandleIsNt ? "nt" : "legacy")}, mode={helperMode}, " +
                    $"bind=0x{helperBindFlags:X}, bridgeError={bridgeError}; " +
                    $"gameLegacyAttempts:{directAttempts} gameNtAttempts:{directNtAttempts}";
                return false;
            }

            uint helperMiscFlags = D3D11ResourceMiscSharedKeyedMutex |
                                   (helperHandleIsNt ? D3D11ResourceMiscSharedNtHandle : 0);
            Plugin.Log!.Info(
                $"[Video] native keyed texture accepted: owner=same-adapter-system-device, " +
                $"handle={(helperHandleIsNt ? "nt" : "legacy")}, mode={helperMode}, " +
                $"bind=0x{helperBindFlags:X}, misc=0x{helperMiscFlags:X}, " +
                $"producer=game-plain-shared-gpu-bridge");

            texture = helperTexture;
            helperTexture = null;
            sharedHandle = helperHandle;
            helperHandle = IntPtr.Zero;
            sharedHandleIsNt = helperHandleIsNt;
            keyedMutex = helperMutex;
            helperMutex = IntPtr.Zero;
            ownerDevice = helperDevicePtr;
            helperDevicePtr = IntPtr.Zero;
            ownerContext = (IntPtr)helperContext;
            helperContext = null;
            gameBridgeTexture = (IntPtr)gameBridge;
            gameBridge = null;
            helperBridgeTexture = helperBridge;
            helperBridge = IntPtr.Zero;
            error = string.Empty;
            return true;
        }
        finally
        {
            if (helperBridge != IntPtr.Zero)
                System.Runtime.InteropServices.Marshal.Release(helperBridge);
            if (gameBridge != null)
                gameBridge->Release();
            if (helperContext != null)
                helperContext->Release();
            if (helperMutex != IntPtr.Zero)
                System.Runtime.InteropServices.Marshal.Release(helperMutex);
            if (helperHandleIsNt && helperHandle != IntPtr.Zero)
                D3D11InteropHelpers.CloseSharedNtHandle(helperHandle);
            if (helperTexture != null)
                helperTexture->Release();
            if (helperDevicePtr != IntPtr.Zero)
                System.Runtime.InteropServices.Marshal.Release(helperDevicePtr);
        }
    }

    private bool TryCreatePlainSharedGpuBridge(
        ID3D11Device* helperDevice,
        uint width,
        uint height,
        DXGI_FORMAT format,
        out ID3D11Texture2D* gameTexture,
        out IntPtr helperTexture,
        out string error)
    {
        gameTexture = null;
        helperTexture = IntPtr.Zero;
        error = string.Empty;

        if (_device == null || helperDevice == null)
        {
            error = "game or helper D3D11 device was null";
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = default;
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE.D3D11_USAGE_DEFAULT;
        desc.BindFlags = (uint)D3D11_BIND_FLAG.D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11ResourceMiscShared;

        ID3D11Texture2D* candidate = null;
        int createHr = _device->CreateTexture2D(&desc, null, &candidate);
        if (createHr < 0 || candidate == null)
        {
            error =
                $"CreateTexture2D(game plain-shared GPU bridge) failed: 0x{createHr:X8}; " +
                $"desc={width}x{height}, format={format}, bind=0x{desc.BindFlags:X}, misc=0x{desc.MiscFlags:X}";
            return false;
        }

        if (!D3D11InteropHelpers.TryGetSharedHandle((IntPtr)candidate, out IntPtr bridgeHandle) ||
            bridgeHandle == IntPtr.Zero)
        {
            candidate->Release();
            error = "Game plain-shared GPU bridge was created but its legacy shared handle was unavailable";
            return false;
        }

        if (!D3D11InteropHelpers.TryOpenSharedTexture(
                (IntPtr)helperDevice,
                bridgeHandle,
                ntHandle: false,
                out IntPtr openedTexture,
                out string openError))
        {
            candidate->Release();
            error = $"System helper device could not open the game plain-shared GPU bridge: {openError}";
            return false;
        }

        gameTexture = candidate;
        helperTexture = openedTexture;
        return true;
    }

    private bool TryCreateKeyedTextureOnDevice(
        ID3D11Device* creationDevice,
        bool useNtHandle,
        uint width,
        uint height,
        DXGI_FORMAT format,
        out ID3D11Texture2D* texture,
        out IntPtr sharedHandle,
        out IntPtr keyedMutex,
        out string acceptedMode,
        out uint acceptedBindFlags,
        out string attempts)
    {
        texture = null;
        sharedHandle = IntPtr.Zero;
        keyedMutex = IntPtr.Zero;
        acceptedMode = string.Empty;
        acceptedBindFlags = 0;

        uint miscFlags = D3D11ResourceMiscSharedKeyedMutex |
                         (useNtHandle ? D3D11ResourceMiscSharedNtHandle : 0);

        uint shaderResourceBind = (uint)D3D11_BIND_FLAG.D3D11_BIND_SHADER_RESOURCE;
        uint renderTargetBind = (uint)D3D11_BIND_FLAG.D3D11_BIND_RENDER_TARGET;
        DXGI_FORMAT typelessFormat = format switch
        {
            DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM or
            DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_UNORM_SRGB => DXGI_FORMAT.DXGI_FORMAT_R8G8B8A8_TYPELESS,
            DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_UNORM or
            DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_UNORM_SRGB => DXGI_FORMAT.DXGI_FORMAT_B8G8R8A8_TYPELESS,
            _ => format,
        };
        (DXGI_FORMAT Format, string Name)[] formatCandidates = typelessFormat == format
            ? [(format, "native-format")]
            : [(format, "typed"), (typelessFormat, "typeless")];
        (uint BindFlags, string Name)[] candidates =
        [
            (0, "keyed-copy-only"),
            (renderTargetBind, "keyed-render-target"),
            (renderTargetBind | shaderResourceBind, "keyed-render-target-shader-resource"),
            (shaderResourceBind, "keyed-shader-resource"),
        ];

        var failures = new System.Text.StringBuilder();
        foreach ((DXGI_FORMAT resourceFormat, string formatName) in formatCandidates)
        {
            foreach ((uint bindFlags, string name) in candidates)
            {
                string candidateName = $"{formatName}-{name}";
                D3D11_TEXTURE2D_DESC desc = default;
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = resourceFormat;
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                desc.Usage = D3D11_USAGE.D3D11_USAGE_DEFAULT;
                desc.BindFlags = bindFlags;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = miscFlags;

                ID3D11Texture2D* candidate;
                int hr = creationDevice->CreateTexture2D(&desc, null, &candidate);
                if (hr < 0 || candidate == null)
                {
                    failures.Append($" {candidateName}(format={resourceFormat},bind=0x{bindFlags:X})->0x{hr:X8};");
                    continue;
                }

                IntPtr candidateHandle;
                string handleError = string.Empty;
                bool handleCreated = useNtHandle
                    ? D3D11InteropHelpers.TryCreateSharedNtHandle((IntPtr)candidate, out candidateHandle, out handleError)
                    : D3D11InteropHelpers.TryGetSharedHandle((IntPtr)candidate, out candidateHandle);
                if (!handleCreated || candidateHandle == IntPtr.Zero)
                {
                    candidate->Release();
                    string suffix = string.IsNullOrEmpty(handleError) ? string.Empty : $"({handleError})";
                    failures.Append($" {candidateName}->no-{(useNtHandle ? "nt" : "legacy")}-handle{suffix};");
                    continue;
                }

                if (!D3D11InteropHelpers.TryQueryKeyedMutex((IntPtr)candidate, out IntPtr candidateKeyedMutex) ||
                    candidateKeyedMutex == IntPtr.Zero)
                {
                    if (useNtHandle)
                        D3D11InteropHelpers.CloseSharedNtHandle(candidateHandle);
                    candidate->Release();
                    failures.Append($" {candidateName}->no-keyed-mutex;");
                    continue;
                }

                texture = candidate;
                sharedHandle = candidateHandle;
                keyedMutex = candidateKeyedMutex;
                acceptedMode = candidateName;
                acceptedBindFlags = bindFlags;
                attempts = failures.ToString();
                return true;
            }
        }

        attempts = failures.ToString();
        return false;
    }

    private void SkipNativeSharedFrameForBusySlot()
    {
        _skipCount++;
        _nativeSharedBusySkipCount++;

        long now = Stopwatch.GetTimestamp();
        bool shouldLog = _nativeSharedBusySkipCount <= 3 ||
                         now - _lastNativeSharedBusyLogTicks >= Stopwatch.Frequency;
        if (!shouldLog)
        {
            _nativeSharedBusySkipSuppressed++;
            return;
        }

        int suppressed = _nativeSharedBusySkipSuppressed;
        _nativeSharedBusySkipSuppressed = 0;
        _lastNativeSharedBusyLogTicks = now;

        string suffix = suppressed > 0 ? $", suppressed={suppressed}" : string.Empty;
        Plugin.Log!.Info($"[Video] Native shared texture update skipped. busySkips={_nativeSharedBusySkipCount}{suffix}");
    }

    private void DisableNativeSharedPath(string reason)
    {
        _nativeSharedDisabled = true;
        PreferD3D11TextureFrames = false;
        ReleaseNativeSharedTextures();

        if (_nativeSharedFallbackLogged)
            return;

        _nativeSharedFallbackLogged = true;
        Plugin.Log!.Warning($"[Video] Native D3D11 shared texture path disabled; falling back to readback. {reason}");
        RecordingDiagnosticLog.WriteNativeFailure(
            "Video",
            $"Native D3D11 shared texture path disabled; fallback=readback. {reason}");
    }
}
