# Changelog

All notable changes to CastMirror. Versions follow [Semantic Versioning](https://semver.org/).

## 1.0.0 - 2026-09-30

First stable release. It ships as two separate GitHub releases: `windows-v1.0.0` (installer and zip) and `linux-v1.0.0` (.deb and .rpm). The Windows client is the primary product; the Linux build shares the engine and gets its fixes.

### Fixed
- The Media Foundation encoder leaked one output sample per frame when the transform allocated its own samples, about 110 MB a minute at 1080p60. A 10-minute soak now stays within 12 MB of its baseline.
- Listing windows crashed the process when a window title needed more than 512 bytes in UTF-8.
- The Linux build broke on the Windows-only window-capture loop and on a Windows-only audio member.
- Starting a cast and stopping it at the same time could free the session under the connecting thread. `Stop` now cancels an in-flight start safely.
- A state callback that read stats, or a UI poll during connect, could deadlock or freeze for the whole connection attempt.
- The per-session AES key and IV mask were written to the session log. They are now redacted in every log sink.
- A receiver that answered `error` to the offer made the app wait out the full answer timeout. It now fails at once, and answers for a different sequence number are ignored.
- Pressing Back on the TV ended the app on the receiver, and CastMirror relaunched it for 30 seconds. It now ends the cast.
- The Media Foundation encoder never worked on GPU hardware encoders, which are asynchronous MFTs. It now unlocks them, disables B-frames, guarantees SPS/PPS on key frames, and falls back to x264 if it never produces a frame.
- Frame ids advanced when the encoder produced nothing, so the receiver stalled until the next key frame.
- A failed screen capture silently streamed a test pattern to the TV. It now stops with an error.
- Capture read every desktop surface as 8-bit BGRA. Microsoft documents that desktop duplication delivers BGRA, but a driver that hands back 16-bit float, 10-bit or RGBA surfaces would have produced noise. Those formats are now converted (float is tone-mapped to SDR), and an unknown format stops capture with an error. This is defensive: it has not been seen on an HDR display.
- Audio stopped for good when the default playback device changed. Capture now follows the new device and restarts if it dies.
- 24-bit and 32-bit audio devices were read as 16-bit, and surround sound dropped everything but the front pair.
- RTCP packets that only carried NACKs pulled the smoothed round-trip time and loss toward zero, hiding real congestion.
- Discovery listed phantom devices from unrelated mDNS traffic and used the wrong address behind some reflectors.
- Adaptive-bitrate and reconnect state were shared between threads without locks.
- The Game and Cinema presets did nothing different from Auto. They now set their own delay and bitrate.
- Capture frame rate was not limited to what the TV can decode.
- Second launch of the app exited silently instead of showing the running window.
- Quitting from the tray icon dropped a cast without asking.
- The main panel clipped its controls at small window sizes or high display scaling.
- The audio switch and Mute TV both drove one engine flag and overwrote each other. Freeze and mute stayed on into the next session.
- A device added by IP appeared twice once discovery found it, and could not be removed.
- Changing preset overwrote the target delay you had set.
- The Stop button lost its red on mouse-over, and the LIVE badge was unreadable in the light theme.
- Config saves could interleave and corrupt the file, and a failed write replaced the good file.
- Colours were converted with BT.601 while TVs decode HD video as BT.709, so reds and greens looked shifted. The stream is now converted and tagged as BT.709.
- The offer named a codec the encoder never produced when a mirroring-incompatible codec was requested.
- A TV that was switched off stayed in the device list forever. Discovery now drops it after ten minutes of silence. Devices added by address stay.
- Closing the app during a cast dropped the final Stopping and Idle events.
- The RTCP socket was closed before its thread stopped, which could hand the descriptor to an unrelated socket.
- A window source started through the C API lost its title and geometry, so "cast to last" fell back to the monitor.
- The status line kept saying "Scanning" after a TV had been found.

### Added
- Windows installer with firewall rules, an optional sign-in start, and a clean uninstall.
- Keyboard shortcuts and live-region announcements for screen readers.
- Remembers the window size and the last TV and source.
- Show or hide the mouse pointer in the mirrored picture.
- Configurable reconnect window (30 seconds to 10 minutes).
- Logs window seeds from disk, copies the selection, and can copy a bug-report bundle with IP addresses masked.
- Crash minidumps with a notice on the next launch. Nothing is uploaded.
- About dialog with an on-request update check.
- The window shows the engine's advice ("Wi-Fi is dropping packets...") and reconnect progress. C API stats grew to ABI version 3.
- Optional global shortcuts: Ctrl+Alt+C, Ctrl+Alt+F, Ctrl+Alt+M.
- Window capture uses Windows.Graphics.Capture (Windows 10 1903+), so windows in front of the shared window no longer appear in the stream. Older Windows falls back to cropping the desktop.
- Share the audio of a single app instead of the whole PC (Windows 10 2004+). If the chosen app isn't running when the cast starts, no audio is shared. `scripts\verify_process_audio.ps1` proves it against two real processes.
- Spanish, German and French translations (unreviewed machine translations). The UI follows the Windows display language, and missing strings fall back to English.
- Licence bundle with the GPL notice and source offer for the libx264 build.

### Removed
- The HTTP/CAF fallback server. It bound every interface without authentication and its session path would terminate the process.

### Known gaps
- Windows binaries are not code-signed, so SmartScreen warns on first run.
- Verified against a simulated receiver and unit tests, not against a physical-device matrix. The rows in `docs/DEVICE_MATRIX.md` predate this release and were not re-measured for it.
- The async Media Foundation path and the HDR path have unit or simulated coverage only. The Media Foundation path needs a run on NVIDIA, AMD and Intel hardware.
- Rotated (portrait) monitors are captured without rotation correction.
- Translations are machine-made and unreviewed by native speakers. Right-to-left languages are not handled.
