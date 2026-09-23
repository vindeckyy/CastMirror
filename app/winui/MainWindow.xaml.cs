using System;
using System.IO;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using CastMirror.Services;
using CastMirror.ViewModels;

namespace CastMirror
{
    public sealed partial class MainWindow : Window
    {
        public MainViewModel ViewModel { get; } = new MainViewModel();

        private SettingsWindow? _settingsWindow;
        private TrayIconService? _tray;
        private bool _exiting;

        public MainWindow()
        {
            this.InitializeComponent();
            Title = "CastMirror";
            try
            {
                AppWindow.Resize(new Windows.Graphics.SizeInt32(1100, 720));
            }
            catch
            {
                // Resizing is best-effort; the default size is fine.
            }

            ThemeService.Register(Content as FrameworkElement);
            ThemeService.Apply(ViewModel.Settings.UiTheme);

            ViewModel.StateChanged += OnSessionStateChanged;
            SyncTrayIcon();

            AppWindow.Closing += OnWindowClosing;
            Closed += OnWindowClosed;
        }

        private void OnActionButtonClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.ToggleCast();
        }

        private void OnRefreshSourcesClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.RefreshSources();
        }

        private void OnRescanClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.RescanDevices();
        }

        private async void OnAddByIpClicked(object sender, RoutedEventArgs e)
        {
            var input = new TextBox
            {
                PlaceholderText = "192.168.1.50",
                HorizontalAlignment = HorizontalAlignment.Stretch
            };
            var dialog = new ContentDialog
            {
                Title = "Add by IP",
                Content = new StackPanel
                {
                    Spacing = 8,
                    Children =
                    {
                        new TextBlock
                        {
                            Text = "Enter the IP address of a Cast device that mDNS discovery cannot see.",
                            TextWrapping = TextWrapping.Wrap
                        },
                        input
                    }
                },
                PrimaryButtonText = "Add",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Primary,
                XamlRoot = Content.XamlRoot
            };

            ContentDialogResult result = await dialog.ShowAsync();
            if (result != ContentDialogResult.Primary) return;

            string address = input.Text.Trim();
            if (address.Length == 0)
            {
                ViewModel.ErrorMessage = "Enter an IP address to add a device.";
                return;
            }
            ViewModel.AddDeviceByIp(address);
        }

        private void OnDismissErrorClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.DismissError();
        }

        private void OnOpenSettingsClicked(object sender, RoutedEventArgs e)
        {
            if (_settingsWindow == null)
            {
                _settingsWindow = new SettingsWindow(ViewModel, SyncTrayIcon);
                _settingsWindow.Closed += (_, _) => _settingsWindow = null;
            }

            _settingsWindow.Activate();
        }

        /// <summary>Shows or hides the tray icon to match the saved preference.</summary>
        private void SyncTrayIcon()
        {
            if (ViewModel.Settings.EnableTrayOnStartup)
            {
                if (_tray == null)
                {
                    _tray = new TrayIconService(
                        Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread(),
                        ShowFromTray,
                        () => ViewModel.ToggleCast(),
                        ExitFromTray,
                        () => ViewModel.IsStreaming);
                }
                if (!_tray.IsActive)
                {
                    _tray.Start();
                }
                UpdateTrayTooltip();
            }
            else if (_tray != null)
            {
                _tray.Dispose();
                _tray = null;
            }
        }

        private void ShowFromTray()
        {
            RestoreWindow();
        }

        private void ExitFromTray()
        {
            _exiting = true;
            Close();
        }

        private void RestoreWindow()
        {
            try
            {
                AppWindow.Show();
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
            Activate();
        }

        private void OnSessionStateChanged(CastMirrorState state)
        {
            UpdateTrayTooltip();
        }

        private void UpdateTrayTooltip()
        {
            if (_tray == null) return;
            _tray.SetTooltip(ViewModel.IsStreaming
                ? $"CastMirror - casting to {ViewModel.SelectedDevice?.Name ?? "TV"}"
                : "CastMirror");
        }

        private void OnWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
        {
            if (_exiting) return;
            if (!ViewModel.Settings.CloseToTray) return;
            if (_tray == null || !_tray.IsActive) return;

            // Keep the session alive in the tray instead of quitting.
            args.Cancel = true;
            try
            {
                AppWindow.Hide();
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
        }

        private void OnWindowClosed(object sender, WindowEventArgs args)
        {
            ViewModel.Shutdown();
            _tray?.Dispose();
            _tray = null;
            NotificationService.Shutdown();
        }
    }
}
