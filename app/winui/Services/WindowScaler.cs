using System;
using System.Runtime.InteropServices;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Windows.Graphics;

namespace CastMirror.Services
{
    /// <summary>
    /// Bridges the two coordinate spaces a WinUI window lives in.
    ///
    /// XAML lays out and expresses sizes in DIPs (device-independent pixels,
    /// 96 = 100%), while <see cref="AppWindow"/> and the window manager work in
    /// physical screen pixels. The app is PerMonitorV2 (app.manifest), so the
    /// factor is per monitor and changes when the window is dragged onto a
    /// display with a different scale.
    ///
    /// Sizing a window with a raw pixel count therefore produces a window that
    /// shrinks as the display scale rises: 1100 physical pixels is a 1100-DIP
    /// window at 100%, but only a 550x360-DIP window at 200% - cramped, and
    /// with the content scrolled into unusableness. Every window that wants a
    /// given logical size goes through <see cref="ResizeToDips"/>.
    /// </summary>
    internal static class WindowScaler
    {
        /// <summary>The DPI that corresponds to 100% scaling.</summary>
        public const double BaseDpi = 96.0;

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint GetDpiForWindow(IntPtr hwnd);

        private delegate IntPtr SubclassProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam,
                                             UIntPtr id, UIntPtr data);

        [DllImport("comctl32.dll")]
        private static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id, UIntPtr data);

        [DllImport("comctl32.dll")]
        private static extern IntPtr DefSubclassProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential)]
        private struct Point { public int X; public int Y; }

        [StructLayout(LayoutKind.Sequential)]
        private struct MinMaxInfo
        {
            public Point Reserved;
            public Point MaxSize;
            public Point MaxPosition;
            public Point MinTrackSize;
            public Point MaxTrackSize;
        }

        private const uint WmGetMinMaxInfo = 0x0024;

        // The subclass delegates must outlive the windows that use them.
        private static readonly System.Collections.Generic.List<SubclassProc> Subclasses = new();

        /// <summary>
        /// Stops the user from dragging the window below a usable size. The size is
        /// logical (DIPs) and is re-scaled every time, so it stays right when the
        /// window moves to a monitor with a different scale.
        /// </summary>
        public static void EnforceMinimumSize(AppWindow? window, int minWidthDip, int minHeightDip)
        {
            IntPtr hwnd = HandleOf(window);
            if (hwnd == IntPtr.Zero) return;
            SubclassProc proc = (h, msg, wParam, lParam, id, data) =>
            {
                if (msg == WmGetMinMaxInfo)
                {
                    double scale = ScaleOf(window);
                    var info = Marshal.PtrToStructure<MinMaxInfo>(lParam);
                    info.MinTrackSize.X = (int)Math.Round(minWidthDip * scale);
                    info.MinTrackSize.Y = (int)Math.Round(minHeightDip * scale);
                    Marshal.StructureToPtr(info, lParam, false);
                    return IntPtr.Zero;
                }
                return DefSubclassProc(h, msg, wParam, lParam);
            };
            Subclasses.Add(proc);
            if (!SetWindowSubclass(hwnd, proc, UIntPtr.Zero, UIntPtr.Zero))
            {
                Subclasses.Remove(proc);
                LogService.Log("[dpi] could not install the minimum-size hook");
            }
        }

        /// <summary>
        /// The HWND for a window, or <see cref="IntPtr.Zero"/> when it cannot be
        /// resolved. Requires Windows 10 1607+; the project targets 19041+.
        /// </summary>
        public static IntPtr HandleOf(AppWindow? window)
        {
            if (window is null) return IntPtr.Zero;
            try
            {
                return Win32Interop.GetWindowFromWindowId(window.Id);
            }
            catch (Exception ex)
            {
                LogService.Log($"[dpi] window handle unavailable: {ex.Message}");
                return IntPtr.Zero;
            }
        }

        /// <summary>
        /// Scale factor of the monitor the window currently sits on: 1.0 at
        /// 100%, 1.5 at 150%, 2.0 at 200%. Falls back to 1.0 rather than
        /// collapsing the window when the handle or DPI cannot be read.
        /// </summary>
        public static double ScaleOf(AppWindow? window)
        {
            IntPtr hwnd = HandleOf(window);
            if (hwnd == IntPtr.Zero) return 1.0;
            try
            {
                // 0 means an invalid HWND, and anything below 96 is not a
                // meaningful scale, so both fall back to 100%.
                uint dpi = GetDpiForWindow(hwnd);
                return dpi < (uint)BaseDpi ? 1.0 : dpi / BaseDpi;
            }
            catch (Exception ex)
            {
                LogService.Log($"[dpi] GetDpiForWindow failed: {ex.Message}");
                return 1.0;
            }
        }

        /// <summary>
        /// Converts a logical (DIP) size to the physical pixels this window's
        /// monitor expects, without applying the work-area clamp.
        /// </summary>
        public static SizeInt32 ToPhysical(AppWindow? window, int widthDip, int heightDip)
        {
            double scale = ScaleOf(window);
            return new SizeInt32(
                Math.Max(1, (int)Math.Round(widthDip * scale)),
                Math.Max(1, (int)Math.Round(heightDip * scale)));
        }

        /// <summary>
        /// Sizes a window to a logical (DIP) size, scaled for the monitor it is
        /// on and clamped to that monitor's work area.
        ///
        /// The clamp matters on small or high-scaled displays: a 700x780-DIP
        /// settings window needs 1400x1560 physical pixels at 200%, which does
        /// not exist on a 1366x768 laptop, and without clamping the window would
        /// extend past the screen edge. When the clamp actually bites, the
        /// window is centred so the part that fits stays visible.
        /// </summary>
        public static void ResizeToDips(AppWindow? window, int widthDip, int heightDip)
        {
            if (window is null) return;
            try
            {
                double scale = ScaleOf(window);
                int width = Math.Max(1, (int)Math.Round(widthDip * scale));
                int height = Math.Max(1, (int)Math.Round(heightDip * scale));

                RectInt32 work = WorkAreaOf(window);
                bool clamped = false;
                if (work.Width > 0 && work.Height > 0)
                {
                    int fitWidth = Math.Min(width, work.Width);
                    int fitHeight = Math.Min(height, work.Height);
                    clamped = fitWidth != width || fitHeight != height;
                    width = Math.Max(1, fitWidth);
                    height = Math.Max(1, fitHeight);
                }

                window.Resize(new SizeInt32(width, height));

                if (clamped && work.Width > 0 && work.Height > 0)
                {
                    window.Move(new PointInt32(
                        work.X + (work.Width - width) / 2,
                        work.Y + (work.Height - height) / 2));
                }
            }
            catch (Exception ex)
            {
                // A window that keeps its default size still works.
                LogService.Log($"[dpi] resize to {widthDip}x{heightDip} DIP failed: {ex.Message}");
            }
        }

        /// <summary>
        /// Work area of the display nearest the window, in physical pixels.
        /// Returns an empty rect when it cannot be read, which callers treat as
        /// "no clamp available".
        /// </summary>
        private static RectInt32 WorkAreaOf(AppWindow window)
        {
            try
            {
                return DisplayArea.GetFromWindowId(window.Id, DisplayAreaFallback.Nearest).WorkArea;
            }
            catch (Exception ex)
            {
                LogService.Log($"[dpi] work area unavailable: {ex.Message}");
                return default;
            }
        }
    }
}