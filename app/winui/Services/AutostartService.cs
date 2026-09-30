using System;
using Microsoft.Win32;

namespace CastMirror.Services
{
    /// <summary>
    /// "Start CastMirror when I sign in", kept in the per-user Run key so it needs
    /// no administrator rights and disappears with the user profile. The registry
    /// is the source of truth: the installer can set the same value, and the
    /// Settings switch reads it back instead of keeping a second copy.
    /// </summary>
    public static class AutostartService
    {
        private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        private const string ValueName = "CastMirror";

        /// <summary>Command-line flag that starts the app hidden in the tray.</summary>
        public const string BackgroundFlag = "--background";

        public static bool IsEnabled
        {
            get
            {
                try
                {
                    using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKey);
                    return key?.GetValue(ValueName) is string;
                }
                catch (Exception ex)
                {
                    LogService.Log($"[autostart] read failed: {ex.Message}");
                    return false;
                }
            }
        }

        public static bool Set(bool enabled)
        {
            try
            {
                using RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey);
                if (enabled)
                {
                    string? exe = Environment.ProcessPath;
                    if (string.IsNullOrEmpty(exe)) return false;
                    key.SetValue(ValueName, $"\"{exe}\" {BackgroundFlag}");
                }
                else
                {
                    key.DeleteValue(ValueName, throwOnMissingValue: false);
                }
                return true;
            }
            catch (Exception ex)
            {
                LogService.Log($"[autostart] write failed: {ex.Message}");
                return false;
            }
        }
    }
}
