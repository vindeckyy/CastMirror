using System;
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
            // A close-to-tray window keeps the process alive, so the OS will not
            // always run ProcessExit. The mutex is a named kernel object and is
            // released by the kernel when the process dies regardless, but
            // releasing it here keeps a clean exit from looking like a crash to
            // the next launch.
            AppDomain.CurrentDomain.ProcessExit += (_, _) => Services.SingleInstance.Release();
        }

        protected override void OnLaunched(LaunchActivatedEventArgs args)
        {
            // Register before any window exists: unpackaged apps need this to
            // deliver toasts, and the settings window asks IsSupported.
            Services.NotificationService.Initialize();

            // One instance per session. The native engine holds its config,
            // discovery listeners, and cast session in process-wide state, so a
            // second copy would compete for the same LAN. The already-running
            // window is reachable from its tray icon, so this launch just ends.
            if (!Services.SingleInstance.TryAcquire())
            {
                Services.LogService.Log("[single-instance] another instance is already running; exiting");
                Exit();
                return;
            }

            _window = new MainWindow();
            _window.Activate();
        }

        private static void OnUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            // LogService owns the shared %APPDATA%\CastMirror\gui-errors.log and
            // caps its growth; the previous hand-rolled append could grow the
            // file without bound inside an exception loop.
            Services.LogService.Log($"[unhandled] {e.Message}{Environment.NewLine}{e.Exception}");
            // Keep the app alive; most XAML binding/layout faults are
            // recoverable and a hard crash loses the user's session.
            e.Handled = true;
        }
    }
}
