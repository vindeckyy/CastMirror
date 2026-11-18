CastMirror 1.0.4 for Windows fixes WinUI 3 interop, theme, selection, and live-telemetry bugs alongside shared-engine networking, media, and lifecycle fixes, and updates the version shown in the app to 1.0.4.

## Download

| File | Use it for |
|---|---|
| `CastMirror-Setup-1.0.4-x64.exe` | The installer. Adds a Start menu entry and Windows Firewall rules for private and domain networks. |
| `CastMirror-win-x64.zip` | Portable copy. Run `scripts\setup_firewall.ps1` once from an elevated PowerShell, or allow CastMirror when Windows asks. |
| `SHA256SUMS-windows.txt` | Checksums for both files. |

Needs Windows 10 version 2004 or later, 64-bit. The binaries are not code-signed yet, so SmartScreen will warn on first run: choose **More info**, then **Run anyway**. Compare the file against `SHA256SUMS-windows.txt` first if you want to be sure.

## Fixed in 1.0.4

- Fixed native-interop lifetime and Win32 bugs in the Windows client: `TrayIconService` now uses a per-instance window class and unregisters cleanly when toggled, `LogsViewModel` retains its native log callback delegate statically, `HotkeyService` handles 64-bit `wParam` without overflow, `CrashHandler` verifies `MiniDumpWriteDump` output, and `SettingsService` reads exact-size native JSON queries in one pass.
- Fixed WinUI shell, theme, and session-state bugs: `DeviceItem.Monogram` no longer adds character codes as integers, Light and HighContrast themes display readable Hero banner text and Casting Options radio rings, search filtering no longer clears `SelectedDevice`, live telemetry sparklines and disconnect controls are wired, and target-delay changes debounce and report save errors.
- Shared `castcore` fixes harden `Start()`/`Stop()` cancellation, adaptive bitrate ceilings, mDNS TXT bounds, thread-safe nonce generation, and WASAPI/WGC error handling. The About dialog, version footer, and update-check fallback all report 1.0.4.

## How it was tested
- The full native unit and integration suite passes locally on Linux (255 CTest cases: 254 passed, 1 Windows-only test skipped; 271 `TEST()` definitions across 31 test files) under GCC Debug and ASan + UBSan. The release workflow runs the Windows native suite, .NET client tests, and Windows UBSan before packaging and publishes the installer and portable zip from that build.
- XAML/C# binding resolution and localization key parity (242 keys across `de`, `es`, `fr`) are verified alongside the .NET `CastMirror.Tests` suite in CI.

## Known limits

- **Not yet verified on a physical-device matrix.** The Windows app was not exercised on a live desktop for this release, so if you try it on a real TV, a report in an issue is welcome.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset. This is not a game-streaming tool.
- Cast receivers accept H.264 and VP8 for mirroring, so there is no HEVC or VP9 option.

## Licence

CastMirror's own source is Apache-2.0. The Windows build includes FFmpeg and libx264, which are GPL, so the download as a whole is distributed under the GPL. The notice and the source offer are in `THIRD-PARTY-LICENSES.txt` inside the download.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
