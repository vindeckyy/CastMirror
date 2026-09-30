using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using CastMirror.Services;
using CastMirror.ViewModels;

namespace CastMirror
{
    /// <summary>
    /// Settings surface mirroring the Linux settings tab, backed by the engine's
    /// persisted configuration (every change is written through
    /// <see cref="MainViewModel"/>, which also applies it live when a session is
    /// running).
    /// </summary>
    public sealed partial class SettingsWindow : Window
    {
        private static readonly int[] FpsOptions = { 0, 24, 25, 30, 50, 60 };
        private static readonly uint[] AudioQualityOptions = { 64000, 96000, 128000, 192000, 256000 };
        private static readonly int[] ReconnectOptions = { 30, 60, 120, 300, 600 };

        private readonly MainViewModel _viewModel;
        private readonly Action _desktopIntegrationChanged;
        private readonly Func<System.Collections.Generic.IReadOnlyList<string>> _applyHotkeys;

        // True until the first LoadFromSettings() completes: XAML load coerces
        // slider values and fires ValueChanged while later-named elements do not
        // exist yet, so handlers must not touch controls or save settings then.
        private bool _loading = true;

        public SettingsWindow(MainViewModel viewModel, Action desktopIntegrationChanged,
                              Func<System.Collections.Generic.IReadOnlyList<string>> applyHotkeys)
        {
            _viewModel = viewModel;
            _desktopIntegrationChanged = desktopIntegrationChanged;
            _applyHotkeys = applyHotkeys;
            InitializeComponent();

            // Logical (DIP) size, converted for the monitor this window opens
            // on. This window is tall, so the work-area clamp is what keeps it
            // on-screen on a laptop at 125-150%.
            WindowScaler.ResizeToDips(AppWindow, 700, 780);

            ThemeService.Register(Content as FrameworkElement);
            LoadFromSettings();

            // The Cast surface has its own bitrate slider; keep this one in step
            // with it (and with the engine config it writes).
            _viewModel.PropertyChanged += OnViewModelPropertyChanged;
            Closed += (_, _) => _viewModel.PropertyChanged -= OnViewModelPropertyChanged;
        }

        private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName != nameof(MainViewModel.BitrateCapKbps)) return;
            if (BitrateSlider == null) return;

            _loading = true;
            try
            {
                BitrateSlider.Value = Clamp(_viewModel.BitrateCapKbps, BitrateSlider.Minimum, BitrateSlider.Maximum);
            }
            finally
            {
                _loading = false;
            }
            UpdateBitrateLabel();
        }

        private void LoadFromSettings()
        {
            _loading = true;
            try
            {
                CastMirrorSettings settings = _viewModel.Settings;

                BitrateSlider.Value = Clamp(settings.EffectiveBitrateCapKbps(), BitrateSlider.Minimum, BitrateSlider.Maximum);
                UpdateBitrateLabel();

                // Unknown persisted values stay unselected instead of being
                // silently coerced to a default that the next save would write
                // over the user's hand-edited value.
                int fpsIndex = Array.IndexOf(FpsOptions, settings.CaptureFps);
                if (fpsIndex < 0)
                {
                    NoteUnsupportedValue("capture_fps", settings.CaptureFps);
                }
                else
                {
                    FpsCombo.SelectedIndex = fpsIndex;
                }

                SteadySwitch.IsOn = !settings.AdaptiveResolutionEnabled;
                SoftwareEncodeSwitch.IsOn = settings.ForceSoftwareEncode;
                AudioSwitch.IsOn = settings.AudioEnabled;

                int audioIndex = Array.IndexOf(AudioQualityOptions, settings.AudioBitrateBps);
                if (audioIndex < 0)
                {
                    NoteUnsupportedValue("audio_bitrate_bps", settings.AudioBitrateBps);
                }
                else
                {
                    AudioQualityCombo.SelectedIndex = audioIndex;
                }


                DelaySlider.Value = Clamp(settings.TargetDelayMs, DelaySlider.Minimum, DelaySlider.Maximum);
                UpdateDelayLabel();

                LatencyHudSwitch.IsOn = settings.LatencyHudEnabled;
                CursorSwitch.IsOn = settings.ShowCursor;
                int reconnectIndex = Array.IndexOf(ReconnectOptions, settings.ReconnectWindowSeconds);
                if (reconnectIndex < 0)
                {
                    NoteUnsupportedValue("reconnect_window_s", settings.ReconnectWindowSeconds);
                }
                else
                {
                    ReconnectCombo.SelectedIndex = reconnectIndex;
                }
                SubnetScanSwitch.IsOn = settings.SubnetScanEnabled;
                TraySwitch.IsOn = settings.EnableTrayOnStartup;
                CloseToTraySwitch.IsOn = settings.CloseToTray;
                AutostartSwitch.IsOn = AutostartService.IsEnabled;
                HotkeySwitch.IsOn = settings.GlobalHotkeys;

                NotifySwitch.IsOn = settings.NotifyOnEvents && NotificationService.IsSupported;
                NotifySwitch.IsEnabled = NotificationService.IsSupported;
                NotifyHelpText.Text = NotificationService.IsSupported
                    ? "Shows a notification when casting starts or stops, when the connection drops, and when a cast fails."
                    : "Windows would not register CastMirror for notifications, so this is unavailable.";

                string uiTheme = (settings.UiTheme ?? string.Empty).Trim();
                int themeIndex = uiTheme switch
                {
                    "" => 0,
                    "light" => 1,
                    "dark" => 2,
                    _ => -1
                };
                if (themeIndex < 0)
                {
                    NoteUnsupportedValue("ui_theme", settings.UiTheme);
                }
                else
                {
                    ThemeCombo.SelectedIndex = themeIndex;
                }

                // The reload succeeded: any earlier error text is stale.
                ClearError();
            }
            catch (Exception ex)
            {
                ShowError(ex.Message);
                MainViewModel.LogError(ex);
            }
            finally
            {
                _loading = false;
            }
        }

        private static double Clamp(double value, double minimum, double maximum)
        {
            if (value < minimum) return minimum;
            if (value > maximum) return maximum;
            return value;
        }

        private void UpdateBitrateLabel()
        {
            if (BitrateValueText == null) return;
            BitrateValueText.Text = $"{BitrateSlider.Value / 1000.0:F1} Mbps";
        }

        private void UpdateDelayLabel()
        {
            if (DelayValueText == null) return;
            DelayValueText.Text = $"{DelaySlider.Value:F0} ms";
        }

        private void ShowError(string message)
        {
            SaveErrorText.Text = $"Could not save settings: {message}";
            SaveErrorText.Visibility = Visibility.Visible;
        }

        /// <summary>
        /// Hides the error banner. Called after a successful load and after
        /// every applied change: the banner reports the LAST failure, so a
        /// later successful save must not leave it visible.
        /// </summary>
        private void ClearError()
        {
            SaveErrorText.Visibility = Visibility.Collapsed;
        }

        private static void NoteUnsupportedValue(string key, object? value)
        {
            // Not an error the user must fix on the spot; recorded so a
            // hand-edited value that no control offers is visible in the log.
            MainViewModel.LogError(new InvalidDataException(
                $"Unsupported persisted value {key}={value}; the settings window left it untouched."));
        }

        private void OnBitrateChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            UpdateBitrateLabel();
            if (_loading) return;
            _viewModel.ApplyBitrateCap((uint)e.NewValue);
            ClearError();
        }

        private void OnDelayChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            UpdateDelayLabel();
            if (_loading) return;
            _viewModel.ApplyTargetDelay((int)e.NewValue);
            ClearError();
        }

        private void OnFpsChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            int index = FpsCombo.SelectedIndex;
            if (index < 0 || index >= FpsOptions.Length) return;
            _viewModel.ApplyCaptureFps(FpsOptions[index]);
            ClearError();
        }

        private void OnAudioQualityChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            int index = AudioQualityCombo.SelectedIndex;
            if (index < 0 || index >= AudioQualityOptions.Length) return;
            _viewModel.ApplyAudioBitrate(AudioQualityOptions[index]);
            ClearError();
        }

        private void OnSteadyToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplySteadyFrameRate(SteadySwitch.IsOn);
            ClearError();
        }

        private void OnSoftwareEncodeToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyForceSoftwareEncode(SoftwareEncodeSwitch.IsOn);
            ClearError();
        }

        private void OnAudioToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyAudioEnabled(AudioSwitch.IsOn);
            ClearError();
        }

        private void OnReconnectChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            int index = ReconnectCombo.SelectedIndex;
            if (index < 0 || index >= ReconnectOptions.Length) return;
            _viewModel.ApplyReconnectWindow(ReconnectOptions[index]);
            ClearError();
        }

        private void OnCursorToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyShowCursor(CursorSwitch.IsOn);
            ClearError();
        }

        private void OnLatencyHudToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyLatencyHud(LatencyHudSwitch.IsOn);
            ClearError();
        }

        private async void OnSubnetScanToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            if (!SubnetScanSwitch.IsOn)
            {
                _viewModel.ApplySubnetScan(false);
                ClearError();
                return;
            }

            if (await Dialogs.ConfirmSubnetScanAsync(Content.XamlRoot))
            {
                _viewModel.ApplySubnetScan(true);
                ClearError();
            }
            else
            {
                _loading = true;
                SubnetScanSwitch.IsOn = false;
                _loading = false;
            }
        }

        private void OnTrayToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyTrayEnabled(TraySwitch.IsOn);
            ClearError();
            _desktopIntegrationChanged?.Invoke();
        }

        private void OnCloseToTrayToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyCloseToTray(CloseToTraySwitch.IsOn);
            ClearError();
            _desktopIntegrationChanged?.Invoke();
        }

        private void OnHotkeysToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyGlobalHotkeys(HotkeySwitch.IsOn);
            var taken = _applyHotkeys();
            if (taken.Count > 0)
            {
                ShowError($"{string.Join(", ", taken)} is already used by another app. The other shortcuts still work.");
                return;
            }
            ClearError();
        }

        private void OnAutostartToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            if (!AutostartService.Set(AutostartSwitch.IsOn))
            {
                ShowError("Windows would not let CastMirror change its sign-in entry.");
                _loading = true;
                AutostartSwitch.IsOn = AutostartService.IsEnabled;
                _loading = false;
                return;
            }
            ClearError();
        }

        private void OnNotifyToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyNotifications(NotifySwitch.IsOn);
            ClearError();
        }

        private void OnThemeChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            // No selection means the persisted value was unsupported; keep it
            // until the user picks a real entry.
            if (ThemeCombo.SelectedIndex < 0) return;
            string theme = ThemeCombo.SelectedIndex switch
            {
                1 => "light",
                2 => "dark",
                _ => string.Empty
            };
            _viewModel.ApplyTheme(theme);
            ClearError();
        }

        private async void OnSelfTestClicked(object sender, RoutedEventArgs e)
        {
            // The engine's self-test probes capture, encoder, audio and network
            // and blocks for seconds; run it off the UI thread.
            if (sender is Button button) button.IsEnabled = false;
            try
            {
                string json = await Task.Run(SettingsService.RunSelfTest);
                // WinUI installs a DispatcherQueue sync context, so this
                // continuation is back on the UI thread.
                SelfTestText.Text = DescribeSelfTest(json);
                SelfTestText.Visibility = Visibility.Visible;
            }
            finally
            {
                if (sender is Button doneButton) doneButton.IsEnabled = true;
            }

            var dialog = new ContentDialog
            {
                Title = "Self-test",
                Content = new TextBlock { Text = SelfTestText.Text, TextWrapping = TextWrapping.Wrap },
                CloseButtonText = "Close"
            };
            await Dialogs.ShowAsync(dialog, Content.XamlRoot);
        }

        private static string DescribeSelfTest(string json)
        {
            if (string.IsNullOrEmpty(json)) return "Self-test could not run.";

            try
            {
                using JsonDocument document = JsonDocument.Parse(json);
                var lines = new System.Collections.Generic.List<string>();
                foreach (string key in new[] { "capture", "encoder", "audio", "network" })
                {
                    if (!document.RootElement.TryGetProperty(key, out JsonElement section)) continue;
                    bool ok = section.TryGetProperty("ok", out JsonElement okElement) &&
                              okElement.ValueKind == JsonValueKind.True;
                    string detail = section.TryGetProperty("detail", out JsonElement detailElement)
                        ? detailElement.GetString() ?? string.Empty
                        : string.Empty;
                    lines.Add($"{(ok ? "\u2713" : "\u2717")} {key}: {detail}");
                }
                return string.Join(Environment.NewLine, lines);
            }
            catch (JsonException)
            {
                return json;
            }
        }

        private void OnOpenLogsClicked(object sender, RoutedEventArgs e)
        {
            try
            {
                Directory.CreateDirectory(Services.LogService.DirectoryPath);
                Process.Start(new ProcessStartInfo("explorer.exe", Services.LogService.DirectoryPath) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                ShowError(ex.Message);
                MainViewModel.LogError(ex);
            }
        }

        /// <summary>Re-reads the engine configuration (e.g. after an external edit).</summary>
        public void Reload()
        {
            _viewModel.ReloadSettings();
            LoadFromSettings();
        }
    }
}
