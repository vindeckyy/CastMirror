using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace CastMirror.Services
{
    /// <summary>
    /// Writes a minidump when the process crashes, including crashes inside the
    /// native engine that .NET never sees, and tells the user on the next launch.
    /// Nothing is uploaded: the dump stays in %APPDATA%\CastMirror\crashes and the
    /// user decides whether to attach it to a bug report. A dump holds process
    /// memory, so the message says so.
    /// </summary>
    public static class CrashHandler
    {
        private const int KeepDumps = 5;
        private const string MarkerName = "last-crash.txt";

        private delegate int UnhandledExceptionFilter(IntPtr exceptionPointers);

        [DllImport("kernel32.dll")]
        private static extern IntPtr SetUnhandledExceptionFilter(UnhandledExceptionFilter? filter);

        [DllImport("dbghelp.dll", SetLastError = true)]
        private static extern bool MiniDumpWriteDump(IntPtr process, uint processId, IntPtr file, int dumpType,
                                                     ref MiniDumpExceptionInformation exception,
                                                     IntPtr userStream, IntPtr callback);

        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        private struct MiniDumpExceptionInformation
        {
            public uint ThreadId;
            public IntPtr ExceptionPointers;
            [MarshalAs(UnmanagedType.Bool)] public bool ClientPointers;
        }

        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();

        // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo: enough to see
        // the stack and the objects it points at, without a multi-hundred-MB full dump.
        private const int DumpType = 0x00000040 | 0x00001000;

        // Kept in a static so the delegate outlives Install(); a collected delegate
        // would turn the crash handler into a second crash.
        private static UnhandledExceptionFilter? _filter;

        public static string CrashDirectory => Path.Combine(LogService.DirectoryPath, "crashes");

        public static void Install()
        {
            try
            {
                _filter = OnCrash;
                SetUnhandledExceptionFilter(_filter);
            }
            catch (Exception ex)
            {
                LogService.Log($"[crash] handler not installed: {ex.Message}");
            }
        }

        private static int OnCrash(IntPtr exceptionPointers)
        {
            try
            {
                Directory.CreateDirectory(CrashDirectory);
                string path = Path.Combine(CrashDirectory, $"crash-{DateTime.Now:yyyyMMdd-HHmmss}.dmp");
                using (var file = new FileStream(path, FileMode.Create, FileAccess.Write))
                {
                    var info = new MiniDumpExceptionInformation
                    {
                        ThreadId = GetCurrentThreadId(),
                        ExceptionPointers = exceptionPointers,
                        ClientPointers = false
                    };
                    using Process process = Process.GetCurrentProcess();
                    MiniDumpWriteDump(process.Handle, (uint)process.Id, file.SafeFileHandle.DangerousGetHandle(),
                                      DumpType, ref info, IntPtr.Zero, IntPtr.Zero);
                }
                File.WriteAllText(Path.Combine(LogService.DirectoryPath, MarkerName), path);
                PruneOldDumps(CrashDirectory, KeepDumps);
            }
            catch
            {
                // Already crashing: never throw from here.
            }
            // 0 = EXCEPTION_CONTINUE_SEARCH: let Windows finish the crash as usual.
            return 0;
        }

        /// <summary>Deletes all but the newest <paramref name="keep"/> dump files.</summary>
        public static void PruneOldDumps(string directory, int keep)
        {
            try
            {
                if (!Directory.Exists(directory)) return;
                foreach (FileInfo old in new DirectoryInfo(directory).GetFiles("crash-*.dmp")
                             .OrderByDescending(f => f.LastWriteTimeUtc).Skip(keep))
                {
                    old.Delete();
                }
            }
            catch (Exception ex)
            {
                LogService.Log($"[crash] prune failed: {ex.Message}");
            }
        }

        /// <summary>
        /// The dump left by the previous run, or null. Reading it clears the marker,
        /// so each crash is reported once.
        /// </summary>
        public static string? TakePreviousCrash(string? directory = null)
        {
            try
            {
                string marker = Path.Combine(directory ?? LogService.DirectoryPath, MarkerName);
                if (!File.Exists(marker)) return null;
                string dump = File.ReadAllText(marker).Trim();
                File.Delete(marker);
                return dump.Length == 0 ? null : dump;
            }
            catch (Exception ex)
            {
                LogService.Log($"[crash] marker unreadable: {ex.Message}");
                return null;
            }
        }
    }
}
