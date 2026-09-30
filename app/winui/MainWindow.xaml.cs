using System;
using System.IO;
using System.Threading;
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
        private LogsWindow? _logsWindow;
        private TrayIconService? _tray;
        private bool _exiting;

        public MainWindow()
        {
            this.InitializeComponent();
            Title = "CastMirror";
            // 1100x720 is a logical (DIP) size. AppWindow.Resize takes physical
            // pixels, so passing those numbers raw would give a window that
            // shrinks as the display scale rises - 550x360 DIPs at 200%, with
            // the cast panel scrolled and unusable. WindowScaler converts to
            // the monitor's scale and clamps to its work area.
            WindowScaler.ResizeToDips(AppWindow, 1100, 720);

            ThemeService.Register(Content as FrameworkElement);
            ThemeService.Apply(ViewModel.Settings.UiTheme);

            ViewModel.StateChanged += OnSessionStateChanged;
            NotificationService.Activated += OnNotificationActivated;
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

        private void OnOpenLogsClicked(object sender, RoutedEventArgs e)
        {
            if (_logsWindow == null)
            {
                _logsWindow = new LogsWindow();
                _logsWindow.Closed += (_, _) => _logsWindow = null;
            }

            _logsWindow.Activate();
        }

        private void OnFirstRunScanClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.CompleteFirstRun(enableSubnetScan: true);
        }

        private void OnFirstRunDismissClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.CompleteFirstRun(enableSubnetScan: false);
        }

        /// <summary>
        /// Brings the window forward when the user clicks a toast. Both
        /// actionable toasts (a failed cast, a failed tray hide) just restore
        /// the window, so anything else is ignored.
        /// </summary>
        private void OnNotificationActivated(string? argument)
        {
            if (argument == null ||
                (argument.IndexOf("cast-failed", StringComparison.OrdinalIgnoreCase) < 0 &&
                 argument.IndexOf("tray-unavailable", StringComparison.OrdinalIgnoreCase) < 0))
            {
                return;
            }

            DispatcherQueue.TryEnqueue(RestoreWindow);
        }

        /// <summary>Shows or hides the tray icon to match the saved preference.</summary>
        private void SyncTrayIcon()
        {
            if (ViewModel.Settings.EnableTrayOnStartup)
            {
                TrayIconService tray = EnsureTray();
                if (!tray.IsActive)
                {
                    StartTrayInBackground(tray);
                }
                UpdateTrayTooltip();
            }
            else if (_tray != null)
            {
                _tray.Dispose();
                _tray = null;
            }
        }

        /// <summary>Creates the tray icon on first use; never returns null.</summary>
        private TrayIconService EnsureTray()
        {
            return _tray ??= new TrayIconService(
                Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread(),
                ShowFromTray,
                () => ViewModel.ToggleCast(),
                ExitFromTray,
                () => ViewModel.IsStreaming);
        }

        /// <summary>
        /// Starts the tray icon without blocking the caller.
        ///
        /// TrayIconService.Start() waits on its tray thread for the shell to
        /// acknowledge the icon, which can take seconds. Called from
        /// SyncTrayIcon() during construction that would stall the first frame
        /// of the window, so the wait happens on a worker and the caller only
        /// learns the result (for the tooltip) afterwards.
        /// </summary>
        private void StartTrayInBackground(TrayIconService tray)
        {
            Thread? worker = null;
            worker = new Thread(() =>
            {
                bool started = false;
                try
                {
                    started = tray.Start();
                }
                catch (Exception ex)
                {
                    ViewModels.MainViewModel.LogError(ex);
                }
                finally
                {
                    GC.KeepAlive(worker);
                }

                // The outcome is only known here, on the worker: reporting a
                // failure synchronously would be a false positive, because the
                // icon has not been added yet at the call site. Anything that
                // touches the window hops back to the UI thread.
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (started)
                    {
                        UpdateTrayTooltip();
                        return;
                    }
                    NotificationService.Notify(
                        ViewModel.Settings.NotifyOnEvents,
                        "CastMirror",
                        "Still running in the background; the tray icon could not be created, so relaunch CastMirror to reopen the window.",
                        "tag=tray-unavailable");
                });
            })
            {
                IsBackground = true,
                Name = "CastMirrorTrayStart"
            };
            worker.Start();
        }

        private void ShowFromTray()
        {
            RestoreWindow();
        }

        /// <summary>
        /// Tray "Exit" goes through the same window-close path as the title
        /// bar, so an active cast session gets the same confirmation either way.
        /// </summary>
        private void ExitFromTray()
        {
            // The user asked to quit, so the close handler must not turn this
            // back into a hide. Without this, CloseToTray (on by default) makes
            // OnWindowClosing cancel the very close this method asked for: the
            // window re-hides and the app is unquittable from the tray.
            _exiting = true;
            RestoreWindow();
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
            if (ViewModel.Settings.CloseToTray)
            {
                // Keep the session alive instead of quitting. CloseToTray is
                // the intent; the tray icon is merely the way back in, so it
                // is created on demand and a failed shell add only costs the
                // restore path — never the running cast.
                args.Cancel = true;
                TrayIconService tray = EnsureTray();
                if (!tray.IsActive)
                {
                    // Async for the same reason as SyncTrayIcon: this runs inside
                    // the window-close handler, so a multi-second shell wait here
                    // would leave the window frozen on screen instead of hidden.
                    StartTrayInBackground(tray);
                }
                try
                {
                    AppWindow.Hide();
                }
                catch (Exception ex)
                {
                    ViewModels.MainViewModel.LogError(ex);
                }
                return;
            }

            if (ViewModel.IsSessionActive)
            {
                // Quitting mid-cast drops the session without warning.
                args.Cancel = true;
                ConfirmExitAndCloseAsync();
            }
        }

        private async void ConfirmExitAndCloseAsync()
        {
            var dialog = new ContentDialog
            {
                Title = "Casting in progress",
                Content = "A cast session is still running. Quit CastMirror and stop casting?",
                PrimaryButtonText = "Quit",
                CloseButtonText = "Keep casting",
                DefaultButton = ContentDialogButton.Close,
                XamlRoot = Content.XamlRoot
            };

            ContentDialogResult result = await dialog.ShowAsync();
            if (result != ContentDialogResult.Primary) return;
            _exiting = true;
            Close();
        }

        private void OnWindowClosed(object sender, WindowEventArgs args)
        {
            NotificationService.Activated -= OnNotificationActivated;
            // Close the auxiliary windows too. They are independent AppWindows,
            // so they survive this one closing and are left floating over an app
            // whose engine has already been shut down — controls bound to a dead
            // engine, and a second taskbar entry for a closing process.
            CloseAuxiliaryWindows();
            ViewModel.Shutdown();
            _tray?.Dispose();
            _tray = null;
            NotificationService.Shutdown();
        }

        /// <summary>
        /// Closes the Settings and Logs windows, clearing their fields first so
        /// the Closed handlers do not race with the reassignment.
        /// </summary>
        private void CloseAuxiliaryWindows()
        {
            SettingsWindow? settings = _settingsWindow;
            _settingsWindow = null;
            try
            {
                settings?.Close();
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }

            LogsWindow? logs = _logsWindow;
            _logsWindow = null;
            try
            {
                logs?.Close();
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
        }
    }
}
