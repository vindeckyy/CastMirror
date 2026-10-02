# Changelog

All notable changes to CastMirror. Versions follow [Semantic Versioning](https://semver.org/).

## Unreleased

### Added
- The Windows shell is redesigned: a custom title bar with a search field (Ctrl+K), a left navigation rail, a hero banner, a device card grid, a right-hand casting rail and a status bar, replacing the previous stacked settings panel. The design tokens behind it live in `App.xaml` and carry real values for the dark, light and high-contrast schemes, so the shell stays legible when the light theme is forced.

### Fixed
- The Debug build started with no devices and no sources. `castcore.dll` is linked against FFmpeg, OpenSSL, protobuf and the MinGW runtime, and those are hard imports that the loader resolves before the app's managed entry point runs, but only `castcore.dll` itself was copied next to the exe. The app therefore came up reporting "castcore.dll was not found next to CastMirror.exe" unless something else happened to put the MSYS2 toolchain on PATH — which is what `run-gui.bat` did. Debug and test runs now get the same dependency closure `package.ps1` already shipped.
- Audio drifted out of sync with video on the TV, further the longer a film ran. Two clocks were driving the audio timeline independently and disagreeing: real capture is stamped from the WASAPI render position and only advances when the endpoint delivers a buffer, while the silence frames sent when the endpoint is quiet (or while muted) were stamped with the wall clock. Each one could therefore be stamped behind the other, punching a hole in the audio RTP timeline or stepping it backwards, and the receiver never caught up. The emitted audio timeline now advances by exactly one frame per frame sent.
- Video sat permanently late against audio by the source's present lag. Two clocks drove the video timeline: a freshly captured frame carries the present time DXGI reports (about 20 ms behind wall clock), while a re-send — emitted whenever the desktop produced no new frame — was stamped from the pacer's tick deadline, which is wall clock. Each re-send stepped the video timeline forward by that lag, and every frame after it was pinned behind, so the receiver, which plays by RTP timestamp, showed the picture late for the whole session. The timeline now takes the source's present time when it leads, and otherwise steps by the time that actually elapsed since the last emit, so video stays on the source clock on every path and keeps real-time rate through a stall.
- Moving the mouse pulled the picture out of sync again. A cursor-only desktop update carries no present time, and its content timestamp fell back to the wall clock — about 20 ms ahead of the source timeline — so every mouse movement stepped the video clock forward by the present lag. A cursor-only update changes nothing about the desktop image, so it now carries the previous frame's present time forward.
- Casting a single window put the wall clock into every video timestamp for the whole session. Windows.Graphics.Capture has no present-time handling at all: each frame was stamped with "now", which sits about 20 ms ahead of the audio timeline, so a window cast was skewed from the first frame. Each frame is now stamped from its own content instant, falling back to the previous one when a frame reports none.
- The video encoder was torn down and rebuilt about twenty times in five minutes, freezing the picture. A geometry change was the only case that needed a reconfigure, but every bitrate-only adjustment rebuilt the encoder too. Bitrate changes now apply in place and request a key frame.
- A congestion-imposed bitrate ceiling never lifted. It was cleared by a counter that the bitrate ramp reset every time it fired, so the bitrate ramped up and was immediately clamped back for the rest of the session. It now has its own counter.
- Ordinary round-trip-time wobble was read as a rising congestion trend and downshifted the bitrate on a healthy link. A tick that failed to clear the trend margin now breaks the streak instead of silently continuing it.
- Audio captured from an endpoint that is not 48 kHz stereo ran slow against the video clock, drifting further out of sync the longer the film ran. The resampler discarded the fraction of a sample it had not finished emitting at each packet boundary, and rejected a whole pending sample whenever the rate ratio was a whole number. Up to 0.21% of the audio was lost — about 2 ms every second. The phase is now carried across packets and finished from the sample it retained.
- "Cast one application window" did nothing. The radio's checked state was bound one way, so a click never reached the view model and Start casting still mirrored the screen. The choice now takes effect, a window picker appears beneath it, and the radio, that picker and the Cast page's source list follow each other.
- Several controls in the Windows shell were out of line. The caption buttons overlapped the title-bar icons (the bar is now 48 px tall to match them), the search field was off-centre with its magnifier hidden behind the box and its text above the icon's line, the selected nav entry stopped short of the rail edges, the casting-rail radio marks sat against the card edge and below their labels, and Start casting did not stretch like the buttons around it. Settings expanders and the log list also lined up with neither their headers nor the filter row.

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
