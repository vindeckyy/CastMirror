using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace CastMirror.Services
{
    /// <summary>
    /// Applies the configured color scheme to every registered window root.
    /// "system" (or anything unknown) follows the OS.
    /// </summary>
    public static class ThemeService
    {
        private static readonly List<WeakReference<FrameworkElement>> Roots = new();
        private static string _theme = string.Empty;

        public static string Current => _theme;

        /// <summary>Registers a window root so it tracks later theme changes.</summary>
        public static void Register(FrameworkElement? root)
        {
            if (root == null) return;
            Roots.Add(new WeakReference<FrameworkElement>(root));
            ApplyTo(root);
        }

        /// <summary>Sets the scheme: "light", "dark", or empty/system.</summary>
        public static void Apply(string theme)
        {
            _theme = theme ?? string.Empty;
            Roots.RemoveAll(reference => !reference.TryGetTarget(out _));
            foreach (var reference in Roots)
            {
                if (reference.TryGetTarget(out var root)) ApplyTo(root);
            }
        }

        private static void ApplyTo(FrameworkElement root)
        {
            try
            {
                root.RequestedTheme = _theme switch
                {
                    "light" => ElementTheme.Light,
                    "dark" => ElementTheme.Dark,
                    _ => ElementTheme.Default
                };
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
        }
    }
}
