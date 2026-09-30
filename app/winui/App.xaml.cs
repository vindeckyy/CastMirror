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
            Services.CrashHandler.Install();
            UnhandledException += OnUnhandledException;
            // Faults outside XAML (worker threads, un-awaited tasks) used to leave
            // no trace at all.
            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
                Services.LogService.Log($"[fatal] {e.ExceptionObject}");
            System.Threading.Tasks.TaskScheduler.UnobservedTaskException += (_, e) =>
            {
                Services.LogService.Log($"[unobserved task] {e.Exception}");
                e.SetObserved();
            };
            // A close-to-tray window keeps the process alive, so the OS will not
            // always run ProcessExit. The mutex is a named kernel object and is
            // released by the kernel when the process dies regardless, but
            // releasing it here keeps a clean exit from looking like a crash to
            // the next launch.
            AppDomain.CurrentDomain.ProcessExit += (_, _) => Services.SingleInstance.Release();
        }

        protected override void OnLaunched(LaunchActivatedEventArgs args)
        {
            // One instance per session. The native engine holds its config,
            // discovery listeners, and cast session in process-wide state, so a
            // second copy would compete for the same LAN. This launch asks the
            // running instance to show its window and ends.
            //
            // This check comes first: the notification platform below registers
            // a process-wide COM activator, which a launch that is about to exit
            // must not take from the running instance.
            if (!Services.SingleInstance.TryAcquire())
            {
                Services.LogService.Log("[single-instance] another instance is already running; waking it");
                Services.SingleInstance.SignalRunningInstance();
                Exit();
                return;
            }

            // Register before any window exists: unpackaged apps need this to
            // deliver toasts, and the settings window asks IsSupported.
            Services.NotificationService.Initialize();

            // Started from the Run key at sign-in: stay in the tray instead of
            // throwing a window over whatever the user opened first.
            bool background = Array.Exists(Environment.GetCommandLineArgs(),
                arg => string.Equals(arg, Services.AutostartService.BackgroundFlag, StringComparison.OrdinalIgnoreCase));
            var main = new MainWindow(background);
            _window = main;
            if (!main.StartHidden)
            {
                main.Activate();
            }
        }

        private static void OnUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            // LogService owns the shared %APPDATA%\CastMirror\gui-errors.log and
            // caps its growth; the previous hand-rolled append could grow the
            // file without bound inside an exception loop.
            Services.LogService.Log($"[unhandled] {e.Message}{Environment.NewLine}{e.Exception}");
            // Keep the app alive; most XAML binding/layout faults are
            // recoverable and a hard crash loses the user's session. The user
            // is told something went wrong, so a swallowed fault is never silent.
            e.Handled = true;
            if ((Current as App)?._window is MainWindow main)
            {
                main.DispatcherQueue.TryEnqueue(main.ViewModel.ReportUnexpectedError);
            }
        }
    }
}
