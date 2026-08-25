using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml.Media;

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

        public event PropertyChangedEventHandler? PropertyChanged;
        protected void OnPropertyChanged([CallerMemberName] string? name = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }
}
