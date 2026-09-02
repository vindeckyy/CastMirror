using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using CastMirror.Services;

namespace CastMirror.ViewModels
{
    public class DeviceItem
    {
        public string Id { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;
        public string ModelName { get; set; } = string.Empty;
        public string StatusText { get; set; } = "Ready";
    }

    public class MainViewModel : INotifyPropertyChanged
    {
        public ObservableCollection<DeviceItem> Devices { get; } = new();
        public ObservableCollection<string> Displays { get; } = new();

        private DeviceItem? _selectedDevice;
        public DeviceItem? SelectedDevice
        {
            get => _selectedDevice;
            set { _selectedDevice = value; OnPropertyChanged(); }
        }

        private bool _isStreaming;
        public bool IsStreaming
        {
            get => _isStreaming;
            set
            {
                _isStreaming = value;
                OnPropertyChanged();
                OnPropertyChanged(nameof(IsLiveVisible));
                OnPropertyChanged(nameof(ActionButtonText));
            }
        }

        public bool IsLiveVisible => IsStreaming;
        public bool IsSearchingVisible => Devices.Count == 0 && !IsStreaming;

        private string _actionButtonText = "Cast Display";
        public string ActionButtonText => IsStreaming ? "Stop Casting" : "Cast Display";

        private int _presetIndex = 0;
        public int PresetIndex
        {
            get => _presetIndex;
            set { _presetIndex = value; OnPropertyChanged(); }
        }

        private bool _audioEnabled = true;
        public bool AudioEnabled
        {
            get => _audioEnabled;
            set { _audioEnabled = value; OnPropertyChanged(); }
        }

        public string StatsFpsText { get; set; } = "FPS: 60.0";
        public string StatsBitrateText { get; set; } = "Bitrate: 6.0 Mbps";
        public string StatsLatencyText { get; set; } = "RTT: 12 ms";
        public string StatsLossText { get; set; } = "Loss: 0.0%";

        private readonly StateCallback _stateCallback;
        private readonly DevicesCallback _devicesCallback;
        private readonly StatsCallback _statsCallback;

        public MainViewModel()
        {
            Displays.Add("Primary Display");

            _stateCallback = OnStateChanged;
            _devicesCallback = OnDevicesChanged;
            _statsCallback = OnStatsUpdated;

            try
            {
                if (CastCoreBridge.castmirror_init())
                {
                    CastCoreBridge.castmirror_set_state_callback(_stateCallback, IntPtr.Zero);
                    CastCoreBridge.castmirror_set_devices_callback(_devicesCallback, IntPtr.Zero);
                    CastCoreBridge.castmirror_set_stats_callback(_statsCallback, IntPtr.Zero);
                    CastCoreBridge.castmirror_start_discovery();
                }
            }
            catch (DllNotFoundException)
            {
                // Fallback for standalone XAML preview without native library loaded
            }
        }

        public void ToggleCast()
        {
            if (IsStreaming)
            {
                CastCoreBridge.castmirror_stop_cast();
                IsStreaming = false;
            }
            else if (SelectedDevice != null)
            {
                uint bitrate = _presetIndex switch
                {
                    1 => 4000,
                    2 => 2500,
                    _ => 6000
                };
                bool ok = CastCoreBridge.castmirror_start_cast(SelectedDevice.Id, 0, 60, bitrate);
                if (ok)
                {
                    IsStreaming = true;
                }
            }
        }

        private void OnStateChanged(CastMirrorState state, string message, IntPtr userData)
        {
            IsStreaming = (state == CastMirrorState.Streaming);
        }

        private void OnDevicesChanged(int count, IntPtr userData)
        {
            Devices.Clear();
            for (int i = 0; i < count; ++i)
            {
                if (CastCoreBridge.castmirror_get_device_info(i, out var dev))
                {
                    Devices.Add(new DeviceItem
                    {
                        Id = dev.Id,
                        Name = dev.Name,
                        ModelName = dev.ModelName,
                        StatusText = "Ready"
                    });
                }
            }
            if (SelectedDevice == null && Devices.Count > 0)
            {
                SelectedDevice = Devices[0];
            }
            OnPropertyChanged(nameof(IsSearchingVisible));
        }

        private void OnStatsUpdated(ref CastMirrorStreamStats stats, IntPtr userData)
        {
            StatsFpsText = $"FPS: {stats.CurrentFps:F1}";
            StatsBitrateText = $"Bitrate: {stats.BitrateKbps / 1000.0:F1} Mbps";
            StatsLatencyText = $"RTT: {stats.RoundTripTimeMs:F0} ms";
            StatsLossText = $"Loss: {stats.PacketLossFraction * 100.0:F1}%";
            OnPropertyChanged(nameof(StatsFpsText));
            OnPropertyChanged(nameof(StatsBitrateText));
            OnPropertyChanged(nameof(StatsLatencyText));
            OnPropertyChanged(nameof(StatsLossText));
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        protected void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}
