using System;
using System.Diagnostics;
using System.IO;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;
using CastMirror.Services;
using CastMirror.ViewModels;

namespace CastMirror
{
    /// <summary>
    /// Live, searchable diagnostic log surface backed by the engine's log
    /// callback. Level filter, text filter, copy-to-clipboard and clear, plus the
    /// open-log-directory action the settings page also offers.
    /// </summary>
    public sealed partial class LogsWindow : Window
    {
        public LogsViewModel ViewModel { get; } = new();

        public LogsWindow()
        {
            InitializeComponent();
            // Logical (DIP) size, converted for the monitor this window opens on.
            WindowScaler.ResizeToDips(AppWindow, 900, 600);

            ThemeService.Register(Content as FrameworkElement);
            LevelCombo.SelectedIndex = 1; // Info
            ViewModel.Attach();
            Closed += (_, _) => ViewModel.Detach();
        }

        private void OnLevelChanged(object sender, SelectionChangedEventArgs e)
        {
            if (ViewModel == null) return;
            ViewModel.MinLevel = Math.Max(0, LevelCombo.SelectedIndex);
        }

        private void OnFilterChanged(object sender, TextChangedEventArgs e)
        {
            if (ViewModel == null) return;
            ViewModel.Filter = FilterBox.Text;
        }

        private void OnCopyClicked(object sender, RoutedEventArgs e)
        {
            try
            {
                var package = new DataPackage();
                package.SetText(ViewModel.CopyText());
                Clipboard.SetContent(package);
            }
            catch (Exception ex)
            {
                MainViewModel.LogError(ex);
            }
        }

        private void OnClearClicked(object sender, RoutedEventArgs e)
        {
            ViewModel.Clear();
        }

        private void OnOpenFolderClicked(object sender, RoutedEventArgs e)
        {
            try
            {
                Directory.CreateDirectory(LogService.DirectoryPath);
                Process.Start(new ProcessStartInfo("explorer.exe", LogService.DirectoryPath) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                MainViewModel.LogError(ex);
            }
        }
    }
}
