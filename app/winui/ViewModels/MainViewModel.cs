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
        private bool _isSelected;

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
            set
            {
                if (_name == value) return;
                _name = value;
                OnPropertyChanged();
                // The card monogram is derived from the name.
                OnPropertyChanged(nameof(Monogram));
            }
        }

        public string ModelName
        {
            get => _modelName;
            set { if (_modelName == value) return; _modelName = value; OnPropertyChanged(); }
        }

        /// <summary>True while this row is the one the primary action will cast to.</summary>
        public bool IsSelected
        {
            get => _isSelected;
            set { if (_isSelected == value) return; _isSelected = value; OnPropertyChanged(); }
        }

        public string StatusText
        {
            get => _statusText;
            set { if (_statusText == value) return; _statusText = value; OnPropertyChanged(); }
        }

        /// <summary>Two-letter monogram shown on the device card, e.g. "LR" for "Living Room TV".</summary>
        public string Monogram
        {
            get
            {
                var words = (Name ?? string.Empty)
                    .Split(new[] { ' ', '-', '_' }, StringSplitOptions.RemoveEmptyEntries)
                    .Where(w => char.IsLetterOrDigit(w[0]))
                    .ToList();
                if (words.Count == 0) return "?";
                if (words.Count == 1)
                {
                    string only = words[0];
                    return only.Length == 1
                        ? only.ToUpperInvariant()
                        : (char.ToUpperInvariant(only[0]) + only[1]).ToString();
                }
                return (char.ToUpperInvariant(words[0][0]) + char.ToUpperInvariant(words[1][0])).ToString();
            }
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
        /// <summary>The windows in <see cref="Sources"/>, for the picker shown in window mode.</summary>
        public ObservableCollection<SourceItem> WindowSources { get; } = new();

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
                // The card ring is driven by the row, so the old and new rows
                // both have to hear about it.
                if (_selectedDevice != null) _selectedDevice.IsSelected = false;
                _selectedDevice = value;
                if (value != null) value.IsSelected = true;
                OnPropertyChanged();
                OnPropertyChanged(nameof(CanToggleCast));
                OnPropertyChanged(nameof(TargetDeviceName));
                OnPropertyChanged(nameof(SessionSubtitle));
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
                // The Casting Options radio follows the source, so picking a window
                // in either list flips it to "window" and a display flips it back.
                if (value != null)
                {
                    SelectedCastMode = value.Kind == (int)CastMirrorSourceKind.Window
                        ? CastMode.Window
                        : CastMode.Screen;
                }
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


        /// <summary>
        /// Every place that adds to or removes from <see cref="Devices"/> ends
        /// here, so the shell's derived state (counts, search results, the
        /// empty-state copy) cannot drift from the list it describes.
        /// </summary>
        private void NotifyDeviceListChanged()
        {
            OnPropertyChanged(nameof(HasDevices));
            OnPropertyChanged(nameof(IsSearchingVisible));
            OnPropertyChanged(nameof(PreviewTitle));
            OnPropertyChanged(nameof(DeviceCountText));
            OnPropertyChanged(nameof(VisibleDevices));
            OnPropertyChanged(nameof(HasNoSearchResults));
        }
        // ---- Shell state for the redesigned window -------------------------
        // The window is a shell (title bar, nav rail, search, device grid) around
        // the same engine calls, so these properties only describe what the user
        // is looking at; nothing here reaches the native engine.

        /// <summary>Which top-level destination the nav rail has selected.</summary>
        public enum ShellSection
        {
            Home,
            Devices,
            Cast,
            Settings,
            Help
        }

        private ShellSection _section = ShellSection.Home;
        public ShellSection Section
        {
            get => _section;
            set
            {
                if (_section == value) return;
                _section = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsHomeSection));
                OnPropertyChanged(nameof(IsDevicesSection));
                OnPropertyChanged(nameof(IsCastSection));
                OnPropertyChanged(nameof(IsSettingsSection));
                OnPropertyChanged(nameof(IsHelpSection));
            }
        }

        public bool IsHomeSection => Section == ShellSection.Home;
        public bool IsDevicesSection => Section == ShellSection.Devices;
        public bool IsCastSection => Section == ShellSection.Cast;
        public bool IsSettingsSection => Section == ShellSection.Settings;
        public bool IsHelpSection => Section == ShellSection.Help;

        private string _searchText = string.Empty;
        /// <summary>
        /// The shell's search box. It filters the device list only: device names
        /// are the only content the window holds that a user would search for.
        /// </summary>
        public string SearchText
        {
            get => _searchText;
            set
            {
                string next = value ?? string.Empty;
                if (_searchText == next) return;
                _searchText = next;
                OnPropertyChanged();
                OnPropertyChanged(nameof(VisibleDevices));
                OnPropertyChanged(nameof(HasNoSearchResults));
            }
        }

        /// <summary>Devices matching <see cref="SearchText"/>, or all of them when the box is empty.</summary>
        public IEnumerable<DeviceItem> VisibleDevices
        {
            get
            {
                if (_searchText.Trim().Length == 0) return Devices;
                return Devices.Where(MatchesSearch);
            }
        }

        private bool MatchesSearch(DeviceItem device)
        {
            string needle = _searchText.Trim();
            return device.Name.IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0
                || (device.ModelName ?? string.Empty).IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0
                || (device.AddressText ?? string.Empty).IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        /// <summary>True when a search is active and nothing matched, so the grid shows its empty state.</summary>
        public bool HasNoSearchResults => _searchText.Trim().Length > 0 && !VisibleDevices.Any();

        /// <summary>What the user picked in Casting Options: a whole display or one window.</summary>
        public enum CastMode
        {
            Screen,
            Window
        }

        private CastMode _castMode = CastMode.Screen;
        public CastMode SelectedCastMode
        {
            get => _castMode;
            set
            {
                if (_castMode == value) return;
                _castMode = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsScreenMode));
                OnPropertyChanged(nameof(IsWindowMode));
                OnPropertyChanged(nameof(CanToggleCast));
                // The chosen source has to belong to the chosen mode, or the
                // primary button would cast a window the options say is off.
                AlignSelectedSourceToMode();
            }
        }

        public bool IsScreenMode => SelectedCastMode == CastMode.Screen;
        public bool IsWindowMode => SelectedCastMode == CastMode.Window;

        /// <summary>Internal so the Casting Options radio can re-align after its own change.</summary>
        internal void AlignSelectedSourceToMode()
        {
            int kind = _castMode == CastMode.Window
                ? (int)CastMirrorSourceKind.Window
                : (int)CastMirrorSourceKind.Monitor;
            if (SelectedSource != null && SelectedSource.Kind == kind) return;
            SelectedSource = Sources.FirstOrDefault(s => s.Kind == kind) ?? SelectedSource;
        }

        /// <summary>True when window capture exists on this machine; the option is hidden when it does not.</summary>
        public bool SupportsWindowCapture => Sources.Any(s => s.Kind == (int)CastMirrorSourceKind.Window);

        /// <summary>The name of the device a cast would go to, for the status line and the action button.</summary>
        public string TargetDeviceName => SelectedDevice?.Name ?? string.Empty;

        /// <summary>"4 devices" / "1 device" / "No devices" for the footer's device counter.</summary>
        public string DeviceCountText
        {
            get
            {
                if (Devices.Count == 0) return Localizer.T("No devices");
                if (Devices.Count == 1) return Localizer.Format("{0} device", Devices.Count);
                return Localizer.Format("{0} devices", Devices.Count);
            }
        }

        /// <summary>Headline for the status card: Connected, Connecting, or Ready to cast.</summary>
        public string SessionHeadline
        {
            get
            {
                if (IsStreaming) return Localizer.T("Connected");
                if (IsSessionActive) return Localizer.T("Connecting");
                return Localizer.T("Ready to cast");
            }
        }

        /// <summary>Second line under the status headline, explaining what the state means.</summary>
        public string SessionSubtitle
        {
            get
            {
                if (IsStreaming) return Localizer.Format("Streaming to {0}.", TargetDeviceName);
                if (IsSessionActive) return Localizer.T("Negotiating with the device.");
                if (SelectedDevice == null) return Localizer.T("Pick a device to begin.");
                return Localizer.Format("Ready to cast to {0}.", TargetDeviceName);
            }
        }

        /// <summary>Greets the signed-in user in the title bar, e.g. "Hello, HB".</summary>
        public string GreetingText
        {
            get
            {
                string name = Environment.UserName ?? string.Empty;
                string first = name.Split('\\', ' ').FirstOrDefault() ?? string.Empty;
                if (first.Length == 0) return Localizer.T("Hello");
                if (first.Length > 1) first = (char.ToUpperInvariant(first[0]) + first.Substring(1)).ToString();
                return Localizer.Format("Hello, {0}", first);
            }
        }

        /// <summary>Up to two initials for the avatar chip, e.g. "HB".</summary>
        public string UserInitials
        {
            get
            {
                string name = Environment.UserName ?? string.Empty;
                var parts = name.Split(new[] { '\\', ' ' }, StringSplitOptions.RemoveEmptyEntries)
                    .Where(p => char.IsLetterOrDigit(p[0]))
                    .Take(2)
                    .ToList();
                if (parts.Count == 0) return "?";
                return string.Concat(parts.Select(p => char.ToUpperInvariant(p[0])));
            }
        }

        /// <summary>The app version, shown in the footer's right-hand corner.</summary>
        public string VersionText
        {
            get
            {
                string version = typeof(MainViewModel).Assembly.GetName().Version?.ToString(3) ?? "1.0.0";
                return "v" + version;
            }
        }

        public string PreviewTitle
        {
            get
            {
                if (IsSessionActive) return Localizer.T(IsStreaming ? "Casting" : "Connecting...");
                if (NoDevicesFound) return Localizer.T("No Cast devices found");
                if (Devices.Count == 0) return Localizer.T("Looking for Cast devices...");
                return Localizer.T("Ready to cast");
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

        private const string ScanningMessage = "Scanning your Wi-Fi network for Cast devices.";
        private string _statusMessage = Localizer.T(ScanningMessage);
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

        private string _healthHint = string.Empty;
        /// <summary>Plain-language advice from the engine ("Wi-Fi is dropping packets..."), empty when healthy.</summary>
        public string HealthHint
        {
            get => _healthHint;
            private set
            {
                if (_healthHint == value) return;
                _healthHint = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(HasHealthHint));
            }
        }
        public bool HasHealthHint => !string.IsNullOrEmpty(_healthHint);

        private string _statsQualityText = string.Empty;
        public string StatsQualityText
        {
            get => _statsQualityText;
            private set
            {
                if (_statsQualityText == value) return;
                _statsQualityText = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(HasStatsQuality));
            }
        }

        /// <summary>
        /// True once a session is reporting quality. The footer's divider only
        /// belongs when there is something on both sides of it, so the idle shell
        /// does not show a separator with no text after it.
        /// </summary>
        public bool HasStatsQuality => !string.IsNullOrEmpty(_statsQualityText);

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

        public MainViewModel()
        {
            _dispatcher = DispatcherQueue.GetForCurrentThread();

            _stateCallback = OnStateChanged;
            _devicesCallback = OnDevicesChanged;

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
                ErrorMessage = Localizer.T("Select a Cast device first.");
                return;
            }
            if (source == null)
            {
                ErrorMessage = Localizer.T("Select a display or window to mirror.");
                return;
            }

            CastPending = true;
            ErrorMessage = string.Empty;
            StatusMessage = Localizer.Format("Connecting to {0}...", device.Name);

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
                    string message = started ? string.Empty : (ReadLastError() ?? Localizer.T("Failed to start casting."));
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
                StatusMessage = Localizer.T("Could not start casting.");
            }
        }

        private async Task StopCastAsync()
        {
            CastPending = true;
            StatusMessage = Localizer.T("Stopping...");
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
                StatusMessage = Localizer.T("Sources can't be refreshed while casting.");
                return;
            }

            try
            {
                var sources = new List<SourceItem>();

                int displayCount = CastCoreBridge.castmirror_get_display_count();
                for (int i = 0; i < displayCount; ++i)
                {
                    if (!CastCoreBridge.castmirror_get_display_info(i, out var display)) continue;
                    string name = string.IsNullOrWhiteSpace(display.Name) ? Localizer.Format("Display {0}", display.Id) : display.Name;
                    if (display.IsPrimary) name += " " + Localizer.T("(Primary)");
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
                            Name = string.IsNullOrWhiteSpace(window.Title) ? Localizer.Format("Window {0}", window.Id) : window.Title,
                            Detail = string.IsNullOrWhiteSpace(window.AppClass) ? Localizer.T("Window") : window.AppClass
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
                WindowSources.Clear();
                foreach (var source in sources)
                {
                    if (source.Kind == (int)CastMirrorSourceKind.Window) WindowSources.Add(source);
                }
                OnPropertyChanged(nameof(SupportsWindowCapture));
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
                NotificationService.Notify(Settings.NotifyOnEvents, Localizer.T("Connection lost"),
                    Localizer.Format("Reconnecting to {0}.", SelectedDevice?.Name ?? Localizer.T("the TV")));
            }

            if (state == CastMirrorState.Failed)
            {
                ErrorMessage = ReadLastError() ?? Localizer.T("The cast session failed.");
                StatusMessage = Localizer.T("Connection failed. Check the TV and try again.");
                NotificationService.Notify(Settings.NotifyOnEvents, Localizer.T("Casting failed"), ErrorMessage, "cast-failed");
            }
            else if (state == CastMirrorState.Streaming)
            {
                if (!string.IsNullOrEmpty(message)) StatusMessage = message;
                else StatusMessage = Localizer.Format("Streaming to {0}.", SelectedDevice?.Name ?? Localizer.T("the TV"));
                if (!wasStreaming)
                {
                    NotificationService.Notify(Settings.NotifyOnEvents, Localizer.T("Casting started"), StatusMessage);
                }
            }
            else if (state == CastMirrorState.Idle)
            {
                ResetStats();
                if (!HasError) StatusMessage = Localizer.T("Ready when you are.");
                if (wasActive)
                {
                    NotificationService.Notify(Settings.NotifyOnEvents, Localizer.T("Casting stopped"),
                        Localizer.T("The Cast session has ended."));
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

            // The status card and the footer's device line both read from these.
            OnPropertyChanged(nameof(SessionHeadline));
            OnPropertyChanged(nameof(SessionSubtitle));

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
                        StatusText = Localizer.T("Ready")
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

            // The "scanning" line is stale once a TV has turned up.
            if (Devices.Count > 0 && !IsSessionActive && StatusMessage == Localizer.T(ScanningMessage))
            {
                StatusMessage = Localizer.T("Choose a device, then press Start casting.");
            }

            if (changed)
            {
                NoDevicesFound = false;
                NotifyDeviceListChanged();
            }
        }

        private void OnDiscoveryTimedOut()
        {
            if (Devices.Count == 0 && !IsSessionActive)
            {
                NoDevicesFound = true;
                StatusMessage = Localizer.T("Make sure your TV or speaker is powered on and on the same Wi-Fi network.");
            }
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

            HealthHint = stats.RecoveryAttempt > 0
                ? Localizer.Format("Connection lost. Retrying for {0} s.", stats.RecoveryElapsedSeconds)
                : Localizer.T(stats.HealthHint);

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
            HealthHint = string.Empty;

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

        public void ApplyAudioProcess(string exeName)
        {
            Settings.AudioProcessName = exeName ?? string.Empty;
            PersistSettings();
        }

        public void ApplyGlobalHotkeys(bool enabled)
        {
            Settings.GlobalHotkeys = enabled;
            PersistSettings();
        }

        /// <summary>Shortcut action: start when idle, stop when a session is active.</summary>
        public void HotkeyToggleCast() => ToggleCast();

        public void HotkeyToggleFreeze()
        {
            if (IsStreaming) FreezeStream = !FreezeStream;
        }

        public void HotkeyToggleMute()
        {
            if (IsStreaming) MuteTvAudio = !MuteTvAudio;
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
            ErrorMessage = Localizer.T("Something went wrong inside CastMirror. Your cast keeps running. " +
                           "If it happens again, open Logs and copy the log into a bug report.");
        }

        /// <summary>Tells the user the last run ended in a crash and where the crash file is.</summary>
        public void ReportPreviousCrash(string dumpPath)
        {
            ErrorMessage = Localizer.Format("CastMirror closed unexpectedly last time. A crash file was saved in {0}. " +
                           "It contains a copy of the program's memory, so attach it to a bug report only if you're comfortable sharing that.",
                           System.IO.Path.GetDirectoryName(dumpPath));
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
            NotifyDeviceListChanged();
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
                StatusMessage = Localizer.T(ScanningMessage);
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
                ErrorMessage = Localizer.Format("\"{0}\" is not a valid IPv4 address (e.g. 192.168.1.50).", address);
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
                    StatusText = Localizer.T("Manual")
                };
                Devices.Add(existing);
                _manualDevices.Add(existing);
                NoDevicesFound = false;
                NotifyDeviceListChanged();
            }

            SelectedDevice = existing;
            StatusMessage = Localizer.Format("Added {0}.", address);
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
