using System;
using Microsoft.Windows.AppNotifications;
using Microsoft.Windows.AppNotifications.Builder;

namespace CastMirror.Services
{
    /// <summary>
    /// Toast notifications for cast lifecycle events. Unpackaged apps can fail
    /// to register (no package identity on some builds), so every call is
    /// guarded and <see cref="IsSupported"/> tells the UI whether to offer the
    /// toggle.
    /// </summary>
    public static class NotificationService
    {
        public static bool IsSupported { get; private set; }

        /// <summary>
        /// Raised when the user clicks a toast. The argument is the raw
        /// activation argument string (e.g. "tag=cast-failed"), so callers can
        /// decide whether to surface the window.
        /// </summary>
        public static event Action<string?>? Activated;

        public static void Initialize()
        {
            try
            {
                // Without this the toast is shown but clicking it does nothing:
                // no window is brought forward and no action runs.
                AppNotificationManager.Default.NotificationInvoked += OnNotificationInvoked;
                AppNotificationManager.Default.Register();
                IsSupported = true;
            }
            catch (Exception ex)
            {
                IsSupported = false;
                LogService.Log(ex.ToString());
            }
        }

        public static void Shutdown()
        {
            if (!IsSupported) return;
            try
            {
                AppNotificationManager.Default.NotificationInvoked -= OnNotificationInvoked;
                AppNotificationManager.Default.Unregister();
            }
            catch (Exception ex)
            {
                LogService.Log(ex.ToString());
            }
        }

        /// <summary>Shows a toast when the user has notifications enabled.</summary>
        /// <param name="tag">Optional activation tag echoed back on click.</param>
        public static void Notify(bool enabled, string title, string body, string? tag = null)
        {
            if (!enabled || !IsSupported) return;
            try
            {
                var builder = new AppNotificationBuilder()
                    .AddText(title)
                    .AddText(body);
                if (!string.IsNullOrEmpty(tag))
                {
                    builder.AddArgument("tag", tag);
                }
                AppNotificationManager.Default.Show(builder.BuildNotification());
            }
            catch (Exception ex)
            {
                LogService.Log(ex.ToString());
            }
        }

        private static void OnNotificationInvoked(AppNotificationManager sender, AppNotificationActivatedEventArgs args)
        {
            try
            {
                Activated?.Invoke(args?.Argument);
            }
            catch (Exception ex)
            {
                LogService.Log(ex.ToString());
            }
        }
    }
}
