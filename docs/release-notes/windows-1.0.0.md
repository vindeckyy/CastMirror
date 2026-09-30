CastMirror 1.0.0 is the first stable release of the Windows app. It mirrors your whole screen or one window, with sound, to a Chromecast, Google TV or Cast-enabled TV on the same network. It does not use Chrome.

## Download

| File | Use it for |
|---|---|
| `CastMirror-Setup-1.0.0-x64.exe` | The installer. Adds a Start menu entry and Windows Firewall rules for private and domain networks. |
| `CastMirror-win-x64.zip` | Portable copy. Run `scripts\setup_firewall.ps1` once from an elevated PowerShell, or allow CastMirror when Windows asks. |
| `SHA256SUMS-windows.txt` | Checksums for both files. |

Needs Windows 10 version 2004 or later, 64-bit. The binaries are not code-signed yet, so SmartScreen will warn on first run: choose **More info**, then **Run anyway**. Compare the file against `SHA256SUMS-windows.txt` first if you want to be sure.

## What you get

**Sharing**
- A whole display, or a single window. Window sharing uses Windows.Graphics.Capture (Windows 10 1903+), so a window that is in front of the shared one stays out of the picture. Older Windows crops the desktop instead.
- System sound through WASAPI loopback, or the sound of one app only (Windows 10 2004+). If that app isn't running when the cast starts, nothing is shared rather than everything. The sound follows your default speakers when you switch to headphones, and 5.1 or 7.1 is folded down to stereo.
- A steady 30 or 60 fps even when nothing on screen changes.
- Show or hide the mouse pointer in the mirrored picture.

**Quality and reliability**
- H.264 on the GPU through Media Foundation, with an automatic fall back to x264 on the CPU if the GPU encoder produces nothing. Audio is Opus.
- Colours are converted and tagged as BT.709, which is what TVs decode HD video as. Earlier builds converted with BT.601, which shifted reds and greens.
- Six presets (Auto, High, Balanced, Smooth, Game, Cinema) and live bitrate changes. The sender lowers bitrate and resolution when packets are lost and raises them again when the link recovers.
- Reconnects after a dropped connection, a sleep or a Wi-Fi change. You choose how long it keeps trying, from 30 seconds to 10 minutes.
- Freeze the picture or mute the TV without ending the cast.

**The app**
- Finds TVs over mDNS. If your router blocks mDNS, an opt-in subnet scan finds them directly, and **Add by IP** works for the rest.
- Runs as a single instance: starting it again brings the running window forward. Closing the window keeps the cast in the tray.
- Optional start at sign-in, desktop notifications, light and dark themes, and system-wide shortcuts (Ctrl+Alt+C start or stop, Ctrl+Alt+F freeze, Ctrl+Alt+M mute).
- Screen-reader announcements and keyboard shortcuts in the window.
- English, Spanish, German and French, following your Windows display language. The three translations are machine-made and unreviewed, so corrections are welcome.
- A Logs window with a **Copy for bug report** button that masks the last part of every IP address. A crash saves a memory dump in `%APPDATA%\CastMirror\crashes` and tells you on the next launch. Nothing is uploaded.

## Fixed since the pre-release builds

- The Media Foundation encoder leaked one sample per frame, about 110 MB a minute at 1080p60. A 10-minute soak now stays within 12 MB of its starting size.
- Listing windows crashed the app when a window title was long or used many non-Latin characters.
- The per-session encryption key and IV mask were written to the log. They are now redacted everywhere.
- Starting and stopping a cast at the same moment could free the session while it was still connecting. Stop now cancels an in-flight start.
- A state callback could deadlock the whole connection attempt.
- A TV that answered `error` to the offer made the app wait out the full timeout. It now fails at once.
- Pressing Back on the TV ended the app on the receiver and CastMirror relaunched it for 30 seconds. It now ends the cast.
- The GPU encoder path never worked on hardware encoders, which are asynchronous. It now unlocks them, disables B-frames and guarantees SPS/PPS on key frames.
- A failed screen capture streamed a test pattern to the TV. It now stops with an error.
- Audio stopped for good when the default playback device changed. Capture now follows it.
- 24-bit and 32-bit audio devices were read as 16-bit.
- Discovery listed phantom devices from unrelated mDNS traffic, and a TV that was switched off stayed in the list forever.
- Config saves could interleave and corrupt the file.
- The main panel clipped its controls at small sizes or high display scaling, and a second launch exited silently.

The complete list is in the [changelog](https://github.com/vindeckyy/CastMirror/blob/master/CHANGELOG.md).

## How it was tested

- 210 automated tests pass on Windows, including full TLS, offer and answer, and media streaming against a simulated receiver. Window capture is tested against real windows, and per-app audio against two real processes.
- The suite also runs under UBSan, and the RTCP and mDNS parsers are mutation-fuzzed.
- A 10-minute soak of a 1080p desktop stream shows a flat memory and handle count.
- The WinUI client has unit tests and a UI Automation smoke test.

## Known limits

- **Not yet verified on a physical-device matrix.** Everything above was tested against a simulated receiver. The device rows in `docs/DEVICE_MATRIX.md` predate this release and were not re-measured for it. If you try it on a real TV, a report in an issue is welcome.
- The GPU (Media Foundation) path has not been run on NVIDIA, AMD or Intel hardware by the maintainers, and HDR displays are handled defensively (converted to SDR) but not verified. Turn on **Force software encode** in Settings if the picture glitches.
- Portrait (rotated) monitors are captured without rotation correction.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset. This is not a game-streaming tool.
- Cast receivers accept H.264 and VP8 for mirroring, so there is no HEVC or VP9 option.

## Licence

CastMirror's own source is Apache-2.0. The Windows build includes FFmpeg and libx264, which are GPL, so the download as a whole is distributed under the GPL. The notice and the source offer are in `THIRD-PARTY-LICENSES.txt` inside the download.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
