using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Microsoft.UI.Dispatching;
using CastMirror.Services;

namespace CastMirror.ViewModels
{
    public class DeviceItem : INotifyPropertyChanged
    {
        private string _name = string.Empty;
        private string _modelName = string.Empty;
        private string _statusText = "Ready";

        public string Id { get; set; } = string.Empty;

        private string _ipAddress = string.Empty;
        public string IpAddress
        {
            get => _ipAddress;
            set
            {
                if (_ipAddress == value) return;
                _ipAddress = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(AddressText));
            }
        }

        private ushort _port;
        public ushort Port
        {
            get => _port;
            set
            {
                if (_port == value) return;
                _port = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(AddressText));
            }
        }

        /// <summary>ip:port when the engine reported a port, otherwise the bare address.</summary>
        public string AddressText =>
            string.IsNullOrEmpty(IpAddress)
                ? string.Empty
                : (Port > 0 ? $"{IpAddress}:{Port}" : IpAddress);

        public string Name
        {
            get => _name;
            set { if (_name == value) return; _name = value; OnPropertyChanged(); }
        }

        public string ModelName
        {
            get => _modelName;
            set { if (_modelName == value) return; _modelName = value; OnPropertyChanged(); }
        }

        public string StatusText
        {
            get => _statusText;
            set { if (_statusText == value) return; _statusText = value; OnPropertyChanged(); }
        }

        public event PropertyChangedEventHandler? PropertyChanged;

        private void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }

    public class SourceItem
    {
        public int Kind { get; set; }
        public int Id { get; set; }
        public string Name { get; set; } = string.Empty;
        public string Detail { get; set; } = string.Empty;
        public override string ToString() => Name;
    }

    public class MainViewModel : INotifyPropertyChanged
    {
        public ObservableCollection<DeviceItem> Devices { get; } = new();
        public ObservableCollection<SourceItem> Sources { get; } = new();

        /// <summary>
        /// Engine configuration, shared with the settings window. Mutate through
        /// the Apply* methods so changes are persisted and applied live.
        /// </summary>
        public CastMirrorSettings Settings { get; private set; } = new();

        // Nominal per-preset bitrates live in CastMirrorSettings.EffectiveBitrateCapKbps
        // (0 stored = preset default); the UI pushes the stored cap, not a table.
        // Game and Cinema define their own buffer depth; every other preset uses
        // the delay the user chose in Settings instead of overwriting it.
        private int PresetDelayMs(int presetIndex) => presetIndex switch
        {
            (int)CastMirrorQualityPreset.Game => 150,
            (int)CastMirrorQualityPreset.Cinema => 400,
            _ => Settings.TargetDelayMs
        };

        private readonly DispatcherQueue? _dispatcher;
        private readonly DispatcherQueueTimer? _statsTimer;
        private readonly DispatcherQueueTimer? _discoveryTimer;
        private readonly bool _nativeAvailable;
        private volatile bool _disposed;

        // False when the engine config read failed: every Apply*/Persist path
        // is gated on this so a default-valued DTO can never overwrite the
        // user's saved configuration (see SettingsService.Load remarks).
        private bool _settingsLoaded;

        // The TV and source used last time, applied once when they show up.
        private string _pendingLastDeviceId = string.Empty;
        private string _pendingLastSourceKind = string.Empty;
        private int _pendingLastSourceId;

        // Devices added by address (not from mDNS); kept across refreshes.
        private readonly List<DeviceItem> _manualDevices = new();

        private DeviceItem? _selectedDevice;

        public DeviceItem? SelectedDevice
        {
            get => _selectedDevice;
            set
            {
                if (ReferenceEquals(_selectedDevice, value)) return;
                _selectedDevice = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(CanToggleCast));
            }
        }

        private SourceItem? _selectedSource;
        public SourceItem? SelectedSource
        {
            get => _selectedSource;
            set
            {
                if (ReferenceEquals(_selectedSource, value)) return;
                _selectedSource = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(CanToggleCast));
            }
        }

        private bool _isStreaming;
        public bool IsStreaming
        {
            get => _isStreaming;
            private set
            {
                if (_isStreaming == value) return;
                _isStreaming = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsLiveVisible));
                OnPropertyChanged(nameof(PreviewTitle));
            }
        }

        private bool _isSessionActive;
        public bool IsSessionActive
        {
            get => _isSessionActive;
            private set
            {
                if (_isSessionActive == value) return;
                _isSessionActive = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(ShowCastButton));
                OnPropertyChanged(nameof(ShowStopButton));
                OnPropertyChanged(nameof(IsSearchingVisible));
                OnPropertyChanged(nameof(CanToggleCast));
                OnPropertyChanged(nameof(PreviewTitle));
            }
        }

        private bool _castPending;
        public bool CastPending
        {
            get => _castPending;
            private set
            {
                if (_castPending == value) return;
                _castPending = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(CanToggleCast));
            }
        }

        private bool _noDevicesFound;
        public bool NoDevicesFound
        {
            get => _noDevicesFound;
            private set
            {
                if (_noDevicesFound == value) return;
                _noDevicesFound = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsSearchingVisible));
                OnPropertyChanged(nameof(PreviewTitle));
            }
        }

        public bool HasDevices => Devices.Count > 0;
        public bool IsLiveVisible => IsStreaming;
        public bool IsSearchingVisible => Devices.Count == 0 && !IsSessionActive && !NoDevicesFound;
        public bool CanToggleCast => !CastPending && (IsSessionActive || (SelectedDevice != null && SelectedSource != null));

        /// <summary>The Cast button and the Stop button swap places; each has its own template-safe style.</summary>
        public bool ShowCastButton => !IsSessionActive;
        public bool ShowStopButton => IsSessionActive;

        public string PreviewTitle
        {
            get
            {
                if (IsSessionActive) return IsStreaming ? "Casting" : "Connecting...";
                if (NoDevicesFound) return "No Cast devices found";
                if (Devices.Count == 0) return "Looking for Cast devices...";
                return "Ready to cast";
            }
        }

        private int _presetIndex;
        public int PresetIndex
        {
            get => _presetIndex;
            set
            {
                if (_presetIndex == value) return;
                _presetIndex = value;
                OnPropertyChanged();
                // Persist the choice: quality_preset keys the per-preset
                // bitrate caps on both sides, so leaving it unsaved desynced
                // the engine's view of which cap applies.
                Settings.QualityPreset = CastMirrorSettings.PresetName(value);
                PersistSettings();
                // A preset switch changes the effective cap, so the inline
                // bitrate slider (and the settings window) must follow it.
                OnPropertyChanged(nameof(BitrateCapKbps));
                OnPropertyChanged(nameof(BitrateCapText));
                ApplyPresetLive(value);
            }
        }

        private bool _audioEnabled = true;
        public bool AudioEnabled
        {
            get => _audioEnabled;
            set
            {
                if (_audioEnabled == value) return;
                _audioEnabled = value;
                OnPropertyChanged();
                if (IsSessionActive)
                {
                    // Mid-cast the switch is the same control as "Mute TV audio":
                    // the engine has one mute flag, so the two must not fight.
                    MuteTvAudio = !value;
                }
                else
                {
                    Settings.AudioEnabled = value;
                    PersistSettings();
                }
            }
        }

        private string _statusMessage = "Scanning your Wi-Fi network for Cast devices.";
        public string StatusMessage
        {
            get => _statusMessage;
            private set
            {
                if (_statusMessage == value) return;
                _statusMessage = value;
                OnPropertyChanged();
            }
        }

        private string _errorMessage = string.Empty;
        public string ErrorMessage
        {
            get => _errorMessage;
            set
            {
                string newValue = value ?? string.Empty;
                if (_errorMessage == newValue) return;
                _errorMessage = newValue;
                OnPropertyChanged();
                OnPropertyChanged(nameof(HasError));
            }
        }

        public bool HasError => !string.IsNullOrEmpty(ErrorMessage);

        private string _statsFpsText = "FPS: --";
        public string StatsFpsText
        {
            get => _statsFpsText;
            private set { if (_statsFpsText == value) return; _statsFpsText = value; OnPropertyChanged(); }
        }

        private string _statsBitrateText = "Bitrate: --";
        public string StatsBitrateText
        {
            get => _statsBitrateText;
            private set { if (_statsBitrateText == value) return; _statsBitrateText = value; OnPropertyChanged(); }
        }

        private string _statsLatencyText = "RTT: --";
        public string StatsLatencyText
        {
            get => _statsLatencyText;
            private set { if (_statsLatencyText == value) return; _statsLatencyText = value; OnPropertyChanged(); }
        }

        private string _statsLossText = "Loss: --";
        public string StatsLossText
        {
            get => _statsLossText;
            private set { if (_statsLossText == value) return; _statsLossText = value; OnPropertyChanged(); }
        }

        private string _statsQualityText = string.Empty;
        public string StatsQualityText
        {
            get => _statsQualityText;
            private set { if (_statsQualityText == value) return; _statsQualityText = value; OnPropertyChanged(); }
        }

        // Sparkline history: one sample per stats tick, capped so a long session
        // cannot grow without bound. The Sparkline control owns the rendering.
        private const int SparkCapacity = Controls.Sparkline.DefaultCapacity;
        private readonly List<double> _fpsHistory = new();
        private readonly List<double> _bitrateHistory = new();
        private readonly List<double> _rttHistory = new();
        private readonly List<double> _lossHistory = new();

        private double[] _fpsSeries = Array.Empty<double>();
        public double[] FpsSeries
        {
            get => _fpsSeries;
            private set { _fpsSeries = value; OnPropertyChanged(); }
        }

        private double[] _bitrateSeries = Array.Empty<double>();
        public double[] BitrateSeries
        {
            get => _bitrateSeries;
            private set { _bitrateSeries = value; OnPropertyChanged(); }
        }

        private double[] _rttSeries = Array.Empty<double>();
        public double[] RttSeries
        {
            get => _rttSeries;
            private set { _rttSeries = value; OnPropertyChanged(); }
        }

        private double[] _lossSeries = Array.Empty<double>();
        public double[] LossSeries
        {
            get => _lossSeries;
            private set { _lossSeries = value; OnPropertyChanged(); }
        }

        private static void PushHistory(List<double> history, double value)
        {
            history.Add(value);
            if (history.Count > SparkCapacity) history.RemoveAt(0);
        }

        private bool _freezeStream;
        public bool FreezeStream
        {
            get => _freezeStream;
            set
            {
                if (_freezeStream == value) return;
                _freezeStream = value;
                OnPropertyChanged();
                if (_nativeAvailable)
                {
                    try
                    {
                        CastCoreBridge.castmirror_set_freeze(value);
                    }
                    catch (Exception ex)
                    {
                        LogError(ex);
                    }
                }
            }
        }

        private bool _muteTvAudio;
        public bool MuteTvAudio
        {
            get => _muteTvAudio;
            set
            {
                if (_muteTvAudio == value) return;
                _muteTvAudio = value;
                OnPropertyChanged();
                if (_nativeAvailable)
                {
                    TryNative(() => CastCoreBridge.castmirror_set_muted(value));
                }
                if (IsSessionActive && _audioEnabled == value)
                {
                    _audioEnabled = !value;
                    OnPropertyChanged(nameof(AudioEnabled));
                }
            }
        }

        private readonly StateCallback _stateCallback;
        private readonly DevicesCallback _devicesCallback;
        private readonly StatsCallback _statsCallback;

        public MainViewModel()
        {
            _dispatcher = DispatcherQueue.GetForCurrentThread();

            _stateCallback = OnStateChanged;
            _devicesCallback = OnDevicesChanged;
            _statsCallback = OnStatsUpdated;

            try
            {
                // Fails fast (and loudly) when CastMirror.exe and castcore.dll
                // come from different builds: the struct mirrors would otherwise
                // marshal into the wrong offsets with no error at the boundary.
                CastCoreBridge.VerifyNativeAbi();

                if (!CastCoreBridge.castmirror_init())
                {
                    ErrorMessage = "Failed to initialize the CastCore engine.";
                    return;
                }

                _nativeAvailable = true;
                CastCoreBridge.castmirror_set_state_callback(_stateCallback, IntPtr.Zero);
                CastCoreBridge.castmirror_set_devices_callback(_devicesCallback, IntPtr.Zero);
                CastCoreBridge.castmirror_set_stats_callback(_statsCallback, IntPtr.Zero);

                LoadSettings();

                if (_dispatcher != null)
                {
                    _statsTimer = _dispatcher.CreateTimer();
                    _statsTimer.Interval = TimeSpan.FromSeconds(1);
                    _statsTimer.Tick += (_, _) => PollStats();

                    _discoveryTimer = _dispatcher.CreateTimer();
                    _discoveryTimer.Interval = TimeSpan.FromSeconds(12);
                    _discoveryTimer.IsRepeating = false;
                    _discoveryTimer.Tick += (_, _) => OnDiscoveryTimedOut();
                }

                RefreshSources();
                CastCoreBridge.castmirror_start_discovery();
                _discoveryTimer?.Start();
            }
            catch (DllNotFoundException)
            {
                ErrorMessage = "castcore.dll was not found next to CastMirror.exe.";
            }
            catch (BadImageFormatException)
            {
                ErrorMessage = "castcore.dll architecture does not match this app (expected x64).";
            }
            catch (EntryPointNotFoundException ex)
            {
                ErrorMessage = $"castcore.dll is missing a required export: {ex.Message}";
            }
            catch (Exception ex)
            {
                ErrorMessage = $"Startup failed: {ex.Message}";
                LogError(ex);
            }
        }

        public async void ToggleCast()
        {
            if (!_nativeAvailable || CastPending) return;

            if (IsSessionActive)
            {
                await StopCastAsync();
            }
            else
            {
                await StartCastAsync();
            }
        }

        private async Task StartCastAsync()
        {
            DeviceItem? device = SelectedDevice;
            SourceItem? source = SelectedSource;
            if (device == null)
            {
                ErrorMessage = "Select a Cast device first.";
                return;
            }
            if (source == null)
            {
                ErrorMessage = "Select a display or window to mirror.";
                return;
            }

            CastPending = true;
            ErrorMessage = string.Empty;
            StatusMessage = $"Connecting to {device.Name}...";

            bool ok = false;
            string error = string.Empty;
            int preset = PresetIndex;
            bool audio = AudioEnabled;
            try
            {
                var result = await Task.Run(() =>
                {
                    // 0 fps / 0 kbps: the engine applies capture_fps and the
                    // selected preset's bitrate from the saved configuration.
                    bool started = CastCoreBridge.castmirror_start_cast_ex(
                        NativeText.EncodeZ(device.Id), source.Kind, source.Id, 0, 0, preset, audio);
                    string message = started ? string.Empty : (ReadLastError() ?? "Failed to start casting.");
                    return (started, message);
                });
                ok = result.started;
                error = result.message;
            }
            catch (Exception ex)
            {
                error = ex.Message;
                LogError(ex);
            }
            finally
            {
                CastPending = false;
            }

            if (!ok)
            {
                ErrorMessage = error;
                StatusMessage = "Could not start casting.";
            }
        }

        private async Task StopCastAsync()
        {
            CastPending = true;
            StatusMessage = "Stopping...";
            try
            {
                await Task.Run(() => CastCoreBridge.castmirror_stop_cast());
            }
            catch (Exception ex)
            {
                ErrorMessage = ex.Message;
                LogError(ex);
            }
            finally
            {
                CastPending = false;
            }
        }

        public void RefreshSources()
        {
            if (!_nativeAvailable) return;

            if (IsSessionActive)
            {
                StatusMessage = "Sources can't be refreshed while casting.";
                return;
            }

            try
            {
                var sources = new List<SourceItem>();

                int displayCount = CastCoreBridge.castmirror_get_display_count();
                for (int i = 0; i < displayCount; ++i)
                {
                    if (!CastCoreBridge.castmirror_get_display_info(i, out var display)) continue;
                    string name = string.IsNullOrWhiteSpace(display.Name) ? $"Display {display.Id}" : display.Name;
                    if (display.IsPrimary) name += " (Primary)";
                    sources.Add(new SourceItem
                    {
                        Kind = (int)CastMirrorSourceKind.Monitor,
                        Id = display.Id,
                        Name = name,
                        Detail = $"{display.Width}x{display.Height} @ {display.RefreshRate}Hz"
                    });
                }

                if (CastCoreBridge.castmirror_window_capture_supported())
                {
                    int windowCount = CastCoreBridge.castmirror_get_window_count();
                    for (int i = 0; i < windowCount; ++i)
                    {
                        if (!CastCoreBridge.castmirror_get_window_info(i, out var window)) continue;
                        sources.Add(new SourceItem
                        {
                            Kind = (int)CastMirrorSourceKind.Window,
                            Id = window.Id,
                            Name = string.IsNullOrWhiteSpace(window.Title) ? $"Window {window.Id}" : window.Title,
                            Detail = string.IsNullOrWhiteSpace(window.AppClass) ? "Window" : window.AppClass
                        });
                    }
                }

                int previousKind = SelectedSource?.Kind ?? -1;
                int previousId = SelectedSource?.Id ?? -1;
                if (previousKind < 0 && _pendingLastSourceKind.Length > 0)
                {
                    previousKind = string.Equals(_pendingLastSourceKind, "window", StringComparison.OrdinalIgnoreCase)
                        ? (int)CastMirrorSourceKind.Window
                        : (int)CastMirrorSourceKind.Monitor;
                    previousId = _pendingLastSourceId;
                }
                _pendingLastSourceKind = string.Empty;
                Sources.Clear();
                foreach (var source in sources) Sources.Add(source);
                SelectedSource = Sources.FirstOrDefault(s => s.Kind == previousKind && s.Id == previousId)
                                 ?? Sources.FirstOrDefault();
            }
            catch (Exception ex)
            {
                ErrorMessage = $"Source enumeration failed: {ex.Message}";
                LogError(ex);
            }
        }

        public void DismissError()
        {
            ErrorMessage = string.Empty;
        }

        public void Shutdown()
        {
            _bitrateCommitTimer?.Stop();
            CommitBitrate();
            _disposed = true;
            try
            {
                _statsTimer?.Stop();
                _discoveryTimer?.Stop();
                if (_nativeAvailable)
                {
                    CastCoreBridge.castmirror_stop_cast();
                    CastCoreBridge.castmirror_stop_discovery();
                    CastCoreBridge.castmirror_shutdown();
                }
            }
            catch (Exception ex)
            {
                LogError(ex);
            }
        }

        private void OnStateChanged(CastMirrorState state, IntPtr message, IntPtr userData)
        {
            // Native strings are UTF-8 (see NativeText), so decode the borrowed
            // pointer explicitly instead of letting the marshaller treat the
            // delegate parameter as an ANSI string.
            string text = Marshal.PtrToStringUTF8(message) ?? string.Empty;
            RunOnUiThread(() => ApplyState(state, text));
        }

        private CastMirrorState _lastState = CastMirrorState.Idle;

        // Freeze and mute belong to one session. A new one must not start with
        // the switches showing On while the engine has them off.
        private void ResetLiveToggles()
        {
            if (_freezeStream)
            {
                _freezeStream = false;
                OnPropertyChanged(nameof(FreezeStream));
            }
            if (_muteTvAudio)
            {
                _muteTvAudio = false;
                OnPropertyChanged(nameof(MuteTvAudio));
            }
            if (_audioEnabled != Settings.AudioEnabled)
            {
                _audioEnabled = Settings.AudioEnabled;
                OnPropertyChanged(nameof(AudioEnabled));
            }
        }

        private void ApplyState(CastMirrorState state, string message)
        {
            bool wasStreaming = IsStreaming;
            bool wasActive = IsSessionActive;
            CastMirrorState previous = _lastState;
            _lastState = state;

            IsStreaming = state == CastMirrorState.Streaming;
            IsSessionActive = state is CastMirrorState.Connecting
                                   or CastMirrorState.Negotiating
                                   or CastMirrorState.Streaming
                                   or CastMirrorState.Reconnecting
                                   or CastMirrorState.Stopping;

            if (!IsSessionActive)
            {
                ResetLiveToggles();
            }

            if (state == CastMirrorState.Reconnecting && previous != CastMirrorState.Reconnecting)
            {
                NotificationService.Notify(Settings.NotifyOnEvents, "Connection lost",
                    $"Reconnecting to {SelectedDevice?.Name ?? "the TV"}.");
            }

            if (state == CastMirrorState.Failed)
            {
                ErrorMessage = ReadLastError() ?? "The cast session failed.";
                StatusMessage = "Connection failed. Check the TV and try again.";
                NotificationService.Notify(Settings.NotifyOnEvents, "Casting failed", ErrorMessage, "cast-failed");
            }
            else if (state == CastMirrorState.Streaming)
            {
                if (!string.IsNullOrEmpty(message)) StatusMessage = message;
                else StatusMessage = $"Streaming to {SelectedDevice?.Name ?? "the TV"}.";
                if (!wasStreaming)
                {
                    NotificationService.Notify(Settings.NotifyOnEvents, "Casting started", StatusMessage);
                }
            }
            else if (state == CastMirrorState.Idle)
            {
                ResetStats();
                if (!HasError) StatusMessage = "Ready when you are.";
                if (wasActive)
                {
                    NotificationService.Notify(Settings.NotifyOnEvents, "Casting stopped",
                        "The Cast session has ended.");
                }
            }
            else if (!string.IsNullOrEmpty(message))
            {
                StatusMessage = message;
            }

            if (IsSessionActive)
            {
                _statsTimer?.Start();
            }
            else
            {
                _statsTimer?.Stop();
            }

            StateChanged?.Invoke(state);
        }

        /// <summary>Raised for every session state change (tray tooltip, notifications).</summary>
        public event Action<CastMirrorState>? StateChanged;

        private void OnDevicesChanged(int count, IntPtr userData)
        {
            RunOnUiThread(() => ApplyDevices(count));
        }

        private void ApplyDevices(int count)
        {
            var incoming = new List<DeviceItem>(Math.Max(count, 0));
            for (int i = 0; i < count; ++i)
            {
                if (CastCoreBridge.castmirror_get_device_info(i, out var device))
                {
                    incoming.Add(new DeviceItem
                    {
                        Id = device.Id,
                        Name = device.Name,
                        ModelName = device.ModelName,
                        IpAddress = device.IpAddress,
                        Port = device.Port,
                        StatusText = "Ready"
                    });
                }
            }

            // mDNS re-announces devices constantly, sometimes in a different
            // order. Rebuild the collection incrementally so the ListView (and
            // the user's selection) is never reset: remove disappeared devices,
            // update existing entries in place, append new ones.
            //
            // Addresses the user typed in are not mDNS results, so carry them
            // over; drop the manual entry once the engine reports that address.
            // Match on the address as well as the id: a manual entry's id is the
            // typed IP, while mDNS reports the device's UUID for the same TV.
            string? selectedAddress = SelectedDevice?.IpAddress;
            _manualDevices.RemoveAll(manual =>
                incoming.Any(d => d.Id == manual.Id || d.IpAddress == manual.IpAddress));
            incoming.AddRange(_manualDevices);

            bool changed = false;
            for (int i = Devices.Count - 1; i >= 0; --i)
            {
                if (!incoming.Any(d => d.Id == Devices[i].Id))
                {
                    Devices.RemoveAt(i);
                    changed = true;
                }
            }
            foreach (var device in incoming)
            {
                DeviceItem? existing = Devices.FirstOrDefault(d => d.Id == device.Id);
                if (existing == null)
                {
                    Devices.Add(device);
                    changed = true;
                }
                else
                {
                    existing.Name = device.Name;
                    existing.ModelName = device.ModelName;
                    existing.IpAddress = device.IpAddress;
                    existing.Port = device.Port;
                    existing.StatusText = device.StatusText;
                }
            }

            // Only touch the selection when the user's device disappeared.
            if (SelectedDevice != null && !Devices.Contains(SelectedDevice))
            {
                SelectedDevice = Devices.FirstOrDefault(d => d.IpAddress == selectedAddress)
                                 ?? Devices.FirstOrDefault();
            }
            else if (SelectedDevice == null && Devices.Count > 0)
            {
                SelectedDevice = Devices[0];
            }

            if (_pendingLastDeviceId.Length > 0)
            {
                DeviceItem? last = Devices.FirstOrDefault(d => d.Id == _pendingLastDeviceId);
                if (last != null)
                {
                    SelectedDevice = last;
                    _pendingLastDeviceId = string.Empty;
                }
            }

            if (changed)
            {
                NoDevicesFound = false;
                OnPropertyChanged(nameof(HasDevices));
                OnPropertyChanged(nameof(IsSearchingVisible));
                OnPropertyChanged(nameof(PreviewTitle));
            }
        }

        private void OnDiscoveryTimedOut()
        {
            if (Devices.Count == 0 && !IsSessionActive)
            {
                NoDevicesFound = true;
                StatusMessage = "Make sure your TV or speaker is powered on and on the same Wi-Fi network.";
            }
        }

        private void OnStatsUpdated([In] ref CastMirrorStreamStats stats, IntPtr userData)
        {
            // Copy the struct INSIDE the callback: the native pointer is only
            // valid for the duration of this call, and the marshalled strings are
            // freed on return. The closure below escapes to the UI thread, so it
            // must capture this snapshot, never `stats` itself.
            CastMirrorStreamStats snapshot = stats;
            RunOnUiThread(() => ApplyStats(snapshot));
        }

        private void PollStats()
        {
            if (!_nativeAvailable || _disposed) return;
            try
            {
                if (CastCoreBridge.castmirror_get_stats(out var stats))
                {
                    ApplyStats(stats);
                }
            }
            catch (Exception ex)
            {
                LogError(ex);
            }
        }

        private void ApplyStats(CastMirrorStreamStats stats)
        {
            StatsFpsText = $"FPS: {stats.CurrentFps:F1}";
            StatsBitrateText = $"Bitrate: {stats.BitrateKbps / 1000.0:F1} Mbps";
            StatsLatencyText = $"RTT: {stats.RoundTripTimeMs:F0} ms";
            StatsLossText = $"Loss: {stats.PacketLossFraction * 100.0:F1}%";

            string encoder = string.IsNullOrWhiteSpace(stats.EncoderName) ? "encoder --" : stats.EncoderName;
            int framerate = stats.CurrentFramerate > 0 ? stats.CurrentFramerate : (int)Math.Round(stats.CurrentFps);
            StatsQualityText = stats.Width > 0
                ? $"{stats.Width}x{stats.Height}@{framerate} - {encoder}"
                : encoder;

            PushHistory(_fpsHistory, stats.CurrentFps);
            PushHistory(_bitrateHistory, stats.BitrateKbps / 1000.0);
            PushHistory(_rttHistory, stats.RoundTripTimeMs);
            PushHistory(_lossHistory, stats.PacketLossFraction * 100.0);
            FpsSeries = _fpsHistory.ToArray();
            BitrateSeries = _bitrateHistory.ToArray();
            RttSeries = _rttHistory.ToArray();
            LossSeries = _lossHistory.ToArray();
        }

        private void ResetStats()
        {
            StatsFpsText = "FPS: --";
            StatsBitrateText = "Bitrate: --";
            StatsLatencyText = "RTT: --";
            StatsLossText = "Loss: --";
            StatsQualityText = string.Empty;

            _fpsHistory.Clear();
            _bitrateHistory.Clear();
            _rttHistory.Clear();
            _lossHistory.Clear();
            FpsSeries = Array.Empty<double>();
            BitrateSeries = Array.Empty<double>();
            RttSeries = Array.Empty<double>();
            LossSeries = Array.Empty<double>();
        }

        private void ApplyPresetLive(int presetIndex)
        {
            if (!_nativeAvailable || !IsSessionActive) return;
            if (presetIndex < 0 || presetIndex > (int)CastMirrorQualityPreset.Cinema) return;
            try
            {
                // The user's stored per-preset cap (0 = nominal default) wins
                // over the hardcoded table the engine only used at start.
                CastCoreBridge.castmirror_set_bitrate(Settings.EffectiveBitrateCapKbps());
                CastCoreBridge.castmirror_set_playout_delay(PresetDelayMs(presetIndex));
                // Game locks the resolution live (low latency beats detail);
                // every other preset restores the user's steady-frame-rate choice.
                bool allowResolutionChange = presetIndex != (int)CastMirrorQualityPreset.Game && !SteadyFrameRate;
                CastCoreBridge.castmirror_set_adaptive_resolution_allowed(allowResolutionChange);
            }
            catch (Exception ex)
            {
                LogError(ex);
            }
        }

        /// <summary>Steady frame rate: locks resolution and frame rate while streaming.</summary>
        public bool SteadyFrameRate
        {
            get => !Settings.AdaptiveResolutionEnabled;
            set
            {
                if (Settings.AdaptiveResolutionEnabled == !value) return;
                Settings.AdaptiveResolutionEnabled = !value;
                OnPropertyChanged();
                PersistSettings();
                if (_nativeAvailable && IsSessionActive)
                {
                    // The engine's flag means "allow resolution changes", so a
                    // steady (locked) stream turns it off.
                    TryNative(() => CastCoreBridge.castmirror_set_adaptive_resolution_allowed(!value));
                }
            }
        }

        public void ApplyCaptureFps(int fps)
        {
            Settings.CaptureFps = fps;
            PersistSettings();
        }

        public void ApplySteadyFrameRate(bool steady)
        {
            SteadyFrameRate = steady;
        }

        public void ApplyForceSoftwareEncode(bool force)
        {
            Settings.ForceSoftwareEncode = force;
            PersistSettings();
        }

        // A slider drag fires a value change per step. The label follows at once;
        // the file write and the encoder retune wait until the drag settles.
        private DispatcherQueueTimer? _bitrateCommitTimer;
        private uint _pendingBitrateKbps;
        private bool _bitrateCommitPending;

        public void ApplyBitrateCap(uint kbps)
        {
            Settings.SetBitrateCapKbps(kbps);
            OnPropertyChanged(nameof(BitrateCapKbps));
            OnPropertyChanged(nameof(BitrateCapText));
            _pendingBitrateKbps = kbps;
            _bitrateCommitPending = true;

            if (_dispatcher == null)
            {
                CommitBitrate();
                return;
            }
            if (_bitrateCommitTimer == null)
            {
                _bitrateCommitTimer = _dispatcher.CreateTimer();
                _bitrateCommitTimer.Interval = TimeSpan.FromMilliseconds(250);
                _bitrateCommitTimer.IsRepeating = false;
                _bitrateCommitTimer.Tick += (_, _) => CommitBitrate();
            }
            _bitrateCommitTimer.Stop();
            _bitrateCommitTimer.Start();
        }

        private void CommitBitrate()
        {
            if (!_bitrateCommitPending) return;
            _bitrateCommitPending = false;
            PersistSettings();
            if (_nativeAvailable && IsSessionActive)
            {
                uint kbps = _pendingBitrateKbps;
                TryNative(() => CastCoreBridge.castmirror_set_bitrate(kbps));
            }
        }

        /// <summary>
        /// Inline bitrate slider on the Cast surface. Two-way bound to the same
        /// stored cap the Settings slider edits, so the two stay synchronized.
        /// </summary>
        public double BitrateCapKbps
        {
            get => Settings.EffectiveBitrateCapKbps();
            set
            {
                uint kbps = (uint)Math.Max(0, Math.Round(value));
                if (Settings.EffectiveBitrateCapKbps() == kbps) return;
                ApplyBitrateCap(kbps);
            }
        }

        public string BitrateCapText => $"{Settings.EffectiveBitrateCapKbps() / 1000.0:F1} Mbps";

        public void ApplyAudioEnabled(bool enabled)
        {
            Settings.AudioEnabled = enabled;
            AudioEnabled = enabled;
            PersistSettings();
        }

        public void ApplyAudioBitrate(uint bps)
        {
            Settings.AudioBitrateBps = bps;
            PersistSettings();
            if (_nativeAvailable && IsSessionActive)
            {
                TryNative(() => CastCoreBridge.castmirror_set_audio_bitrate(bps));
            }
        }

        public void ApplyTargetDelay(int delayMs)
        {
            Settings.TargetDelayMs = delayMs;
            PersistSettings();
            if (_nativeAvailable && IsSessionActive)
            {
                TryNative(() => CastCoreBridge.castmirror_set_playout_delay(delayMs));
            }
        }

        public void ApplyReconnectWindow(int seconds)
        {
            Settings.ReconnectWindowSeconds = seconds;
            PersistSettings();
        }

        public void ApplyShowCursor(bool enabled)
        {
            Settings.ShowCursor = enabled;
            PersistSettings();
        }

        /// <summary>Remembers the window size (in DIPs) for the next launch.</summary>
        public void SaveWindowSize(int widthDip, int heightDip)
        {
            if (widthDip < 200 || heightDip < 200) return;
            Settings.WindowWidth = widthDip;
            Settings.WindowHeight = heightDip;
            PersistSettings();
        }

        /// <summary>Shown when an unhandled fault was swallowed to keep the session alive.</summary>
        public void ReportUnexpectedError()
        {
            ErrorMessage = "Something went wrong inside CastMirror. Your cast keeps running. " +
                           "If it happens again, open Logs and copy the log into a bug report.";
        }

        /// <summary>Tells the user the last run ended in a crash and where the crash file is.</summary>
        public void ReportPreviousCrash(string dumpPath)
        {
            ErrorMessage = "CastMirror closed unexpectedly last time. A crash file was saved in " +
                           $"{System.IO.Path.GetDirectoryName(dumpPath)}. It contains a copy of the program's " +
                           "memory, so attach it to a bug report only if you're comfortable sharing that.";
        }

        /// <summary>Removes a device the user added by address.</summary>
        public void RemoveManualDevice(DeviceItem device)
        {
            if (!_manualDevices.Remove(device)) return;
            Devices.Remove(device);
            if (ReferenceEquals(SelectedDevice, device))
            {
                SelectedDevice = Devices.FirstOrDefault();
            }
            OnPropertyChanged(nameof(HasDevices));
            OnPropertyChanged(nameof(IsSearchingVisible));
            OnPropertyChanged(nameof(PreviewTitle));
        }

        public bool IsManualDevice(DeviceItem device) => _manualDevices.Contains(device);

        public void ApplyLatencyHud(bool enabled)
        {
            Settings.LatencyHudEnabled = enabled;
            PersistSettings();
        }

        public void ApplySubnetScan(bool enabled)
        {
            Settings.SubnetScanEnabled = enabled;
            PersistSettings();
            if (enabled)
            {
                RescanDevices();
            }
        }

        private bool _showFirstRun;
        /// <summary>
        /// True on the first launch (or until the user dismisses the card): the
        /// window explains what discovery needs and offers the subnet scan.
        /// </summary>
        public bool ShowFirstRun
        {
            get => _showFirstRun;
            private set
            {
                if (_showFirstRun == value) return;
                _showFirstRun = value;
                OnPropertyChanged();
            }
        }

        /// <summary>Finishes the first-run card; optionally turns on the LAN scan.</summary>
        public void CompleteFirstRun(bool enableSubnetScan)
        {
            ShowFirstRun = false;
            if (enableSubnetScan) ApplySubnetScan(true);
            Settings.FirstRunComplete = true;
            PersistSettings();
        }

        public void ApplyTrayEnabled(bool enabled)
        {
            Settings.EnableTrayOnStartup = enabled;
            PersistSettings();
        }

        public void ApplyCloseToTray(bool enabled)
        {
            Settings.CloseToTray = enabled;
            PersistSettings();
        }

        public void ApplyNotifications(bool enabled)
        {
            Settings.NotifyOnEvents = enabled;
            PersistSettings();
        }

        public void ApplyTheme(string theme)
        {
            Settings.UiTheme = theme ?? string.Empty;
            PersistSettings();
            ThemeService.Apply(Settings.UiTheme);
        }

        /// <summary>Re-runs device discovery (mDNS plus the optional subnet scan).</summary>
        public void RescanDevices()
        {
            if (!_nativeAvailable) return;
            try
            {
                NoDevicesFound = false;
                CastCoreBridge.castmirror_rescan();
                StatusMessage = "Scanning your Wi-Fi network for Cast devices.";
                _discoveryTimer?.Stop();
                _discoveryTimer?.Start();
            }
            catch (Exception ex)
            {
                ErrorMessage = $"Rescan failed: {ex.Message}";
                LogError(ex);
            }
        }

        /// <summary>Adds a device the user typed in by address and selects it.</summary>
        public bool AddDeviceByIp(string ip)
        {
            string address = (ip ?? string.Empty).Trim();
            // The engine resolves a manual id through inet_pton(AF_INET): only
            // a bare IPv4 literal is castable, so reject hostnames, ports and
            // out-of-range text before the entry becomes permanent.
            if (!System.Net.IPAddress.TryParse(address, out var parsed) ||
                parsed.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
            {
                ErrorMessage = $"\"{address}\" is not a valid IPv4 address (e.g. 192.168.1.50).";
                return false;
            }
            address = parsed.ToString();

            DeviceItem? existing = Devices.FirstOrDefault(d => d.Id == address);
            if (existing == null)
            {
                existing = new DeviceItem
                {
                    Id = address,
                    Name = $"Cast Device ({address})",
                    ModelName = "Chromecast",
                    IpAddress = address,
                    StatusText = "Manual"
                };
                Devices.Add(existing);
                _manualDevices.Add(existing);
                NoDevicesFound = false;
                OnPropertyChanged(nameof(HasDevices));
                OnPropertyChanged(nameof(IsSearchingVisible));
                OnPropertyChanged(nameof(PreviewTitle));
            }

            SelectedDevice = existing;
            StatusMessage = $"Added {address}.";
            return true;
        }

        /// <summary>Re-reads the engine configuration (external edits, settings window reload).</summary>
        public void ReloadSettings()
        {
            if (!_nativeAvailable) return;
            try
            {
                Settings = SettingsService.Load();
                _settingsLoaded = true;
            }
            catch (Exception ex)
            {
                // Keep the previous Settings: persisting this default DTO would
                // overwrite the user's engine configuration (see Load remarks).
                _settingsLoaded = false;
                ErrorMessage = $"Could not load settings: {ex.Message}";
                LogError(ex);
                return;
            }
            PresetIndex = PresetIndexOf(Settings.QualityPreset);
            AudioEnabled = Settings.AudioEnabled;
            OnPropertyChanged(nameof(SteadyFrameRate));
            OnPropertyChanged(nameof(BitrateCapKbps));
            OnPropertyChanged(nameof(BitrateCapText));
        }

        private void LoadSettings()
        {
            try
            {
                Settings = SettingsService.Load();
                _settingsLoaded = true;
            }
            catch (Exception ex)
            {
                // Surface the failure but keep a default DTO gated off from
                // persistence: writing it back would wipe the user's config.
                _settingsLoaded = false;
                ErrorMessage = $"Could not load settings: {ex.Message}";
                LogError(ex);
                return;
            }

            // First Windows launch: the platform default is a steady frame rate
            // (adaptive resolution off); the core's own default stays unchanged
            // for the Linux front-end.
            if (!Settings.FirstRunComplete)
            {
                // First Windows launch: the platform default is a steady frame
                // rate (adaptive resolution off). first_run_complete is only set
                // once the user dismisses the first-run card, so the card does
                // not reappear on every launch.
                Settings.AdaptiveResolutionEnabled = false;
                PersistSettings();
                ShowFirstRun = true;
            }

            PresetIndex = PresetIndexOf(Settings.QualityPreset);
            AudioEnabled = Settings.AudioEnabled;
            _pendingLastDeviceId = Settings.LastDeviceId ?? string.Empty;
            _pendingLastSourceKind = Settings.LastSourceKind ?? string.Empty;
            _pendingLastSourceId = Settings.LastSourceId;
        }

        private static int PresetIndexOf(string preset)
        {
            return CastMirrorSettings.TryParsePresetName(preset, out int index)
                ? index
                : (int)CastMirrorQualityPreset.Auto;
        }

        private void PersistSettings()
        {
            // Never persist a fallback DTO: after a failed Load the in-memory
            // settings are defaults, and saving them would wipe the user's
            // engine configuration. Unblocked by the next successful Load.
            if (!_nativeAvailable || !_settingsLoaded) return;
            if (!SettingsService.Save(Settings))
            {
                ErrorMessage = "Could not save settings.";
            }
        }

        private void TryNative(Action action)
        {
            try
            {
                action();
            }
            catch (Exception ex)
            {
                LogError(ex);
            }
        }

        private void RunOnUiThread(Action action)
        {
            if (_disposed) return;
            try
            {
                if (_dispatcher == null || _dispatcher.HasThreadAccess)
                {
                    action();
                    return;
                }
                // Marshal native worker-thread callbacks onto the UI thread.
                _dispatcher.TryEnqueue(() =>
                {
                    if (_disposed) return;
                    try
                    {
                        action();
                    }
                    catch (Exception ex)
                    {
                        LogError(ex);
                    }
                });
            }
            catch (Exception ex)
            {
                LogError(ex);
            }
        }

        private static string? ReadLastError()
        {
            try
            {
                // GetLastError reads castmirror_get_last_error as UTF-8 bytes;
                // the StringBuilder marshalling here was ANSI.
                return CastCoreBridge.GetLastError();
            }
            catch
            {
                return null;
            }
        }

        internal static void LogError(Exception ex)
        {
            // LogService owns the shared %APPDATA%\CastMirror log and is
            // internally best-effort, so diagnostics can never crash the app.
            LogService.Log(ex.ToString());
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        protected void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}
