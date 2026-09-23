using System;
using System.IO;
using Microsoft.UI.Xaml;

namespace CastMirror
{
    public partial class App : Application
    {
        private Window? _window;

        public App()
        {
            this.InitializeComponent();
            UnhandledException += OnUnhandledException;
        }

        protected override void OnLaunched(LaunchActivatedEventArgs args)
        {
            // Register before any window exists: unpackaged apps need this to
            // deliver toasts, and the settings window asks IsSupported.
            Services.NotificationService.Initialize();
            _window = new MainWindow();
            _window.Activate();
        }

        private static void OnUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            try
            {
                string dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "CastMirror");
                Directory.CreateDirectory(dir);
                File.AppendAllText(
                    Path.Combine(dir, "gui-errors.log"),
                    $"{DateTime.Now:O} [unhandled] {e.Message}{Environment.NewLine}{e.Exception}{Environment.NewLine}{Environment.NewLine}");
            }
            catch
            {
                // Diagnostics must never mask the original failure.
            }

            // Keep the app alive; most XAML binding/layout faults are
            // recoverable and a hard crash loses the user's session.
            e.Handled = true;
        }
    }
}
