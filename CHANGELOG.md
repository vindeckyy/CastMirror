# Changelog

All notable changes to CastMirror. Versions follow [Semantic Versioning](https://semver.org/).

## Unreleased

Windows-focused release-readiness pass.

### Fixed
- Starting a cast and stopping it at the same time could free the session under the connecting thread. `Stop` now cancels an in-flight start safely.
- A state callback that read stats, or a UI poll during connect, could deadlock or freeze for the whole connection attempt.
- The per-session AES key and IV mask were written to the session log. They are now redacted in every log sink.
- A receiver that answered `error` to the offer made the app wait out the full answer timeout. It now fails at once, and answers for a different sequence number are ignored.
- Pressing Back on the TV ended the app on the receiver, and CastMirror relaunched it for 30 seconds. It now ends the cast.
- The Media Foundation encoder never worked on GPU hardware encoders, which are asynchronous MFTs. It now unlocks them, disables B-frames, guarantees SPS/PPS on key frames, and falls back to x264 if it never produces a frame.
- Frame ids advanced when the encoder produced nothing, so the receiver stalled until the next key frame.
- A failed screen capture silently streamed a test pattern to the TV. It now stops with an error.
- HDR desktops (16-bit float surfaces) were read as 8-bit and sent as noise. They are tone-mapped to SDR.
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

### Added
- Windows installer with firewall rules, an optional sign-in start, and a clean uninstall.
- Global shortcuts, keyboard shortcuts, and live-region announcements for screen readers.
- Remembers the window size and the last TV and source.
- Show or hide the mouse pointer in the mirrored picture.
- Configurable reconnect window (30 seconds to 10 minutes).
- Logs window seeds from disk, copies the selection, and can copy a bug-report bundle with IP addresses masked.
- Crash minidumps with a notice on the next launch. Nothing is uploaded.
- About dialog with an on-request update check.
- Licence bundle with the GPL notice and source offer for the libx264 build.

### Removed
- The HTTP/CAF fallback server. It bound every interface without authentication and its session path would terminate the process.

### Known gaps
- The async Media Foundation path and the HDR path have unit or simulated coverage only. They need a run on NVIDIA, AMD and Intel hardware and on an HDR display.
- Rotated (portrait) monitors are captured without rotation correction.
- Window capture still crops the monitor image, so windows in front of the shared window appear in it.
- The app is English only.
