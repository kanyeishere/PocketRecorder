using System;
using System.Runtime.InteropServices;

namespace Recorder.Capture;

internal static unsafe class D3D11InteropHelpers
{
    private static readonly Guid IID_IDXGIResource = new(0x035F3AB4, 0x482E, 0x4E50, 0xB4, 0x1F, 0x8A, 0x7F, 0x8B, 0xD8, 0x96, 0x0B);
    private static readonly Guid IID_IDXGIKeyedMutex = new(0x9D8E1289, 0xD7B3, 0x465F, 0x81, 0x26, 0x25, 0x0E, 0x34, 0x9A, 0xF8, 0x5D);

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

    private static bool TryQueryInterface(IntPtr unknownPtr, Guid iid, out IntPtr interfacePtr)
    {
        interfacePtr = IntPtr.Zero;
        if (unknownPtr == IntPtr.Zero)
            return false;

        int hr = Marshal.QueryInterface(unknownPtr, in iid, out interfacePtr);
        return hr >= 0 && interfacePtr != IntPtr.Zero;
    }
}
