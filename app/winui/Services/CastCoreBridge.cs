using System;
using System.Runtime.InteropServices;

namespace CastMirror.Services
{
    public enum CastMirrorState
    {
        Idle = 0,
        Connecting = 1,
        Negotiating = 2,
        Streaming = 3,
        Reconnecting = 4,
        Stopping = 5,
        Failed = 6
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct CastMirrorDeviceInfo
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string IpAddress;
        public ushort Port;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string ModelName;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CastMirrorStreamStats
    {
        public uint BitrateKbps;
        public double CurrentFps;
        public double RoundTripTimeMs;
        public double PacketLossFraction;
        public int TargetDelayMs;
        public int Width;
        public int Height;
        public ulong FramesSent;
        public ulong PacketsSent;
        public ulong VideoQueueOverruns;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void StateCallback(CastMirrorState state, [MarshalAs(UnmanagedType.LPStr)] string message, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void DevicesCallback(int count, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void StatsCallback(ref CastMirrorStreamStats stats, IntPtr userData);

    public static class CastCoreBridge
    {
        private const string LibName = "castcore";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_init();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_shutdown();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_start_discovery();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_stop_discovery();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_device_count();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_get_device_info(int index, out CastMirrorDeviceInfo outInfo);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_start_cast([MarshalAs(UnmanagedType.LPStr)] string deviceId, int displayId, int targetFps, uint bitrateKbps);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_stop_cast();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern CastMirrorState castmirror_get_state();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_get_stats(out CastMirrorStreamStats outStats);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_bitrate(uint bitrateKbps);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_playout_delay(int delayMs);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_freeze([MarshalAs(UnmanagedType.I1)] bool freeze);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_muted([MarshalAs(UnmanagedType.I1)] bool muted);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_state_callback(StateCallback cb, IntPtr userData);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_devices_callback(DevicesCallback cb, IntPtr userData);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_stats_callback(StatsCallback cb, IntPtr userData);
    }
}
