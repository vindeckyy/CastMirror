using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.UI.Dispatching;
using CastMirror.Services;

namespace CastMirror.ViewModels
{
    /// <summary>One captured engine log line.</summary>
    public sealed class LogEntry
    {
        public DateTime Timestamp { get; init; }
        public int Level { get; init; }
        public string LevelName { get; init; } = string.Empty;
        public string Message { get; init; } = string.Empty;

        public string Display => $"{Timestamp:HH:mm:ss.fff} [{LevelName}] {Message}";
    }

    /// <summary>
    /// Live diagnostic log view backed by the engine's log callback
    /// (castmirror_set_log_callback). Keeps a bounded ring of entries so a long
    /// session cannot grow without limit, and exposes level/text filters plus
    /// copy and clear.
    /// </summary>
    public sealed class LogsViewModel : INotifyPropertyChanged
    {
        private const int MaxEntries = 2000;

        private readonly DispatcherQueue? _dispatcher;
        private readonly List<LogEntry> _all = new();
        private readonly LogCallback _logCallback;
        // The engine can still be winding down (its own log callback fires from
        // worker threads) after Detach() unsubscribes us. Without this the
        // already-queued work keeps appending to a window that no longer exists,
        // and any exception it raises is swallowed into the log we just closed.
        private volatile bool _disposed;

        public ObservableCollection<LogEntry> Entries { get; } = new();

        public LogsViewModel()
        {
            _dispatcher = DispatcherQueue.GetForCurrentThread();
            _logCallback = OnLog;
        }

        private static readonly System.Text.RegularExpressions.Regex LogLine = new(
            @"^(?<ts>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(?:\.\d+)?) \[(?<lvl>[A-Z]+)\s*\] (?<msg>.*)$",
            System.Text.RegularExpressions.RegexOptions.Compiled);

        /// <summary>
        /// Fills the view with the tail of the engine's log file, so opening the
        /// window shows what just happened rather than an empty list.
        /// </summary>
        private void SeedFromDisk()
        {
            foreach (string raw in DiagnosticsService.TailOf(DiagnosticsService.EngineLogPath, 300))
            {
                var match = LogLine.Match(raw);
                if (!match.Success) continue;
                int level = Array.IndexOf(LevelNames, match.Groups["lvl"].Value);
                _ = DateTime.TryParse(match.Groups["ts"].Value, out DateTime when);
                var entry = new LogEntry
                {
                    Timestamp = when == default ? DateTime.Now : when,
                    Level = level < 0 ? 1 : level,
                    LevelName = level < 0 ? match.Groups["lvl"].Value : LevelNames[level],
                    Message = match.Groups["msg"].Value
                };
                _all.Add(entry);
                if (Matches(entry)) Entries.Add(entry);
            }
        }

        /// <summary>Registers the native log callback. Safe to call once.</summary>
        public void Attach()
        {
            SeedFromDisk();
            try
            {
                CastCoreBridge.castmirror_set_log_callback(_logCallback, IntPtr.Zero);
            }
            catch (Exception ex)
            {
                MainViewModel.LogError(ex);
            }
        }

        public void Detach()
        {
            _disposed = true;
            try
            {
                CastCoreBridge.castmirror_set_log_callback(null!, IntPtr.Zero);
            }
            catch (Exception ex)
            {
                MainViewModel.LogError(ex);
            }
        }

        private static readonly string[] LevelNames = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

        private void OnLog(int level, IntPtr message, IntPtr userData)
        {
            var entry = new LogEntry
            {
                Timestamp = DateTime.Now,
                Level = level,
                LevelName = level >= 0 && level < LevelNames.Length ? LevelNames[level] : "LOG",
                // UTF-8: device names and window titles in engine logs are UTF-8,
                // so decoding them as ANSI would mangle every non-ASCII name.
                Message = Marshal.PtrToStringUTF8(message) ?? string.Empty
            };

            void Add()
            {
                if (_disposed) return;
                _all.Add(entry);
                if (_all.Count > MaxEntries) _all.RemoveRange(0, _all.Count - MaxEntries);
                if (Matches(entry)) Entries.Add(entry);
                while (Entries.Count > MaxEntries) Entries.RemoveAt(0);
            }

            if (_dispatcher == null || _dispatcher.HasThreadAccess)
            {
                Add();
            }
            else
            {
                _dispatcher.TryEnqueue(Add);
            }
        }

        private int _minLevel = 1; // Info and above by default.
        public int MinLevel
        {
            get => _minLevel;
            set
            {
                if (_minLevel == value) return;
                _minLevel = value;
                OnPropertyChanged();
                Rebuild();
            }
        }

        private string _filter = string.Empty;
        public string Filter
        {
            get => _filter;
            set
            {
                string next = value ?? string.Empty;
                if (_filter == next) return;
                _filter = next;
                OnPropertyChanged();
                Rebuild();
            }
        }

        private bool Matches(LogEntry entry)
        {
            if (entry.Level < _minLevel) return false;
            if (_filter.Length == 0) return true;
            return entry.Message.Contains(_filter, StringComparison.OrdinalIgnoreCase);
        }

        private void Rebuild()
        {
            Entries.Clear();
            foreach (var entry in _all)
            {
                if (Matches(entry)) Entries.Add(entry);
            }
        }

        public void Clear()
        {
            _all.Clear();
            Entries.Clear();
        }

        /// <summary>The selected lines when there is a selection, otherwise every visible line.</summary>
        public string CopyText(IList<object>? selected = null)
        {
            var builder = new StringBuilder();
            if (selected != null && selected.Count > 0)
            {
                // Selection order is the order the user clicked; the log reads top to bottom.
                var chosen = new List<LogEntry>();
                foreach (object item in selected) if (item is LogEntry entry) chosen.Add(entry);
                chosen.Sort((a, b) => Entries.IndexOf(a).CompareTo(Entries.IndexOf(b)));
                foreach (var entry in chosen) builder.AppendLine(entry.Display);
            }
            else
            {
                foreach (var entry in Entries) builder.AppendLine(entry.Display);
            }
            return builder.ToString();
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        private void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}
