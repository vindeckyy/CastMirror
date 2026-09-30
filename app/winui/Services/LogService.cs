using System;
using System.IO;
using System.Text;

namespace CastMirror.Services
{
    /// <summary>
    /// Single owner of the on-disk diagnostic log under
    /// %APPDATA%\CastMirror\gui-errors.log. Appends are size-capped so a long
    /// session (or an error loop) cannot grow the file without bound: once the
    /// file passes <see cref="MaxBytes"/> it is rewritten with its newest half.
    /// Every call is best-effort — diagnostics must never break the app.
    /// </summary>
    public static class LogService
    {
        /// <summary>Upper bound of the log file, roughly 256 KB.</summary>
        public const int MaxBytes = 256 * 1024;

        private static readonly object Sync = new();

        /// <summary>Directory that holds the log file (and any future state).</summary>
        public static string DirectoryPath =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "CastMirror");

        /// <summary>Full path of the shared error log.</summary>
        public static string LogFilePath => Path.Combine(DirectoryPath, "gui-errors.log");

        /// <summary>Appends one timestamped line to the shared log.</summary>
        public static void Log(string line)
        {
            try
            {
                lock (Sync)
                {
                    Directory.CreateDirectory(DirectoryPath);
                    string path = LogFilePath;
                    TrimIfOverCap(path);
                    File.AppendAllText(path, $"{DateTime.Now:O} {line}{Environment.NewLine}");
                }
            }
            catch
            {
                // Diagnostics must never break startup or the calling thread.
            }
        }

        /// <summary>
        /// Keeps the newest ~half of the file when it exceeds the cap, starting
        /// after a line boundary so no partial line survives at the head.
        /// </summary>
        private static void TrimIfOverCap(string path)
        {
            FileInfo info = new(path);
            if (!info.Exists || info.Length <= MaxBytes) return;

            string tail;
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                stream.Seek(stream.Length - MaxBytes / 2, SeekOrigin.Begin);
                using var reader = new StreamReader(stream, Encoding.UTF8);
                reader.ReadLine(); // drop the partial line the seek landed in
                tail = reader.ReadToEnd();
            }
            File.WriteAllText(path, tail);
        }
    }
}
