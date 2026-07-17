using System;
using System.Runtime.InteropServices;

namespace Recorder.Capture;

internal static unsafe class D3D11InteropHelpers
{
    private const int D3DDriverTypeUnknown = 0;
    private const uint D3D11CreateDeviceBgraSupport = 0x20;
    private const uint D3D11SdkVersion = 7;
    private const uint DxgiSharedResourceReadWrite = 0x80000001;
    private const int DxgiErrorNotFound = unchecked((int)0x887A0002);
    private static readonly Guid IID_ID3D11Device = new(0xDB6F6DDB, 0xAC77, 0x4E88, 0x82, 0x53, 0x81, 0x9D, 0xF9, 0xBB, 0xF1, 0x40);
    private static readonly Guid IID_ID3D11Device1 = new(0xA04BFB29, 0x08EF, 0x43D6, 0xA4, 0x9C, 0xA9, 0xBD, 0xBD, 0xCB, 0xE6, 0x86);
    private static readonly Guid IID_ID3D11Texture2D = new(0x6F15AAF2, 0xD208, 0x4E89, 0x9A, 0xB4, 0x48, 0x95, 0x35, 0xD3, 0x4F, 0x9C);
    private static readonly Guid IID_IDXGIDevice = new(0x54EC77FA, 0x1377, 0x44E6, 0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C);
    private static readonly Guid IID_IDXGIResource = new(0x035F3AB4, 0x482E, 0x4E50, 0xB4, 0x1F, 0x8A, 0x7F, 0x8B, 0xD8, 0x96, 0x0B);
    private static readonly Guid IID_IDXGIResource1 = new(0x30961379, 0x4609, 0x4A41, 0x99, 0x8E, 0x54, 0xFE, 0x56, 0x7E, 0xE0, 0xC1);
    private static readonly Guid IID_IDXGIKeyedMutex = new(0x9D8E1289, 0xD7B3, 0x465F, 0x81, 0x26, 0x25, 0x0E, 0x34, 0x9A, 0xF8, 0x5D);
    private static readonly Guid IID_IDXGIFactory1 = new(0x770AAE78, 0xF26F, 0x4DBA, 0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87);

    public static bool TryGetD3D11DeviceFromSwapChain(IntPtr swapChainPtr, out IntPtr devicePtr, out string error)
    {
        devicePtr = IntPtr.Zero;
        error = string.Empty;

        if (swapChainPtr == IntPtr.Zero)
        {
            error = "swapchain pointer is null";
            return false;
        }

        void** vtable = *(void***)swapChainPtr;
        // IDXGISwapChain inherits IDXGIDeviceSubObject; GetDevice is slot 7.
        var getDevice = (delegate* unmanaged[Stdcall]<void*, Guid*, void**, int>)vtable[7];
        Guid iid = IID_ID3D11Device;
        void* device = null;
        int hr = getDevice((void*)swapChainPtr, &iid, &device);
        if (hr < 0 || device == null)
        {
            error = $"IDXGISwapChain::GetDevice(ID3D11Device) failed: 0x{hr:X8}";
            return false;
        }

        devicePtr = (IntPtr)device;
        return true;
    }

    public static bool TryGetAdapterInfoFromD3D11Device(IntPtr devicePtr, out D3D11AdapterInfo adapterInfo, out string error)
    {
        adapterInfo = default;
        error = string.Empty;

        if (devicePtr == IntPtr.Zero)
        {
            error = "D3D11 device pointer is null";
            return false;
        }

        if (!TryQueryInterface(devicePtr, IID_IDXGIDevice, out IntPtr dxgiDevicePtr))
        {
            error = "ID3D11Device::QueryInterface(IDXGIDevice) failed";
            return false;
        }

        IntPtr adapterPtr = IntPtr.Zero;
        try
        {
            void** dxgiDeviceVtable = *(void***)dxgiDevicePtr;
            // IDXGIDevice inherits IDXGIObject; GetAdapter is slot 7.
            var getAdapter = (delegate* unmanaged[Stdcall]<void*, void**, int>)dxgiDeviceVtable[7];
            void* adapter = null;
            int hr = getAdapter((void*)dxgiDevicePtr, &adapter);
            if (hr < 0 || adapter == null)
            {
                error = $"IDXGIDevice::GetAdapter failed: 0x{hr:X8}";
                return false;
            }

            adapterPtr = (IntPtr)adapter;
            void** adapterVtable = *(void***)adapterPtr;
            // IDXGIAdapter::GetDesc is slot 8.
            var getDesc = (delegate* unmanaged[Stdcall]<void*, DxgiAdapterDesc*, int>)adapterVtable[8];
            DxgiAdapterDesc desc = default;
            hr = getDesc((void*)adapterPtr, &desc);
            if (hr < 0)
            {
                error = $"IDXGIAdapter::GetDesc failed: 0x{hr:X8}";
                return false;
            }

            adapterInfo = new D3D11AdapterInfo(
                desc.VendorId,
                desc.DeviceId,
                desc.SubSysId,
                desc.Revision,
                ReadAdapterDescription(desc),
                desc.AdapterLuidHighPart,
                desc.AdapterLuidLowPart);
            return true;
        }
        finally
        {
            if (adapterPtr != IntPtr.Zero)
                Marshal.Release(adapterPtr);
            Marshal.Release(dxgiDevicePtr);
        }
    }

    public static bool TryGetSharedHandle(IntPtr texturePtr, out IntPtr sharedHandle)
    {
        sharedHandle = IntPtr.Zero;
        if (!TryQueryInterface(texturePtr, IID_IDXGIResource, out IntPtr resourcePtr))
            return false;

        try
        {
            void** vtable = *(void***)resourcePtr;
            // IDXGIResource inherits IDXGIDeviceSubObject; GetSharedHandle is slot 8.
            var getSharedHandle = (delegate* unmanaged[Stdcall]<void*, IntPtr*, int>)vtable[8];
            IntPtr handle = IntPtr.Zero;
            int hr = getSharedHandle((void*)resourcePtr, &handle);
            if (hr < 0)
                return false;

            sharedHandle = handle;
            return true;
        }
        finally
        {
            Marshal.Release(resourcePtr);
        }
    }

    public static bool TryCreateSharedNtHandle(IntPtr texturePtr, out IntPtr sharedHandle, out string error)
    {
        sharedHandle = IntPtr.Zero;
        error = string.Empty;
        if (!TryQueryInterface(texturePtr, IID_IDXGIResource1, out IntPtr resourcePtr))
        {
            error = "ID3D11Texture2D::QueryInterface(IDXGIResource1) failed";
            return false;
        }

        try
        {
            void** vtable = *(void***)resourcePtr;
            // IDXGIResource1::CreateSharedHandle is slot 13.
            var createSharedHandle = (delegate* unmanaged[Stdcall]<void*, void*, uint, char*, IntPtr*, int>)vtable[13];
            IntPtr handle = IntPtr.Zero;
            int hr = createSharedHandle(
                (void*)resourcePtr,
                null,
                DxgiSharedResourceReadWrite,
                null,
                &handle);
            if (hr < 0 || handle == IntPtr.Zero)
            {
                error = $"IDXGIResource1::CreateSharedHandle failed: 0x{hr:X8}";
                return false;
            }

            sharedHandle = handle;
            return true;
        }
        finally
        {
            Marshal.Release(resourcePtr);
        }
    }

    public static bool TryCreateDeviceOnSameAdapter(IntPtr sourceDevicePtr, out IntPtr devicePtr, out string error)
    {
        devicePtr = IntPtr.Zero;
        error = string.Empty;

        if (sourceDevicePtr == IntPtr.Zero)
        {
            error = "source D3D11 device pointer is null";
            return false;
        }

        if (!TryGetAdapterInfoFromD3D11Device(sourceDevicePtr, out D3D11AdapterInfo sourceAdapterInfo, out string sourceAdapterError))
        {
            error = $"Could not read source adapter LUID: {sourceAdapterError}";
            return false;
        }

        if (!TryFindSystemAdapterByLuid(sourceAdapterInfo, out IntPtr adapterPtr, out D3D11AdapterInfo systemAdapterInfo, out string findError))
        {
            error = findError;
            return false;
        }

        try
        {
            int createHr = TryCreateDevice(adapterPtr, D3D11CreateDeviceBgraSupport, out devicePtr);
            if (createHr < 0 || devicePtr == IntPtr.Zero)
            {
                int noFlagsHr = TryCreateDevice(adapterPtr, 0, out devicePtr);
                if (noFlagsHr < 0 || devicePtr == IntPtr.Zero)
                {
                    error =
                        $"System32 D3D11CreateDevice(same adapter by LUID) failed: " +
                        $"source=[{sourceAdapterInfo.DiagnosticSummary}], system=[{systemAdapterInfo.DiagnosticSummary}], " +
                        $"bgra=0x{createHr:X8}, default=0x{noFlagsHr:X8}";
                    return false;
                }
            }

            return true;
        }
        finally
        {
            Marshal.Release(adapterPtr);
        }
    }

    private static bool TryFindSystemAdapterByLuid(
        D3D11AdapterInfo sourceAdapterInfo,
        out IntPtr adapterPtr,
        out D3D11AdapterInfo systemAdapterInfo,
        out string error)
    {
        adapterPtr = IntPtr.Zero;
        systemAdapterInfo = default;
        error = string.Empty;

        Guid factoryIid = IID_IDXGIFactory1;
        int factoryHr = CreateDXGIFactory1(in factoryIid, out IntPtr factoryPtr);
        if (factoryHr < 0 || factoryPtr == IntPtr.Zero)
        {
            error =
                $"System32 CreateDXGIFactory1 failed: 0x{factoryHr:X8}; " +
                $"source=[{sourceAdapterInfo.DiagnosticSummary}]";
            return false;
        }

        string enumeratedAdapters = string.Empty;
        try
        {
            void** factoryVtable = *(void***)factoryPtr;
            // IDXGIFactory1::EnumAdapters1 is slot 12.
            var enumAdapters1 = (delegate* unmanaged[Stdcall]<void*, uint, void**, int>)factoryVtable[12];
            for (uint index = 0; ; index++)
            {
                void* candidate = null;
                int enumHr = enumAdapters1((void*)factoryPtr, index, &candidate);
                if (enumHr == DxgiErrorNotFound)
                    break;
                if (enumHr < 0 || candidate == null)
                {
                    error =
                        $"System32 IDXGIFactory1::EnumAdapters1({index}) failed: 0x{enumHr:X8}; " +
                        $"source=[{sourceAdapterInfo.DiagnosticSummary}], systemAdapters=[{enumeratedAdapters}]";
                    return false;
                }

                try
                {
                    void** adapterVtable = *(void***)candidate;
                    // IDXGIAdapter1::GetDesc1 is slot 10.
                    var getDesc1 = (delegate* unmanaged[Stdcall]<void*, DxgiAdapterDesc1*, int>)adapterVtable[10];
                    DxgiAdapterDesc1 desc = default;
                    int descHr = getDesc1(candidate, &desc);
                    if (descHr < 0)
                    {
                        error =
                            $"System32 IDXGIAdapter1::GetDesc1({index}) failed: 0x{descHr:X8}; " +
                            $"source=[{sourceAdapterInfo.DiagnosticSummary}], systemAdapters=[{enumeratedAdapters}]";
                        return false;
                    }

                    D3D11AdapterInfo candidateInfo = new(
                        desc.VendorId,
                        desc.DeviceId,
                        desc.SubSysId,
                        desc.Revision,
                        ReadAdapterDescription(desc),
                        desc.AdapterLuidHighPart,
                        desc.AdapterLuidLowPart);
                    if (enumeratedAdapters.Length > 0)
                        enumeratedAdapters += " | ";
                    enumeratedAdapters += $"#{index} {candidateInfo.DiagnosticSummary}";

                    if (candidateInfo.AdapterLuidHighPart != sourceAdapterInfo.AdapterLuidHighPart ||
                        candidateInfo.AdapterLuidLowPart != sourceAdapterInfo.AdapterLuidLowPart)
                    {
                        continue;
                    }

                    adapterPtr = (IntPtr)candidate;
                    candidate = null;
                    systemAdapterInfo = candidateInfo;
                    return true;
                }
                finally
                {
                    if (candidate != null)
                        Marshal.Release((IntPtr)candidate);
                }
            }

            error =
                $"System32 DXGI factory did not find the source adapter LUID; " +
                $"source=[{sourceAdapterInfo.DiagnosticSummary}], systemAdapters=[{enumeratedAdapters}]";
            return false;
        }
        finally
        {
            Marshal.Release(factoryPtr);
        }
    }

    public static bool TryOpenSharedTexture(
        IntPtr devicePtr,
        IntPtr sharedHandle,
        bool ntHandle,
        out IntPtr texturePtr,
        out string error)
    {
        texturePtr = IntPtr.Zero;
        error = string.Empty;

        if (devicePtr == IntPtr.Zero || sharedHandle == IntPtr.Zero)
        {
            error = "D3D11 device or shared handle is null";
            return false;
        }

        if (ntHandle)
        {
            if (!TryQueryInterface(devicePtr, IID_ID3D11Device1, out IntPtr device1Ptr))
            {
                error = "ID3D11Device::QueryInterface(ID3D11Device1) failed";
                return false;
            }

            try
            {
                void** vtable = *(void***)device1Ptr;
                // ID3D11Device1::OpenSharedResource1 is slot 48.
                var openSharedResource1 = (delegate* unmanaged[Stdcall]<void*, IntPtr, Guid*, void**, int>)vtable[48];
                Guid iid = IID_ID3D11Texture2D;
                void* openedTexture = null;
                int hr = openSharedResource1((void*)device1Ptr, sharedHandle, &iid, &openedTexture);
                if (hr < 0 || openedTexture == null)
                {
                    error = $"ID3D11Device1::OpenSharedResource1(ID3D11Texture2D) failed: 0x{hr:X8}";
                    return false;
                }

                texturePtr = (IntPtr)openedTexture;
                return true;
            }
            finally
            {
                Marshal.Release(device1Ptr);
            }
        }

        void** deviceVtable = *(void***)devicePtr;
        var openSharedResource = (delegate* unmanaged[Stdcall]<void*, IntPtr, Guid*, void**, int>)deviceVtable[28];
        Guid legacyIid = IID_ID3D11Texture2D;
        void* legacyTexture = null;
        int legacyHr = openSharedResource((void*)devicePtr, sharedHandle, &legacyIid, &legacyTexture);
        if (legacyHr < 0 || legacyTexture == null)
        {
            error = $"ID3D11Device::OpenSharedResource(ID3D11Texture2D) failed: 0x{legacyHr:X8}";
            return false;
        }

        texturePtr = (IntPtr)legacyTexture;
        return true;
    }

    public static void CloseSharedNtHandle(IntPtr sharedHandle)
    {
        if (sharedHandle != IntPtr.Zero)
            _ = CloseHandle(sharedHandle);
    }

    public static bool TryQueryKeyedMutex(IntPtr texturePtr, out IntPtr mutexPtr)
        => TryQueryInterface(texturePtr, IID_IDXGIKeyedMutex, out mutexPtr);

    public static int AcquireKeyedMutex(IntPtr mutexPtr, ulong key, uint milliseconds)
    {
        if (mutexPtr == IntPtr.Zero)
            return unchecked((int)0x80004003);

        void** vtable = *(void***)mutexPtr;
        var acquireSync = (delegate* unmanaged[Stdcall]<void*, ulong, uint, int>)vtable[8];
        return acquireSync((void*)mutexPtr, key, milliseconds);
    }

    public static int ReleaseKeyedMutex(IntPtr mutexPtr, ulong key)
    {
        if (mutexPtr == IntPtr.Zero)
            return unchecked((int)0x80004003);

        void** vtable = *(void***)mutexPtr;
        var releaseSync = (delegate* unmanaged[Stdcall]<void*, ulong, int>)vtable[9];
        return releaseSync((void*)mutexPtr, key);
    }

    private static int TryCreateDevice(IntPtr adapterPtr, uint flags, out IntPtr devicePtr)
    {
        devicePtr = IntPtr.Zero;
        IntPtr immediateContextPtr = IntPtr.Zero;
        try
        {
            int hr = D3D11CreateDevice(
                adapterPtr,
                D3DDriverTypeUnknown,
                IntPtr.Zero,
                flags,
                IntPtr.Zero,
                0,
                D3D11SdkVersion,
                out devicePtr,
                out _,
                out immediateContextPtr);
            if (hr < 0 && devicePtr != IntPtr.Zero)
            {
                Marshal.Release(devicePtr);
                devicePtr = IntPtr.Zero;
            }

            return hr;
        }
        finally
        {
            if (immediateContextPtr != IntPtr.Zero)
                Marshal.Release(immediateContextPtr);
        }
    }

    private static bool TryQueryInterface(IntPtr unknownPtr, Guid iid, out IntPtr interfacePtr)
    {
        interfacePtr = IntPtr.Zero;
        if (unknownPtr == IntPtr.Zero)
            return false;

        int hr = Marshal.QueryInterface(unknownPtr, in iid, out interfacePtr);
        return hr >= 0 && interfacePtr != IntPtr.Zero;
    }

    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int D3D11CreateDevice(
        IntPtr adapter,
        int driverType,
        IntPtr software,
        uint flags,
        IntPtr featureLevels,
        uint featureLevelCount,
        uint sdkVersion,
        out IntPtr device,
        out int featureLevel,
        out IntPtr immediateContext);

    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [DllImport("dxgi.dll", ExactSpelling = true)]
    private static extern int CreateDXGIFactory1(in Guid riid, out IntPtr factory);

    [DllImport("kernel32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    private static string ReadAdapterDescription(DxgiAdapterDesc desc)
    {
        char[] chars = new char[128];
        int length = 0;
        while (length < chars.Length)
        {
            char c = desc.Description[length];
            if (c == '\0')
                break;

            chars[length] = c;
            length++;
        }

        return new string(chars, 0, length);
    }

    private static string ReadAdapterDescription(DxgiAdapterDesc1 desc)
    {
        char[] chars = new char[128];
        int length = 0;
        while (length < chars.Length)
        {
            char c = desc.Description[length];
            if (c == '\0')
                break;

            chars[length] = c;
            length++;
        }

        return new string(chars, 0, length);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DxgiAdapterDesc
    {
        public fixed char Description[128];
        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public nuint DedicatedVideoMemory;
        public nuint DedicatedSystemMemory;
        public nuint SharedSystemMemory;
        public uint AdapterLuidLowPart;
        public int AdapterLuidHighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DxgiAdapterDesc1
    {
        public fixed char Description[128];
        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public nuint DedicatedVideoMemory;
        public nuint DedicatedSystemMemory;
        public nuint SharedSystemMemory;
        public uint AdapterLuidLowPart;
        public int AdapterLuidHighPart;
        public uint Flags;
    }
}

internal readonly record struct D3D11AdapterInfo(
    uint VendorId,
    uint DeviceId,
    uint SubSysId,
    uint Revision,
    string AdapterName,
    int AdapterLuidHighPart,
    uint AdapterLuidLowPart)
{
    public string Vendor => VendorId switch
    {
        0x10DE => "nvidia",
        0x1002 => "amd",
        0x8086 => "intel",
        _ => "unknown",
    };

    public string DiagnosticSummary
        => $"vendor={Vendor}, vendorId=0x{VendorId:X4}, deviceId=0x{DeviceId:X4}, adapter={ValueOrNone(AdapterName)}, luid={AdapterLuidHighPart}:{AdapterLuidLowPart}";

    private static string ValueOrNone(string? value)
        => string.IsNullOrWhiteSpace(value) ? "<none>" : value;
}
