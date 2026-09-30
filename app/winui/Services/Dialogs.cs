using System;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CastMirror.Services
{
    /// <summary>
    /// One place to show a <see cref="ContentDialog"/>. It fixes two things the
    /// raw control gets wrong: a dialog does not inherit the theme the user forced
    /// in Settings (it follows the OS instead), and opening a second dialog while
    /// one is up throws.
    /// </summary>
    internal static class Dialogs
    {
        private static bool _open;

        public static async Task<ContentDialogResult> ShowAsync(ContentDialog dialog, XamlRoot? root)
        {
            if (_open || root == null) return ContentDialogResult.None;
            _open = true;
            try
            {
                dialog.XamlRoot = root;
                dialog.RequestedTheme = ThemeService.RequestedTheme;
                return await dialog.ShowAsync();
            }
            finally
            {
                _open = false;
            }
        }

        /// <summary>
        /// Asks before turning on the subnet scan. Both places that can enable it
        /// (first-run card and Settings) go through here so neither skips the
        /// warning.
        /// </summary>
        public static async Task<bool> ConfirmSubnetScanAsync(XamlRoot? root)
        {
            var dialog = new ContentDialog
            {
                Title = Localizer.T("Scan your network for TVs?"),
                Content = new TextBlock
                {
                    Text = Localizer.T("CastMirror will open a short connection to every address on your local " +
                           "subnet, on port 8009, to find TVs that don't announce themselves.\n\n" +
                           "Leave this off at work, at school and on guest Wi-Fi. Network monitors " +
                           "can flag it as a port scan."),
                    TextWrapping = TextWrapping.Wrap
                },
                PrimaryButtonText = Localizer.T("Scan"),
                CloseButtonText = Localizer.T("Cancel"),
                DefaultButton = ContentDialogButton.Close
            };
            return await ShowAsync(dialog, root) == ContentDialogResult.Primary;
        }
    }
}
