using System;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.UI.Dispatching;

namespace CastMirror.Services
{
    /// <summary>
    /// System tray icon built directly on Shell_NotifyIconW, so the app stays
    /// dependency-free and works unpackaged.
    ///
    /// The icon lives on its own STA thread with a hidden top-level window:
    /// a message-only window cannot become foreground, which TrackPopupMenu
    /// needs for the context menu to behave.
    /// </summary>
    public sealed class TrayIconService : IDisposable
    {
        private const int WM_APP = 0x8000;
        private const int WM_TRAYICON = WM_APP + 1;
        private const int WM_CLOSE = 0x0010;
        private const int WM_DESTROY = 0x0002;
        private const int WM_LBUTTONUP = 0x0202;
        private const int WM_RBUTTONUP = 0x0205;

        private const uint NIM_ADD = 0x00000000;
        private const uint NIM_MODIFY = 0x00000001;
        private const uint NIM_DELETE = 0x00000002;
        private const uint NIF_MESSAGE = 0x00000001;
        private const uint NIF_ICON = 0x00000002;
        private const uint NIF_TIP = 0x00000004;

        private const uint MF_STRING = 0x00000000;
        private const uint MF_GRAYED = 0x00000001;
        private const uint MF_SEPARATOR = 0x00000800;
        private const uint TPM_RETURNCMD = 0x0100;
        private const uint TPM_RIGHTBUTTON = 0x0002;

        private const int CMD_OPEN = 1;
        private const int CMD_STOP = 2;
        private const int CMD_EXIT = 3;
        private const int IDI_APPLICATION = 32512;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NOTIFYICONDATA
        {
            public int cbSize;
            public IntPtr hWnd;
            public uint uID;
            public uint uFlags;
            public uint uCallbackMessage;
            public IntPtr hIcon;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
            public uint dwState;
            public uint dwStateMask;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
            public uint uVersion;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
            public uint dwInfoFlags;
            public Guid guidItem;
            public IntPtr hBalloonIcon;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct WNDCLASSEX
        {
            public int cbSize;
            public uint style;
            public IntPtr lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            [MarshalAs(UnmanagedType.LPWStr)] public string lpszMenuName;
            [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
            public IntPtr hIconSm;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int X;
            public int Y;
        }

        private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern ushort RegisterClassExW(ref WNDCLASSEX lpwcx);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateWindowExW(uint dwExStyle, string lpClassName, string lpWindowName,
            uint dwStyle, int x, int y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu,
            IntPtr hInstance, IntPtr lpParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr DefWindowProcW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern void PostQuitMessage(int nExitCode);

        [DllImport("user32.dll")]
        private static extern int GetMessageW(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

        [DllImport("user32.dll")]
        private static extern bool TranslateMessage(ref MSG lpMsg);

        [DllImport("user32.dll")]
        private static extern IntPtr DispatchMessageW(ref MSG lpMsg);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern uint RegisterWindowMessageW(string lpString);

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern IntPtr CreatePopupMenu();

        [DllImport("user32.dll")]
        private static extern bool DestroyMenu(IntPtr hMenu);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool AppendMenuW(IntPtr hMenu, uint uFlags, IntPtr uIDNewItem, string lpNewItem);

        [DllImport("user32.dll")]
        private static extern int TrackPopupMenu(IntPtr hMenu, uint uFlags, int x, int y, int nReserved,
            IntPtr hWnd, IntPtr prcRect);

        [DllImport("user32.dll")]
        private static extern bool GetCursorPos(out POINT lpPoint);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr LoadIconW(IntPtr hInstance, IntPtr lpIconName);

        [DllImport("user32.dll")]
        private static extern bool DestroyIcon(IntPtr hIcon);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern bool Shell_NotifyIconW(uint dwMessage, ref NOTIFYICONDATA lpData);

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        private static extern uint ExtractIconExW(string lpszFile, int nIconIndex, IntPtr[] phiconLarge, IntPtr[] phiconSmall, uint nIcons);

        [StructLayout(LayoutKind.Sequential)]
        private struct MSG
        {
            public IntPtr hwnd;
            public uint message;
            public IntPtr wParam;
            public IntPtr lParam;
            public uint time;
            public POINT pt;
        }

        private readonly DispatcherQueue? _dispatcher;
        private readonly Action _onOpen;
        private readonly Action _onStopCast;
        private readonly Action _onExit;
        private readonly Func<bool> _isStreaming;

        private readonly object _startLock = new();
        private Thread? _thread;
        private IntPtr _hwnd = IntPtr.Zero;
        private IntPtr _icon = IntPtr.Zero;
        private readonly ManualResetEventSlim _ready = new(false);
        private volatile bool _iconAdded;
        private volatile bool _disposed;
        private bool _ownsIcon;

        private string _tooltip = "CastMirror";
        private WndProcDelegate? _wndProc;  // kept alive for the window's lifetime

        public bool IsActive => _iconAdded;

        public TrayIconService(DispatcherQueue? dispatcher, Action onOpen, Action onStopCast, Action onExit,
            Func<bool> isStreaming)
        {
            _dispatcher = dispatcher;
            _onOpen = onOpen;
            _onStopCast = onStopCast;
            _onExit = onExit;
            _isStreaming = isStreaming;
        }

        /// <summary>Creates the icon. Returns false when the shell rejected it.</summary>
        /// <remarks>
        /// Safe to call from any thread and more than once: the UI thread starts
        /// the tray on a worker so the shell wait never stalls the window, and
        /// the close-to-tray path may reach it again while that is in flight.
        /// A second call joins the first rather than starting a second thread,
        /// which would race RegisterClassExW and leave a duplicate icon.
        /// </remarks>
        public bool Start()
        {
            lock (_startLock)
            {
                if (_disposed) return false;
                if (_thread != null)
                {
                    // Already starting or started: wait on the same signal the
                    // original Start() waits on, so the caller still gets a
                    // truthful answer.
                    _ready.Wait(TimeSpan.FromSeconds(5));
                    return _iconAdded;
                }
                _thread = new Thread(MessageLoop)
                {
                    IsBackground = true,
                    Name = "CastMirrorTray"
                };
                _thread.SetApartmentState(ApartmentState.STA);
                _thread.Start();
            }
            // Waited outside the lock: MessageLoop never takes it, and holding
            // it for the whole wait would block a concurrent Start() that is
            // only trying to observe the outcome. A teardown between the two
            // sections disposes _ready, so the flag is rechecked before waiting
            // on it — a disposed ManualResetEventSlim would throw.
            if (_disposed) return false;
            _ready.Wait(TimeSpan.FromSeconds(5));
            return _iconAdded;
        }

        public void SetTooltip(string tooltip)
        {
            _tooltip = string.IsNullOrEmpty(tooltip) ? "CastMirror" : tooltip;
            if (_hwnd == IntPtr.Zero) return;
            PostMessageW(_hwnd, WM_APP + 2, IntPtr.Zero, IntPtr.Zero);
        }

        private void MessageLoop()
        {
            try
            {
                _wndProc = WindowProc;
                var wc = new WNDCLASSEX
                {
                    cbSize = Marshal.SizeOf<WNDCLASSEX>(),
                    lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProc),
                    hInstance = GetModuleHandleW(null),
                    lpszClassName = "CastMirrorTrayWindow"
                };
                RegisterClassExW(ref wc);

                // WS_EX_TOOLWINDOW + never shown: no taskbar button, but still a
                // real top-level window so the context menu behaves.
                _hwnd = CreateWindowExW(0x00000080, wc.lpszClassName, "CastMirror", 0x80000000,
                    0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
                if (_hwnd == IntPtr.Zero)
                {
                    _ready.Set();
                    return;
                }

                _icon = LoadAppIcon();
                AddIcon();

                while (GetMessageW(out MSG msg, IntPtr.Zero, 0, 0) > 0)
                {
                    TranslateMessage(ref msg);
                    DispatchMessageW(ref msg);
                }
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
            finally
            {
                _ready.Set();
                RemoveIcon();
            }
        }

        private IntPtr LoadAppIcon()
        {
            try
            {
                string? exe = Environment.ProcessPath;
                if (!string.IsNullOrEmpty(exe))
                {
                    var large = new IntPtr[1];
                    var small = new IntPtr[1];
                    if (ExtractIconExW(exe, 0, large, small, 1) > 0)
                    {
                        // The tray slot is 16x16 at 100% scale. The large (32x32)
                        // icon gets squeezed into it and looks smudged, so prefer
                        // the small one.
                        IntPtr handle = small[0] != IntPtr.Zero ? small[0] : large[0];
                        // ExtractIconExW fills both arrays when it is asked for
                        // both, and nothing else owns either handle: Dispose
                        // destroys only the one selected here, so the other
                        // would leak for the lifetime of the process.
                        IntPtr unused = small[0] != IntPtr.Zero ? large[0] : small[0];
                        if (unused != IntPtr.Zero) DestroyIcon(unused);
                        if (handle != IntPtr.Zero)
                        {
                            _ownsIcon = true;
                            return handle;
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
            return LoadIconW(IntPtr.Zero, (IntPtr)IDI_APPLICATION);
        }

        private void AddIcon()
        {
            if (_hwnd == IntPtr.Zero) return;
            var data = BuildIconData(_hwnd);
            _iconAdded = Shell_NotifyIconW(NIM_ADD, ref data);
            if (!_iconAdded)
            {
                LogService.Log("Shell_NotifyIconW(NIM_ADD) failed; tray icon unavailable");
            }
            _ready.Set();
        }

        private void RemoveIcon()
        {
            if (!_iconAdded) return;
            // Capture the handle once: Dispose owns _hwnd and clears it as soon
            // as this thread is gone. A torn or re-read field here used to be
            // how the NIM_DELETE was skipped, leaving a ghost icon in the tray.
            IntPtr hwnd = _hwnd;
            if (hwnd == IntPtr.Zero) return;
            var data = BuildIconData(hwnd);
            Shell_NotifyIconW(NIM_DELETE, ref data);
            _iconAdded = false;
        }

        private NOTIFYICONDATA BuildIconData(IntPtr hwnd)
        {
            return new NOTIFYICONDATA
            {
                cbSize = Marshal.SizeOf<NOTIFYICONDATA>(),
                hWnd = hwnd,
                uID = 1,
                uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
                uCallbackMessage = WM_TRAYICON,
                hIcon = _icon,
                szTip = _tooltip,
                szInfo = string.Empty,
                szInfoTitle = string.Empty
            };
        }

        private IntPtr WindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
        {
            if (msg == WM_TRAYICON)
            {
                int mouse = (int)(lParam.ToInt64() & 0xFFFF);
                if (mouse == WM_LBUTTONUP)
                {
                    Dispatch(_onOpen);
                }
                else if (mouse == WM_RBUTTONUP)
                {
                    ShowContextMenu();
                }
                return IntPtr.Zero;
            }
            if (msg == WM_APP + 2)
            {
                // Tooltip refresh requested from another thread.
                if (_iconAdded)
                {
                    var data = BuildIconData(hWnd);
                    Shell_NotifyIconW(NIM_MODIFY, ref data);
                }
                return IntPtr.Zero;
            }
            if (msg == _taskbarCreated && _taskbarCreated != 0)
            {
                // Explorer restarted: the shell forgot our icon.
                _iconAdded = false;
                AddIcon();
                return IntPtr.Zero;
            }
            if (msg == WM_CLOSE)
            {
                RemoveIcon();
                DestroyWindow(hWnd);
                return IntPtr.Zero;
            }
            if (msg == WM_DESTROY)
            {
                PostQuitMessage(0);
                return IntPtr.Zero;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        private static uint _taskbarCreated = RegisterWindowMessageW("TaskbarCreated");

        // uxtheme exports these two by ordinal only (135 SetPreferredAppMode,
        // 136 FlushMenuThemes). They make native popup menus follow the system
        // dark mode; without them the tray menu stays white on a dark taskbar.
        [DllImport("uxtheme.dll", EntryPoint = "#135")]
        private static extern int SetPreferredAppMode(int mode);

        [DllImport("uxtheme.dll", EntryPoint = "#136")]
        private static extern void FlushMenuThemes();

        private static bool _menuThemeApplied;

        private static void FollowSystemMenuTheme()
        {
            if (_menuThemeApplied) return;
            _menuThemeApplied = true;
            try
            {
                SetPreferredAppMode(1 /* AllowDark */);
                FlushMenuThemes();
            }
            catch (Exception ex) when (ex is EntryPointNotFoundException or DllNotFoundException)
            {
                // Older Windows builds lack the ordinals; the menu stays light.
            }
        }

        private void ShowContextMenu()
        {
            try
            {
                FollowSystemMenuTheme();
                IntPtr menu = CreatePopupMenu();
                if (menu == IntPtr.Zero) return;
                AppendMenuW(menu, MF_STRING, (IntPtr)CMD_OPEN, "Open CastMirror");
                AppendMenuW(menu, _isStreaming() ? MF_STRING : MF_STRING | MF_GRAYED, (IntPtr)CMD_STOP,
                    "Stop casting");
                AppendMenuW(menu, MF_SEPARATOR, IntPtr.Zero, string.Empty);
                AppendMenuW(menu, MF_STRING, (IntPtr)CMD_EXIT, "Exit");

                GetCursorPos(out POINT cursor);
                // Required so the menu closes when the user clicks elsewhere.
                SetForegroundWindow(_hwnd);
                int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.X, cursor.Y, 0,
                    _hwnd, IntPtr.Zero);
                PostMessageW(_hwnd, 0x0000, IntPtr.Zero, IntPtr.Zero);  // WM_NULL
                DestroyMenu(menu);

                switch (command)
                {
                    case CMD_OPEN: Dispatch(_onOpen); break;
                    case CMD_STOP: Dispatch(_onStopCast); break;
                    case CMD_EXIT: Dispatch(_onExit); break;
                }
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
        }

        private void Dispatch(Action action)
        {
            if (_dispatcher == null || _dispatcher.HasThreadAccess)
            {
                action();
                return;
            }
            _dispatcher.TryEnqueue(() =>
            {
                try
                {
                    action();
                }
                catch (Exception ex)
                {
                    ViewModels.MainViewModel.LogError(ex);
                }
            });
        }


        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern IntPtr GetModuleHandleW(string? lpModuleName);

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            bool threadStopped;
            try
            {
                // Read the thread handle under the lock Start() uses: a tray that
                // is still being created on a worker would otherwise race this
                // teardown, and we could null out _thread underneath a live
                // thread or skip joining it.
                Thread? thread;
                lock (_startLock)
                {
                    thread = _thread;
                }
                if (_hwnd != IntPtr.Zero)
                {
                    PostMessageW(_hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
                }
                if (thread?.Join(TimeSpan.FromSeconds(2)) == false && _hwnd != IntPtr.Zero)
                {
                    // The tray thread is most likely parked inside the modal
                    // TrackPopupMenu loop, which can swallow the first WM_CLOSE.
                    // Nudge it once more; the icon itself is always removed by
                    // the tray thread's own finally, so a failed join here only
                    // means the OS reclaims the handles at process exit.
                    PostMessageW(_hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
                    thread.Join(TimeSpan.FromMilliseconds(500));
                }
            }
            catch (Exception ex)
            {
                ViewModels.MainViewModel.LogError(ex);
            }
            finally
            {
                // Tear the shell state down only once the tray thread is gone:
                // its finally already ran RemoveIcon() with the live _hwnd, so
                // zeroing _hwnd/_icon now cannot skip the NIM_DELETE or pull
                // the HICON out from under a NIM_MODIFY still in flight.
                lock (_startLock)
                {
                    threadStopped = _thread == null || !_thread.IsAlive;
                    _thread = null;
                }
                if (threadStopped)
                {
                    _hwnd = IntPtr.Zero;
                    if (_ownsIcon && _icon != IntPtr.Zero)
                    {
                        DestroyIcon(_icon);
                        _icon = IntPtr.Zero;
                        _ownsIcon = false;
                    }
                    // Start() is never called again after _disposed, so nothing
                    // can be waiting on this signal by the time it is disposed.
                    _ready.Dispose();
                }
            }
        }
    }
}
