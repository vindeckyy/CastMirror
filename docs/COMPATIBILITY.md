# CastMirror Device Compatibility Matrix

| Hardware Model | Model String (`md`) | Capability Bitmask (`ca`) | Max H.264 Level | Supported Video Resolutions | Supported Audio | Target Playout Delay |
|---|---|---|---|---|---|---|
| **Chromecast (1st Gen)** | `Chromecast` / `H2G2-42` | `0x00000005` | Level 4.1 | 1080p30, 720p60 | Opus 48kHz stereo, AAC | 400 ms |
| **Chromecast (2nd Gen)** | `NC2-6A5` | `0x00000005` | Level 4.1 | 1080p30, 720p60 | Opus 48kHz stereo, AAC | 400 ms |
| **Chromecast (3rd Gen)** | `GA00439` | `0x00000005` | Level 4.2 | 1080p60, 720p60 | Opus 48kHz stereo, AAC | 200 - 400 ms |
| **Chromecast Ultra** | `NC2-6A5-D` | `0x00000007` | Level 4.2 / 5.1 | 4K30, 1080p60, 720p60 | Opus 48kHz stereo, AAC | 200 - 400 ms |
| **Chromecast with Google TV (HD)** | `G454V` | `0x00000007` | Level 4.2 | 1080p60, 720p60 | Opus 48kHz stereo, AAC | 200 - 400 ms |
| **Chromecast with Google TV (4K)** | `GZRNL` | `0x00000007` | Level 5.1 | 4K60 (VP9), 4K30 (H.264), 1080p60 | Opus 48kHz stereo, AAC | 200 - 400 ms |
| **Google TV Streamer (4K)** | `GR1XN` | `0x00000007` | Level 5.2 | 4K60 (H.264/HEVC/AV1) | Opus 48kHz stereo, AAC | 200 - 400 ms |
| **Google Nest Hub (1st/2nd Gen)** | `Google Nest Hub` | `0x00000005` | Level 3.1 | 720p60, 720p30 | Opus 48kHz stereo | 400 ms |
| **Google Nest Hub Max** | `Nest Hub Max` | `0x00000005` | Level 4.1 | 1080p30, 720p60 | Opus 48kHz stereo | 400 ms |
| **Vizio / Sony / Philips Smart TV (Built-in)** | `Cast TV` | `0x00000005` | Level 4.2 | 1080p60, 720p60 | Opus 48kHz stereo | 400 ms |

Nest Hub class devices are **720p-class**. Do not expect 1080p60 there.

---

## Quality presets (current sender)

These are the user-visible profiles in the GTK GUI and CLI. Resolution is still bounded by the device capability model. Video bitrate defaults below are the slider starting points (1–25 Mbps, remembered per profile once changed). The slider is **locked while connecting or live**.

| Preset | Encode size (typical) | Default video bitrate | Target delay |
|---|---|---|---|
| **Auto** | Up to 1080p60, then adapts | 8 Mbps | 200 ms |
| **High** | Capture size up to device max (1080p60 / 4K when allowed) | 12 Mbps | 200 ms |
| **Balanced** | 1080p | 8 Mbps | 200 ms |
| **Smooth** | 720p60 | 5 Mbps | 200 ms |

**Host audio:** while a session captures the default sink monitor, CastMirror mutes that sink so the PC speakers do not double the TV. Mute is restored on Stop.

---

## Capture sources: screen vs window

CastMirror supports two capture source kinds:

| Kind | X11 | Wayland (portal) |
|---|---|---|
| **Screen (monitor)** | XRandR per-monitor crop from root window | Portal `SelectSources` with type bitmask `1` (monitor) |
| **Window** | Direct window capture via XGetImage/XShmGetImage on the window drawable, with XComposite redirection for occluded-window correctness. Window geometry and lifecycle are tracked without treating temporary workspace unmaps as source destruction | Portal `SelectSources` with type bitmask `2` (window). The compositor's native picker handles selection |

### X11 window capture notes

- Windows are enumerated from the EWMH client list, including managed i3 clients on inactive workspaces. The non-EWMH fallback walks the toplevel tree and filters to viewable windows. Override-redirect windows (popups, docks, tooltips) and CastMirror's own window are excluded.
- On i3, CastMirror temporarily makes the selected source floating and sticky, transparent, and click-through. It therefore stays mapped and keeps rendering while you switch workspaces. The original workspace, floating/sticky/fullscreen state, size, position, opacity, and input shape are restored when casting stops.
- Window IDs are X11 XIDs (cast to `int`). They are **not stable across application restarts** — CastMirror persists the window title and re-resolves by name when restoring the last source. If the window is no longer open, it falls back to the last monitor.
- XComposite automatic redirection is used so occluded windows still capture correctly. If XComposite is unavailable, capture proceeds but may tear for occluded windows.
- XDamage is used on the window drawable (not root) for efficient change detection.
- Cursor compositing (XFixes) is offset by the window's root-relative position so the cursor appears in the right place.
- When a shared window is closed, the session fails with the message "Shared window was closed" rather than silently falling back to monitor capture.

### Wayland portal window notes

- The system portal picker (e.g. `xdg-desktop-portal`) handles window selection natively. CastMirror passes the window source type to `SelectSources`; the user picks the window in the compositor's dialog.
- Portal restore tokens are scoped to the source kind. Switching between screen and window may trigger a new picker dialog.
- Not all compositors/portal implementations support window selection. CastMirror hides the Window toggle in the GUI when the backend reports `SupportsWindowCapture() == false`.

---

## Adaptation Ladder Profiles

CastMirror dynamically steps down or up along the following 8 rungs depending on RTCP packet loss and round-trip time:

| Rung | Resolution | FPS | Target Bitrate | Max Bitrate | Min Bitrate | Trigger Condition |
|---|---|---|---|---|---|---|
| **0 (Ultra 4K)** | 3840 x 2160 | 60 | 25,000 kbps | 35,000 kbps | 18,000 kbps | 4K device, 0% loss, RTT < 20ms |
| **1 (4K Standard)** | 3840 x 2160 | 30 | 18,000 kbps | 25,000 kbps | 12,000 kbps | 4K device, loss < 1%, RTT < 35ms |
| **2 (1440p High)** | 2560 x 1440 | 60 | 12,000 kbps | 18,000 kbps | 8,000 kbps | 1440p+ device, loss < 1.5% |
| **3 (1080p60 Max)** | 1920 x 1080 | 60 | 8,000 kbps | 12,000 kbps | 5,000 kbps | Gen3/Ultra/GTV, loss < 2% |
| **4 (1080p30 Default)**| 1920 x 1080 | 30 | 5,000 kbps | 8,000 kbps | 3,500 kbps | Default baseline for all devices |
| **5 (720p60 High-Motion)**| 1280 x 720 | 60 | 4,000 kbps | 6,000 kbps | 2,500 kbps | Loss 2% - 5%, RTT > 60ms |
| **6 (720p30 Resilient)** | 1280 x 720 | 30 | 2,500 kbps | 4,000 kbps | 1,500 kbps | Loss 5% - 10%, RTT > 90ms |
| **7 (540p30 Emergency)** | 960 x 540 | 30 | 1,200 kbps | 2,000 kbps | 800 kbps | Loss > 10%, RTT > 150ms |
