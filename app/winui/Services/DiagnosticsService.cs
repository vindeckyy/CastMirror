using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace CastMirror.Services
{
    /// <summary>
    /// Builds the text a user pastes into a bug report: versions, the settings in
    /// force, and the tail of both logs. Nothing is sent anywhere; the caller puts
    /// it on the clipboard. Network addresses are masked so a report does not
    /// reveal the reporter's LAN.
    /// </summary>
    public static class DiagnosticsService
    {
        private const int TailLines = 200;

        private static readonly Regex Ipv4 =
            new(@"\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.\d{1,3}\b", RegexOptions.Compiled);

        private static readonly Regex Ansi =
            new("\u001b\\[[0-9;]*m", RegexOptions.Compiled);

        public static string MaskAddresses(string text) => Ipv4.Replace(text, "$1.$2.$3.x");

        public static string StripAnsi(string text) => Ansi.Replace(text, string.Empty);

        /// <summary>Last <paramref name="count"/> lines of a file another process may be writing.</summary>
        public static IReadOnlyList<string> TailOf(string path, int count)
        {
            var lines = new List<string>();
            try
            {
                if (!File.Exists(path)) return lines;
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                                                  FileShare.ReadWrite | FileShare.Delete);
                // The logs rotate at a few hundred KB, so reading the whole file is cheap.
                using var reader = new StreamReader(stream, Encoding.UTF8);
                var window = new Queue<string>(count + 1);
                string? line;
                while ((line = reader.ReadLine()) != null)
                {
                    window.Enqueue(StripAnsi(line));
                    if (window.Count > count) window.Dequeue();
                }
                lines.AddRange(window);
            }
            catch (Exception ex)
            {
                LogService.Log($"[diagnostics] could not read {path}: {ex.Message}");
            }
            return lines;
        }

        public static string EngineLogPath => Path.Combine(LogService.DirectoryPath, "castmirror.log");

        public static string Build(string settingsJson)
        {
            var text = new StringBuilder();
            text.AppendLine("CastMirror diagnostics");
            text.AppendLine($"Version: {typeof(DiagnosticsService).Assembly.GetName().Version?.ToString(3)}");
            text.AppendLine($"Windows: {RuntimeInformation.OSDescription} ({RuntimeInformation.OSArchitecture})");
            text.AppendLine($".NET: {RuntimeInformation.FrameworkDescription}");
            text.AppendLine($"Generated: {DateTime.Now:O}");
            text.AppendLine();
            text.AppendLine("Settings");
            text.AppendLine(string.IsNullOrEmpty(settingsJson) ? "(unavailable)" : settingsJson);
            text.AppendLine();
            AppendTail(text, "Engine log", EngineLogPath);
            AppendTail(text, "App log", LogService.LogFilePath);
            return MaskAddresses(text.ToString());
        }

        private static void AppendTail(StringBuilder text, string title, string path)
        {
            text.AppendLine($"{title} (last {TailLines} lines)");
            var lines = TailOf(path, TailLines);
            if (lines.Count == 0) text.AppendLine("(empty)");
            foreach (string line in lines) text.AppendLine(line);
            text.AppendLine();
        }
    }
}
