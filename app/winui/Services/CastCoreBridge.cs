using System;
using System.Runtime.InteropServices;
using System.Text;

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

    public enum CastMirrorSourceKind
    {
        Monitor = 0,
        Window = 1
    }

    public enum CastMirrorQualityPreset
    {
        Auto = 0,
        High = 1,
        Balanced = 2,
        Smooth = 3,
        Game = 4,
        Cinema = 5
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

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
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
        // Appended in v2: order mirrors castcore/c_api.h.
        public int CurrentFramerate;
        public int AdaptiveRungIndex;
        public int AdaptiveRungCount;
        public int AdaptiveEnabled;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string EncoderName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string CaptureBackend;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DisplayName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct CastMirrorDisplayInfo
    {
        public int Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Name;
        public int X;
        public int Y;
        public int Width;
        public int Height;
        public int RefreshRate;
        [MarshalAs(UnmanagedType.I1)]
        public bool IsPrimary;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct CastMirrorWindowInfo
    {
        public int Id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Title;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string AppClass;
        public int X;
        public int Y;
        public int Width;
        public int Height;
        [MarshalAs(UnmanagedType.I1)]
        public bool Visible;
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
        public static extern int castmirror_get_display_count();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_get_display_info(int index, out CastMirrorDisplayInfo outInfo);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_window_count();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_get_window_info(int index, out CastMirrorWindowInfo outInfo);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_window_capture_supported();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_start_cast([MarshalAs(UnmanagedType.LPStr)] string deviceId, int displayId, int targetFps, uint bitrateKbps);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_start_cast_ex([MarshalAs(UnmanagedType.LPStr)] string deviceId, int sourceKind, int sourceId, int targetFps, uint bitrateKbps, int preset, [MarshalAs(UnmanagedType.I1)] bool audioEnabled);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_stop_cast();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern CastMirrorState castmirror_get_state();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_get_stats(out CastMirrorStreamStats outStats);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_last_error([Out, MarshalAs(UnmanagedType.LPStr)] StringBuilder buffer, int bufLen);

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

        // Settings are read/written as JSON so the app and the engine share one
        // schema (same keys as %APPDATA%\CastMirror\config.json).
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_config_json(
            [Out, MarshalAs(UnmanagedType.LPStr)] StringBuilder buffer, int bufLen);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_set_config_json([MarshalAs(UnmanagedType.LPStr)] string json);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_self_test(
            [Out, MarshalAs(UnmanagedType.LPStr)] StringBuilder buffer, int bufLen);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_rescan();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_audio_bitrate(uint bitrateBps);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_adaptive_resolution_allowed([MarshalAs(UnmanagedType.I1)] bool allow);
    }
}
