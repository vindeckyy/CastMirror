using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace CastMirror.Services
{
    /// <summary>
    /// System-wide shortcuts that work while CastMirror is hidden in the tray or
    /// another app has focus: start or stop the cast, freeze the picture, mute the
    /// TV. Off by default, because a global hotkey can collide with another app's.
    /// </summary>
    public sealed class HotkeyService : IDisposable
    {
        private const int WmHotkey = 0x0312;
        private const uint ModAlt = 0x0001;
        private const uint ModControl = 0x0002;
        private const uint ModNoRepeat = 0x4000;

        private delegate IntPtr SubclassProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam,
                                             UIntPtr id, UIntPtr data);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool RegisterHotKey(IntPtr hwnd, int id, uint modifiers, uint vk);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool UnregisterHotKey(IntPtr hwnd, int id);

        [DllImport("comctl32.dll")]
        private static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id, UIntPtr data);

        [DllImport("comctl32.dll")]
        private static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id);

        [DllImport("comctl32.dll")]
        private static extern IntPtr DefSubclassProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

        /// <summary>The bindings, for the Settings text. Ctrl+Alt+C, F and M.</summary>
        public const string Description = "Ctrl+Alt+C starts or stops the cast, Ctrl+Alt+F freezes the picture, Ctrl+Alt+M mutes the TV.";

        private static readonly UIntPtr SubclassId = (UIntPtr)0xCA57;

        private readonly IntPtr _hwnd;
        private readonly Dictionary<int, Action> _actions = new();
        private readonly List<int> _registered = new();
        // Must stay reachable for as long as the subclass is installed.
        private readonly SubclassProc _proc;
        private bool _hooked;

        public HotkeyService(IntPtr hwnd, Action toggleCast, Action toggleFreeze, Action toggleMute)
        {
            _hwnd = hwnd;
            _actions[1] = toggleCast;
            _actions[2] = toggleFreeze;
            _actions[3] = toggleMute;
            _proc = WindowProc;
        }

        /// <summary>
        /// Registers the shortcuts. Returns the names of any another app already owns;
        /// empty means all three are active.
        /// </summary>
        public IReadOnlyList<string> Enable()
        {
            var taken = new List<string>();
            if (_hwnd == IntPtr.Zero) return new[] { "all shortcuts (no window handle)" };
            if (!_hooked) _hooked = SetWindowSubclass(_hwnd, _proc, SubclassId, UIntPtr.Zero);
            if (!_hooked) return new[] { "all shortcuts (window hook failed)" };

            TryRegister(1, 'C', "Ctrl+Alt+C", taken);
            TryRegister(2, 'F', "Ctrl+Alt+F", taken);
            TryRegister(3, 'M', "Ctrl+Alt+M", taken);
            return taken;
        }

        private void TryRegister(int id, char key, string label, List<string> taken)
        {
            if (_registered.Contains(id)) return;
            if (RegisterHotKey(_hwnd, id, ModControl | ModAlt | ModNoRepeat, key))
            {
                _registered.Add(id);
            }
            else
            {
                taken.Add(label);
                LogService.Log($"[hotkey] {label} is already in use by another app");
            }
        }

        public void Disable()
        {
            foreach (int id in _registered) UnregisterHotKey(_hwnd, id);
            _registered.Clear();
        }

        private IntPtr WindowProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam, UIntPtr id, UIntPtr data)
        {
            if (msg == WmHotkey && _actions.TryGetValue((int)wParam, out Action? action))
            {
                try
                {
                    action();
                }
                catch (Exception ex)
                {
                    LogService.Log($"[hotkey] action failed: {ex.Message}");
                }
                return IntPtr.Zero;
            }
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }

        public void Dispose()
        {
            Disable();
            if (_hooked)
            {
                RemoveWindowSubclass(_hwnd, _proc, SubclassId);
                _hooked = false;
            }
        }
    }
}
