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

    /// <summary>
    /// The native string convention of castcore: NUL-terminated UTF-8.
    /// </summary>
    /// <remarks>
    /// castcore writes UTF-8 (window titles and display names are converted with
    /// WideCharToMultiByte(CP_UTF8); device names are the raw mDNS TXT bytes), so
    /// the fixed-size buffers below are declared as byte[] and decoded here.
    /// Declaring them as CharSet.Ansi strings instead decodes them with the
    /// *system* ANSI code page: on a CP-1252 machine "Кухня TV" arrives as
    /// "ÐšÑƒÑ…Ð½Ñ TV" in the device list, the window picker and the log pane.
    /// </remarks>
    public static class NativeText
    {
        /// <summary>Decodes a fixed-size UTF-8 buffer up to its NUL terminator.</summary>
        public static string Decode(byte[] buffer)
        {
            if (buffer is null) return string.Empty;
            int end = Array.IndexOf(buffer, (byte)0);
            if (end < 0) end = buffer.Length;
            return end == 0 ? string.Empty : Encoding.UTF8.GetString(buffer, 0, end);
        }

        /// <summary>
        /// Encodes a string as NUL-terminated UTF-8 for a <c>const char*</c>
        /// parameter. The incoming pointer is borrowed for the duration of the
        /// call, which is exactly what the array marshaller provides.
        /// </summary>
        public static byte[] EncodeZ(string? value)
        {
            int length = Encoding.UTF8.GetByteCount(value ?? string.Empty);
            var buffer = new byte[length + 1];
            if (length > 0)
            {
                Encoding.UTF8.GetBytes(value!, 0, value!.Length, buffer, 0);
            }
            return buffer;  // the trailing byte stays 0
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CastMirrorDeviceInfo
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
        public byte[] IdUtf8;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
        public byte[] NameUtf8;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
        public byte[] IpAddressUtf8;
        public ushort Port;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
        public byte[] ModelNameUtf8;

        public string Id => NativeText.Decode(IdUtf8);
        public string Name => NativeText.Decode(NameUtf8);
        public string IpAddress => NativeText.Decode(IpAddressUtf8);
        public string ModelName => NativeText.Decode(ModelNameUtf8);
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
        // Appended in v2: order mirrors castcore/c_api.h.
        public int CurrentFramerate;
        public int AdaptiveRungIndex;
        public int AdaptiveRungCount;
        public int AdaptiveEnabled;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
        public byte[] EncoderNameUtf8;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)]
        public byte[] CaptureBackendUtf8;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
        public byte[] DisplayNameUtf8;
        // Appended in v3.
        public int RecoveryAttempt;
        public int RecoveryElapsedSeconds;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)]
        public byte[] HealthHintUtf8;

        public string EncoderName => NativeText.Decode(EncoderNameUtf8);
        public string CaptureBackend => NativeText.Decode(CaptureBackendUtf8);
        public string DisplayName => NativeText.Decode(DisplayNameUtf8);
        public string HealthHint => NativeText.Decode(HealthHintUtf8);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CastMirrorDisplayInfo
    {
        public int Id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
        public byte[] NameUtf8;
        public int X;
        public int Y;
        public int Width;
        public int Height;
        public int RefreshRate;
        [MarshalAs(UnmanagedType.I1)]
        public bool IsPrimary;

        public string Name => NativeText.Decode(NameUtf8);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct CastMirrorWindowInfo
    {
        public int Id;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)]
        public byte[] TitleUtf8;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 128)]
        public byte[] AppClassUtf8;
        public int X;
        public int Y;
        public int Width;
        public int Height;
        [MarshalAs(UnmanagedType.I1)]
        public bool Visible;

        public string Title => NativeText.Decode(TitleUtf8);
        public string AppClass => NativeText.Decode(AppClassUtf8);
    }

    // Native callbacks hand over NUL-terminated UTF-8 as a borrowed pointer, so
    // the delegates take IntPtr and decode with Marshal.PtrToStringUTF8 - an
    // LPStr string parameter would decode it with the ANSI code page instead.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void StateCallback(CastMirrorState state, IntPtr message, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void DevicesCallback(int count, IntPtr userData);

    // [In] is load-bearing: without it a non-blittable ref value type defaults
    // to [In, Out] and the marshaller copies the whole 512-byte struct (plus the
    // three UTF-8 buffers) BACK into the native buffer after every tick. The core
    // passes a writable stack local today, but a const in .rdata would fault.
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void StatsCallback([In] ref CastMirrorStreamStats stats, IntPtr userData);

    // level is a castcore::LogLevel ordinal (0 Debug .. 4 Fatal).
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void LogCallback(int level, IntPtr message, IntPtr userData);

    public static class CastCoreBridge
    {
        private const string LibName = "castcore";

        /// <summary>
        /// ABI version this client was compiled against. Must equal
        /// <see cref="castmirror_abi_version"/> in the loaded castcore.dll; the
        /// guard below enforces it. Bump together with CASTMIRROR_ABI_VERSION.
        /// </summary>
        public const uint ExpectedAbiVersion = 3;

        /// <summary>
        /// Verifies the native ABI before any call crosses the boundary.
        /// </summary>
        /// <remarks>
        /// The structs below are flattened by hand and marshalled by offset, so
        /// a field inserted, reordered or widened in castcore corrupts managed
        /// memory silently - there is no error at either end of the boundary.
        /// c_api.h pins the native layout with static_asserts; these are the
        /// matching numbers on the managed side, and a static constructor is the
        /// only place that runs them exactly once, before the first P/Invoke.
        /// </remarks>
        static CastCoreBridge()
        {
            VerifyLayout("CastMirrorStreamStats", Marshal.SizeOf<CastMirrorStreamStats>(), 512,
                (nameof(CastMirrorStreamStats.BitrateKbps), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.BitrateKbps)).ToInt32(), 0),
                (nameof(CastMirrorStreamStats.FramesSent), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.FramesSent)).ToInt32(), 48),
                (nameof(CastMirrorStreamStats.AdaptiveEnabled), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.AdaptiveEnabled)).ToInt32(), 84),
                (nameof(CastMirrorStreamStats.EncoderNameUtf8), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.EncoderNameUtf8)).ToInt32(), 88),
                (nameof(CastMirrorStreamStats.DisplayNameUtf8), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.DisplayNameUtf8)).ToInt32(), 216),
                (nameof(CastMirrorStreamStats.RecoveryAttempt), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.RecoveryAttempt)).ToInt32(), 344),
                (nameof(CastMirrorStreamStats.HealthHintUtf8), Marshal.OffsetOf<CastMirrorStreamStats>(nameof(CastMirrorStreamStats.HealthHintUtf8)).ToInt32(), 352));
            VerifyLayout("CastMirrorDeviceInfo", Marshal.SizeOf<CastMirrorDeviceInfo>(), 450,
                (nameof(CastMirrorDeviceInfo.IdUtf8), Marshal.OffsetOf<CastMirrorDeviceInfo>(nameof(CastMirrorDeviceInfo.IdUtf8)).ToInt32(), 0),
                (nameof(CastMirrorDeviceInfo.Port), Marshal.OffsetOf<CastMirrorDeviceInfo>(nameof(CastMirrorDeviceInfo.Port)).ToInt32(), 320),
                (nameof(CastMirrorDeviceInfo.ModelNameUtf8), Marshal.OffsetOf<CastMirrorDeviceInfo>(nameof(CastMirrorDeviceInfo.ModelNameUtf8)).ToInt32(), 322));
            VerifyLayout("CastMirrorDisplayInfo", Marshal.SizeOf<CastMirrorDisplayInfo>(), 156,
                (nameof(CastMirrorDisplayInfo.NameUtf8), Marshal.OffsetOf<CastMirrorDisplayInfo>(nameof(CastMirrorDisplayInfo.NameUtf8)).ToInt32(), 4),
                (nameof(CastMirrorDisplayInfo.RefreshRate), Marshal.OffsetOf<CastMirrorDisplayInfo>(nameof(CastMirrorDisplayInfo.RefreshRate)).ToInt32(), 148),
                (nameof(CastMirrorDisplayInfo.IsPrimary), Marshal.OffsetOf<CastMirrorDisplayInfo>(nameof(CastMirrorDisplayInfo.IsPrimary)).ToInt32(), 152));
            VerifyLayout("CastMirrorWindowInfo", Marshal.SizeOf<CastMirrorWindowInfo>(), 408,
                (nameof(CastMirrorWindowInfo.TitleUtf8), Marshal.OffsetOf<CastMirrorWindowInfo>(nameof(CastMirrorWindowInfo.TitleUtf8)).ToInt32(), 4),
                (nameof(CastMirrorWindowInfo.X), Marshal.OffsetOf<CastMirrorWindowInfo>(nameof(CastMirrorWindowInfo.X)).ToInt32(), 388),
                (nameof(CastMirrorWindowInfo.Visible), Marshal.OffsetOf<CastMirrorWindowInfo>(nameof(CastMirrorWindowInfo.Visible)).ToInt32(), 404));
        }

        private static void VerifyLayout(string structName, int size, int expectedSize,
            params (string Field, int Offset, int ExpectedOffset)[] fields)
        {
            if (size != expectedSize)
            {
                throw new InvalidOperationException(
                    $"{structName} marshals to {size} bytes but castcore expects {expectedSize}. " +
                    "The managed mirror and core/include/castcore/c_api.h have diverged.");
            }
            foreach (var field in fields)
            {
                if (field.Offset != field.ExpectedOffset)
                {
                    throw new InvalidOperationException(
                        $"{structName}.{field.Field} marshals at offset {field.Offset} but castcore " +
                        $"expects {field.ExpectedOffset}. The managed mirror and " +
                        "core/include/castcore/c_api.h have diverged.");
                }
            }
        }

        /// <summary>
        /// Throws if the loaded castcore.dll does not implement the ABI this
        /// client was compiled against. Call once at startup, before any other
        /// entry point, and surface the message to the user.
        /// </summary>
        public static void VerifyNativeAbi()
        {
            uint version = castmirror_abi_version();
            if (version != ExpectedAbiVersion)
            {
                throw new InvalidOperationException(
                    $"castcore.dll reports ABI version {version} but CastMirror was built for " +
                    $"{ExpectedAbiVersion}. Reinstall CastMirror so CastMirror.exe and castcore.dll " +
                    "come from the same build.");
            }
        }

        /// <summary>
        /// The engine's last-error message (UTF-8), or null when none is
        /// recorded. Errors are short by contract, so one 1 KiB read suffices;
        /// a fuller message is truncated at the buffer's NUL, never mojibake.
        /// </summary>
        public static string? GetLastError()
        {
            var buffer = new byte[1024];
            int length = castmirror_get_last_error(buffer, buffer.Length);
            return length > 0 ? NativeText.Decode(buffer) : null;
        }


        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint castmirror_abi_version();

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
        public static extern bool castmirror_start_cast([In] byte[] deviceIdUtf8, int displayId, int targetFps, uint bitrateKbps);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_start_cast_ex([In] byte[] deviceIdUtf8, int sourceKind, int sourceId, int targetFps, uint bitrateKbps, int preset, [MarshalAs(UnmanagedType.I1)] bool audioEnabled);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_stop_cast();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern CastMirrorState castmirror_get_state();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_get_stats(out CastMirrorStreamStats outStats);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_last_error([In, Out] byte[] buffer, int bufLen);

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

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_log_callback(LogCallback cb, IntPtr userData);

        // Settings are read/written as JSON so the app and the engine share one
        // schema (same keys as %APPDATA%\CastMirror\config.json). The document is
        // UTF-8 on both sides: pass NativeText.EncodeZ(json) so a non-ASCII
        // string value cannot be re-encoded through the ANSI code page.
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_config_json(
            [In, Out] byte[] buffer, int bufLen);

        // JSON array of apps that have an audio session: [{"pid":..,"name":..,"title":..}].
        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_get_audio_apps(
            [In, Out] byte[] buffer, int bufLen);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool castmirror_set_config_json([In] byte[] jsonUtf8);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int castmirror_self_test(
            [In, Out] byte[] buffer, int bufLen);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_rescan();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_audio_bitrate(uint bitrateBps);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void castmirror_set_adaptive_resolution_allowed([MarshalAs(UnmanagedType.I1)] bool allow);
    }
}
