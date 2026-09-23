using System;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
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

        private readonly MainViewModel _viewModel;
        private readonly Action _desktopIntegrationChanged;

        // True until the first LoadFromSettings() completes: XAML load coerces
        // slider values and fires ValueChanged while later-named elements do not
        // exist yet, so handlers must not touch controls or save settings then.
        private bool _loading = true;

        public SettingsWindow(MainViewModel viewModel, Action desktopIntegrationChanged)
        {
            _viewModel = viewModel;
            _desktopIntegrationChanged = desktopIntegrationChanged;
            InitializeComponent();

            try
            {
                AppWindow.Resize(new Windows.Graphics.SizeInt32(700, 780));
            }
            catch
            {
                // Sizing is best-effort.
            }

            ThemeService.Register(Content as FrameworkElement);
            LoadFromSettings();
        }

        private void LoadFromSettings()
        {
            _loading = true;
            try
            {
                CastMirrorSettings settings = _viewModel.Settings;

                BitrateSlider.Value = Clamp(settings.EffectiveBitrateCapKbps(), BitrateSlider.Minimum, BitrateSlider.Maximum);
                UpdateBitrateLabel();

                FpsCombo.SelectedIndex = Math.Max(0, Array.IndexOf(FpsOptions, settings.CaptureFps));

                SteadySwitch.IsOn = !settings.AdaptiveResolutionEnabled;
                SoftwareEncodeSwitch.IsOn = settings.ForceSoftwareEncode;
                AudioSwitch.IsOn = settings.AudioEnabled;

                int audioIndex = Array.IndexOf(AudioQualityOptions, settings.AudioBitrateBps);
                AudioQualityCombo.SelectedIndex = audioIndex >= 0 ? audioIndex : 2;

                SilenceSwitch.IsOn = settings.SilenceHostSpeakers;

                DelaySlider.Value = Clamp(settings.TargetDelayMs, DelaySlider.Minimum, DelaySlider.Maximum);
                UpdateDelayLabel();

                LatencyHudSwitch.IsOn = settings.LatencyHudEnabled;
                SubnetScanSwitch.IsOn = settings.SubnetScanEnabled;
                TraySwitch.IsOn = settings.EnableTrayOnStartup;
                CloseToTraySwitch.IsOn = settings.CloseToTray;

                NotifySwitch.IsOn = settings.NotifyOnEvents && NotificationService.IsSupported;
                NotifySwitch.IsEnabled = NotificationService.IsSupported;
                NotifyHelpText.Text = NotificationService.IsSupported
                    ? "Shows desktop notifications when casting starts, disconnects, or reconnects."
                    : "Not supported in this build: the notification platform could not be registered.";

                ThemeCombo.SelectedIndex = settings.UiTheme switch
                {
                    "light" => 1,
                    "dark" => 2,
                    _ => 0
                };
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

        private void ClearError()
        {
            SaveErrorText.Visibility = Visibility.Collapsed;
        }

        private void OnBitrateChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            UpdateBitrateLabel();
            if (_loading) return;
            _viewModel.ApplyBitrateCap((uint)e.NewValue);
        }

        private void OnDelayChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            UpdateDelayLabel();
            if (_loading) return;
            _viewModel.ApplyTargetDelay((int)e.NewValue);
        }

        private void OnFpsChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            int index = FpsCombo.SelectedIndex;
            if (index < 0 || index >= FpsOptions.Length) return;
            _viewModel.ApplyCaptureFps(FpsOptions[index]);
        }

        private void OnAudioQualityChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            int index = AudioQualityCombo.SelectedIndex;
            if (index < 0 || index >= AudioQualityOptions.Length) return;
            _viewModel.ApplyAudioBitrate(AudioQualityOptions[index]);
        }

        private void OnSteadyToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplySteadyFrameRate(SteadySwitch.IsOn);
        }

        private void OnSoftwareEncodeToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyForceSoftwareEncode(SoftwareEncodeSwitch.IsOn);
        }

        private void OnAudioToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyAudioEnabled(AudioSwitch.IsOn);
        }

        private void OnLatencyHudToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyLatencyHud(LatencyHudSwitch.IsOn);
        }

        private async void OnSubnetScanToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            if (!SubnetScanSwitch.IsOn)
            {
                _viewModel.ApplySubnetScan(false);
                return;
            }

            var dialog = new ContentDialog
            {
                Title = "Scan LAN for silent TVs",
                Content = new TextBlock
                {
                    Text = "This sends a short TCP connection probe to every IP address on your local " +
                           "subnet on port 8009.\n\nLeave this off on corporate, school, or guest Wi-Fi " +
                           "networks.\n\nDo you want to enable subnet scanning?",
                    TextWrapping = TextWrapping.Wrap
                },
                PrimaryButtonText = "Enable",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Close,
                XamlRoot = Content.XamlRoot
            };

            ContentDialogResult result = await dialog.ShowAsync();
            if (result == ContentDialogResult.Primary)
            {
                _viewModel.ApplySubnetScan(true);
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
            _desktopIntegrationChanged?.Invoke();
        }

        private void OnCloseToTrayToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyCloseToTray(CloseToTraySwitch.IsOn);
            _desktopIntegrationChanged?.Invoke();
        }

        private void OnNotifyToggled(object sender, RoutedEventArgs e)
        {
            if (_loading) return;
            _viewModel.ApplyNotifications(NotifySwitch.IsOn);
        }

        private void OnThemeChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_loading) return;
            string theme = ThemeCombo.SelectedIndex switch
            {
                1 => "light",
                2 => "dark",
                _ => string.Empty
            };
            _viewModel.ApplyTheme(theme);
        }

        private async void OnSelfTestClicked(object sender, RoutedEventArgs e)
        {
            string json = SettingsService.RunSelfTest();
            SelfTestText.Text = DescribeSelfTest(json);
            SelfTestText.Visibility = Visibility.Visible;

            var dialog = new ContentDialog
            {
                Title = "Self-test",
                Content = new TextBlock { Text = SelfTestText.Text, TextWrapping = TextWrapping.Wrap },
                CloseButtonText = "Close",
                XamlRoot = Content.XamlRoot
            };
            await dialog.ShowAsync();
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
                string dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "CastMirror");
                Directory.CreateDirectory(dir);
                Process.Start(new ProcessStartInfo("explorer.exe", dir) { UseShellExecute = true });
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
            ClearError();
            LoadFromSettings();
        }
    }
}
