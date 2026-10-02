using System;
using System.IO;
using System.Threading;
using System.ComponentModel;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
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
        private HotkeyService? _hotkeys;

        // The shell is a three-column dashboard (rail, content, casting rail), so
        // below these the casting rail is the first thing to be crushed.
        private const int MinWidthDip = 1040;
        private const int MinHeightDip = 640;
        private const int DefaultWidthDip = 1280;
        private const int DefaultHeightDip = 800;

        /// <summary>Registers or releases the global shortcuts to match the setting; returns any that were taken.</summary>
        public System.Collections.Generic.IReadOnlyList<string> ApplyHotkeySetting()
        {
            if (_hotkeys == null) return Array.Empty<string>();
            if (!ViewModel.Settings.GlobalHotkeys)
            {
                _hotkeys.Disable();
                return Array.Empty<string>();
            }
            return _hotkeys.Enable();
        }

        private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(MainViewModel.ShowFirstRun)) UpdateFirstRunFocusTrap();
        }

        /// <summary>
        /// While the welcome card is up the controls behind it are disabled, so Tab
        /// and screen readers stay on the card, and focus starts on its first button.
        /// </summary>
        private void UpdateFirstRunFocusTrap()
        {
            bool showing = ViewModel.ShowFirstRun;
            MainContent.IsEnabled = !showing;
            if (showing)
            {
                DispatcherQueue.TryEnqueue(() => FirstRunScanButton.Focus(FocusState.Programmatic));
            }
        }

        private void AddKeyboardShortcuts()
        {
            void Add(VirtualKey key, VirtualKeyModifiers modifiers, Action action)
            {
                var accelerator = new KeyboardAccelerator { Key = key, Modifiers = modifiers };
                accelerator.Invoked += (_, args) =>
                {
                    args.Handled = true;
                    if (!ViewModel.ShowFirstRun) action();
                };
                RootGrid.KeyboardAccelerators.Add(accelerator);
            }

            Add(VirtualKey.Enter, VirtualKeyModifiers.Control, () => ViewModel.ToggleCast());
            Add(VirtualKey.F5, VirtualKeyModifiers.None, () => ViewModel.RescanDevices());
            Add((VirtualKey)188 /* comma */, VirtualKeyModifiers.Control, () => OnOpenSettingsClicked(this, new RoutedEventArgs()));
            Add(VirtualKey.L, VirtualKeyModifiers.Control, () => OnOpenLogsClicked(this, new RoutedEventArgs()));
            Add(VirtualKey.K, VirtualKeyModifiers.Control, FocusSearch);
        }

        /// <summary>
        /// Lets the shell's own title bar replace the system one. The interactive
        /// controls (search, buttons) keep their input because they are not part
        /// of the drag region; only the empty stretches of the bar drag the
        /// window.
        /// </summary>
        private void UseCustomTitleBar()
        {
            ExtendsContentIntoTitleBar = true;
            // Tall caption buttons (48 DIP) match the bar's height, so the glyphs
            // sit on the same centre line as the controls beside them.
            AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
            SetTitleBar(TitleBarGrid);
            // RightInset is in physical pixels and changes with the display scale.
            TitleBarGrid.SizeChanged += (_, _) =>
                CaptionInsetColumn.Width = new GridLength(
                    AppWindow.TitleBar.RightInset / WindowScaler.ScaleOf(AppWindow));
        }

        private async void OnAboutClicked(object sender, RoutedEventArgs e)
        {
            string version = System.Reflection.Assembly.GetExecutingAssembly()
                .GetName().Version?.ToString(3) ?? "1.0.0";
            var dialog = new ContentDialog
            {
                Title = $"CastMirror {version}",
                SecondaryButtonText = Localizer.T("Check for updates"),
                Content = new StackPanel
                {
                    Spacing = 8,
                    Children =
                    {
                        new TextBlock
                        {
                            Text = Localizer.T("Sends your screen and system audio to a Chromecast or Google TV over your local network."),
                            TextWrapping = TextWrapping.Wrap
                        },
                        new TextBlock
                        {
                            Text = Localizer.Format("CastMirror sends nothing to any server and collects no telemetry. Settings and logs are in {0}.", LogService.DirectoryPath),
                            TextWrapping = TextWrapping.Wrap,
                            Opacity = 0.8
                        }
                    }
                },
                PrimaryButtonText = Localizer.T("Open folder"),
                CloseButtonText = Localizer.T("Close"),
                DefaultButton = ContentDialogButton.Close
            };
            ContentDialogResult choice = await Dialogs.ShowAsync(dialog, Content.XamlRoot);
            if (choice == ContentDialogResult.Secondary)
            {
                await CheckForUpdatesAsync();
                return;
            }
            if (choice == ContentDialogResult.Primary)
            {
                try
                {
                    System.IO.Directory.CreateDirectory(LogService.DirectoryPath);
                    System.Diagnostics.Process.Start(
                        new System.Diagnostics.ProcessStartInfo("explorer.exe", LogService.DirectoryPath)
                        { UseShellExecute = true });
                }
                catch (Exception ex)
                {
                    MainViewModel.LogError(ex);
                }
            }
        }

        /// <summary>Runs only on request; CastMirror never checks for updates by itself.</summary>
        private async System.Threading.Tasks.Task CheckForUpdatesAsync()
        {
            string title;
            string body;
            string? pageUrl = null;
            try
            {
                UpdateInfo? update = await UpdateService.CheckAsync();
                if (update == null)
                {
                    title = Localizer.T("You're up to date");
                    body = Localizer.Format("CastMirror {0} is the latest release.", UpdateService.Current.ToString(3));
                }
                else
                {
                    title = Localizer.Format("CastMirror {0} is available", update.Latest.ToString(3));
                    body = Localizer.T("Open the release page to download it.");
                    pageUrl = update.PageUrl;
                }
            }
            catch (Exception ex)
            {
                MainViewModel.LogError(ex);
                title = Localizer.T("Couldn't check for updates");
                body = Localizer.T("GitHub didn't answer. Check your connection and try again.");
            }

            var result = new ContentDialog
            {
                Title = title,
                Content = new TextBlock { Text = body, TextWrapping = TextWrapping.Wrap },
                PrimaryButtonText = pageUrl != null ? Localizer.T("Open release page") : string.Empty,
                CloseButtonText = Localizer.T("Close"),
                DefaultButton = pageUrl != null ? ContentDialogButton.Primary : ContentDialogButton.Close
            };
            if (await Dialogs.ShowAsync(result, Content.XamlRoot) == ContentDialogResult.Primary && pageUrl != null)
            {
                try
                {
                    System.Diagnostics.Process.Start(
                        new System.Diagnostics.ProcessStartInfo(pageUrl) { UseShellExecute = true });
                }
                catch (Exception ex)
                {
                    MainViewModel.LogError(ex);
                }
            }
        }

        private void OnDeviceFlyoutOpening(object? sender, object e)
        {
            // Only devices typed in by address can be removed; discovered ones come
            // back on the next scan.
            if (sender is not MenuFlyout flyout) return;
            var item = flyout.Items.Count > 0 ? flyout.Items[0] as MenuFlyoutItem : null;
            var device = (flyout.Target as FrameworkElement)?.DataContext as DeviceItem;
            if (item != null) item.IsEnabled = device != null && ViewModel.IsManualDevice(device);
        }

        private void OnRemoveDeviceClicked(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.DataContext is DeviceItem device)
            {
                ViewModel.RemoveManualDevice(device);
            }
        }

        /// <summary>True when launched at sign-in with the tray on: the window is never shown.</summary>
        public bool StartHidden { get; }

        public MainWindow(bool startInBackground = false)
        {
            this.InitializeComponent();
            UseCustomTitleBar();
            UiTranslator.Apply(this);
            StartHidden = startInBackground && ViewModel.Settings.EnableTrayOnStartup;
            Title = "CastMirror";
            // A logical (DIP) size. AppWindow.Resize takes physical pixels, so
            // passing those numbers raw would give a window that shrinks as the
            // display scale rises - 640x400 DIPs at 200%, with the three-column
            // shell crushed. WindowScaler converts to the monitor's scale and
            // clamps to its work area.
            // The engine's default size (920x700) means "never saved" here.
            CastMirrorSettings saved = ViewModel.Settings;
            bool hasSavedSize = saved.WindowWidth >= MinWidthDip && saved.WindowHeight >= MinHeightDip &&
                                !(saved.WindowWidth == 920 && saved.WindowHeight == 700);
            WindowScaler.ResizeToDips(AppWindow,
                hasSavedSize ? saved.WindowWidth : DefaultWidthDip,
                hasSavedSize ? saved.WindowHeight : DefaultHeightDip);
            WindowScaler.EnforceMinimumSize(AppWindow, MinWidthDip, MinHeightDip);

            ThemeService.Register(Content as FrameworkElement);
            ThemeService.Apply(ViewModel.Settings.UiTheme);

            ViewModel.StateChanged += OnSessionStateChanged;
            ViewModel.PropertyChanged += OnViewModelPropertyChanged;
            NotificationService.Activated += OnNotificationActivated;
            SyncTrayIcon();
            AddKeyboardShortcuts();
            UpdateFirstRunFocusTrap();

            string? previousCrash = CrashHandler.TakePreviousCrash();
            if (previousCrash != null) ViewModel.ReportPreviousCrash(previousCrash);

            // A second launch asks this instance to show itself instead of doing
            // nothing, which matters most when the window is hidden in the tray.
            SingleInstance.ListenForActivation(() => DispatcherQueue.TryEnqueue(RestoreWindow));

            _hotkeys = new HotkeyService(WindowScaler.HandleOf(AppWindow),
                ViewModel.HotkeyToggleCast, ViewModel.HotkeyToggleFreeze, ViewModel.HotkeyToggleMute);
            ApplyHotkeySetting();

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
                Title = Localizer.T("Add a device by IP address"),
                Content = new StackPanel
                {
                    Spacing = 8,
                    Children =
                    {
                        new TextBlock
                        {
                            Text = Localizer.T("Type the IPv4 address of a TV or speaker that doesn't appear in the list. You can find it in the device's network settings or your router's client list."),
                            TextWrapping = TextWrapping.Wrap
                        },
                        input
                    }
                },
                PrimaryButtonText = Localizer.T("Add"),
                CloseButtonText = Localizer.T("Cancel"),
                DefaultButton = ContentDialogButton.Primary
            };

            ContentDialogResult result = await Dialogs.ShowAsync(dialog, Content.XamlRoot);
            if (result != ContentDialogResult.Primary) return;

            string address = input.Text.Trim();
            if (address.Length == 0)
            {
                ViewModel.ErrorMessage = Localizer.T("Type an IP address to add a device.");
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
                _settingsWindow = new SettingsWindow(ViewModel, SyncTrayIcon, ApplyHotkeySetting);
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

        private void OnNavClick(object sender, RoutedEventArgs e)
        {
            if (sender is not FrameworkElement nav) return;

            if (nav.Tag is not string tag) return;
            ViewModel.Section = tag switch
            {
                "Home" => MainViewModel.ShellSection.Home,
                "Devices" => MainViewModel.ShellSection.Devices,
                "Cast" => MainViewModel.ShellSection.Cast,
                "Settings" => MainViewModel.ShellSection.Settings,
                "Help" => MainViewModel.ShellSection.Help,
                _ => ViewModel.Section
            };

            // Settings lives in its own window; the rail entry explains where it
            // went rather than opening it behind the user's back.
        }

        private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
        {
            if (sender is not TextBox box) return;
            // TextChanged also fires when the view model pushes text back into the
            // box. Assigning only on a real difference keeps that from looping;
            // clearing the box still empties the filter because the values differ.
            if (!string.Equals(box.Text, ViewModel.SearchText, StringComparison.Ordinal))
            {
                ViewModel.SearchText = box.Text;
            }
        }

        private void OnSearchKeyDown(object sender, KeyRoutedEventArgs e)
        {
            // Enter commits the filter. There is no suggestion list to pick from,
            // so this replaces the AutoSuggestBox query the field used to raise.
            if (e.Key != VirtualKey.Enter || sender is not TextBox box) return;
            ViewModel.SearchText = box.Text;
        }

        /// <summary>Focuses the search field (Ctrl+K) and clears any selection.</summary>
        private void FocusSearch()
        {
            SearchBox.Focus(FocusState.Programmatic);
        }

        private void OnDeviceCardClicked(object sender, ItemClickEventArgs e)
        {
            if (e.ClickedItem is DeviceItem device) ViewModel.SelectedDevice = device;
        }

        private void OnCastToDeviceClicked(object sender, RoutedEventArgs e)
        {
            // The button lives in the card, so the clicked item is the button's
            // DataContext rather than the sender.
            if ((sender as FrameworkElement)?.DataContext is not DeviceItem device) return;
            ViewModel.SelectedDevice = device;
            ViewModel.ToggleCast();
        }

        private void OnCastModeChecked(object sender, RoutedEventArgs e)
        {
            if (sender is not RadioButton { Tag: string tag }) return;
            var mode = tag == "Window" ? MainViewModel.CastMode.Window : MainViewModel.CastMode.Screen;
            // IsChecked is one-way, so a click never reaches the view model on its
            // own. The same event also echoes the view model's own change back;
            // that case already matches and is skipped.
            if (mode == ViewModel.SelectedCastMode) return;
            // Windows opened since launch are not in the list yet.
            if (mode == MainViewModel.CastMode.Window) ViewModel.RefreshSources();
            ViewModel.SelectedCastMode = mode;
        }

        private void OnWindowPicked(object sender, SelectionChangedEventArgs e)
        {
            // Ignore the empty selection a list rebuild raises; it is not a choice.
            if (e.AddedItems.Count > 0 && e.AddedItems[0] is SourceItem source)
            {
                ViewModel.SelectedSource = source;
            }
        }

        private async void OnFirstRunScanClicked(object sender, RoutedEventArgs e)
        {
            // The scan needs the same consent as in Settings: it probes every
            // address on the subnet, which some networks treat as an attack.
            if (await Dialogs.ConfirmSubnetScanAsync(Content.XamlRoot))
            {
                ViewModel.CompleteFirstRun(enableSubnetScan: true);
            }
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
                // Stop only. The menu item can be clicked just as the session ends,
                // and a toggle would then start a new cast.
                () => { if (ViewModel.IsSessionActive) ViewModel.ToggleCast(); },
                ExitFromTray,
                () => ViewModel.IsSessionActive);
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
                        Localizer.T("The tray icon could not be created. Start CastMirror again to bring the window back."),
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
            RestoreWindow();
            if (ViewModel.IsSessionActive)
            {
                // Quitting drops the cast, so ask first, exactly as the close
                // button does. Setting _exiting before this used to skip the check.
                ConfirmExitAndCloseAsync();
                return;
            }
            // The user asked to quit, so the close handler must not turn this
            // back into a hide (CloseToTray would otherwise cancel this close and
            // leave the app unquittable from the tray).
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
                ? Localizer.Format("CastMirror - casting to {0}", ViewModel.SelectedDevice?.Name ?? Localizer.T("TV"))
                : "CastMirror");
        }

        /// <summary>Remembers the window size (in DIPs) unless it is minimised or maximised.</summary>
        private void SaveWindowSize()
        {
            try
            {
                if (AppWindow.Presenter is OverlappedPresenter presenter &&
                    presenter.State != OverlappedPresenterState.Restored)
                {
                    return;
                }
                double scale = WindowScaler.ScaleOf(AppWindow);
                ViewModel.SaveWindowSize(
                    (int)Math.Round(AppWindow.Size.Width / scale),
                    (int)Math.Round(AppWindow.Size.Height / scale));
            }
            catch (Exception ex)
            {
                MainViewModel.LogError(ex);
            }
        }

        private void OnWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
        {
            SaveWindowSize();
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
                Title = Localizer.T("Stop casting and quit?"),
                Content = Localizer.Format("You're casting to {0}. Quitting ends the cast.", ViewModel.SelectedDevice?.Name ?? Localizer.T("a TV")),
                PrimaryButtonText = Localizer.T("Quit"),
                CloseButtonText = Localizer.T("Keep casting"),
                DefaultButton = ContentDialogButton.Close
            };

            ContentDialogResult result = await Dialogs.ShowAsync(dialog, Content.XamlRoot);
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
            _hotkeys?.Dispose();
            _hotkeys = null;
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
