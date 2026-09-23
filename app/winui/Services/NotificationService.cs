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

        public static void Initialize()
        {
            try
            {
                AppNotificationManager.Default.Register();
                IsSupported = true;
            }
            catch (Exception ex)
            {
                IsSupported = false;
                ViewModels.MainViewModel.LogError(ex);
            }
        }

        public static void Shutdown()
        {
            if (!IsSupported) return;
            try
            {
                AppNotificationManager.Default.Unregister();
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
        }

        /// <summary>Shows a toast when the user has notifications enabled.</summary>
        public static void Notify(bool enabled, string title, string body)
        {
            if (!enabled || !IsSupported) return;
            try
            {
                var notification = new AppNotificationBuilder()
                    .AddText(title)
                    .AddText(body)
                    .BuildNotification();
                AppNotificationManager.Default.Show(notification);
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
        }
    }
}
