# CastMirror Device Compatibility Matrix

Classification is **model-name based**: `CapabilityModel::Evaluate` (`core/src/capability_model.cc`) lowercases the mDNS `md` value and substring-matches it. The mDNS `ca` capability bitmask is parsed into `CastDevice::capabilities` (`core/src/device_discovery.cc`) but no classifier consumes it, so there is no `ca` column here.

| Device family (as classified) | `md` substrings matched (lowercased) | Max H.264 level | Max resolution / FPS | Max bitrate | Supported audio |
|---|---|---|---|---|---|
| Nest Hub | `nest hub`, `google home hub` | 3.1 | 1280x720 @ 60 | 5000 kbps | Opus 48kHz stereo |
| Chromecast Ultra | `ultra` | 5.1 | 3840x2160 @ 60 | 25000 kbps | Opus 48kHz stereo, AAC |
| Chromecast Gen 1/2 | `h2g2-42`, `nc2-6a5` (excludes `nc2-6a5-d`) | 4.1 | 1920x1080 @ 30 | 8000 kbps | Opus 48kHz stereo, AAC |
| Chromecast with Google TV | `google tv`, `streamer`, `android tv`, `bravia` | 5.1 | 3840x2160 @ 60 | 25000 kbps | Opus 48kHz stereo, AAC |
| Chromecast Gen 3 | `chromecast` | 4.2 | 1920x1080 @ 60 | 15000 kbps | Opus 48kHz stereo, AAC |
| Generic Cast TV | anything else | 4.2 | 1920x1080 @ 60 | 12000 kbps | Opus 48kHz stereo |

Real-hardware `md` values seen in the field (informational — only the substrings above drive classification): Chromecast 1st gen `H2G2-42`, 2nd gen `NC2-6A5`, Ultra `Chromecast Ultra`, Chromecast with Google TV 4K `GZRNL`, Google TV Streamer `G3MYX`. A model code that contains none of the matched substrings falls through to the Generic Cast TV row.

Nest Hub class devices are **720p-class** (`1280x720`). There is no separate Nest Hub Max branch — a model string containing `nest hub`, including `Nest Hub Max`, matches the same 720p / Level 3.1 branch.

Every row defaults to a **200 ms** target playout delay (`DeviceCapabilities::default_target_delay_ms`, `core/include/castcore/capability_model.h`). Game starts at 150 ms and Cinema at 400 ms, and `AdaptiveController` clamps every value to 150–400 ms while adapting it at runtime: `StepUpPlayoutDelay` walks 150→200→300→400 ms when jitter exceeds 30 ms or loss exceeds 3% (or on a NACK burst / PLI) and `StepDownPlayoutDelay` walks back after 15 consecutive clean intervals (`core/src/adaptive_controller.cc`).

---

## Quality presets (current sender)

These six presets — `QualityPreset::kAuto, kHigh, kBalanced, kSmooth, kGame, kCinema` (`core/include/castcore/types.h`) — are the user-visible profiles in both the GTK GUI (`app/gui/cast_tab.cc` renders all six cards) and the CLI (`--preset`, and the interactive `[P]` cycle). Resolution is still bounded by the device capability model. Video bitrate defaults below are the slider starting points (1–25 Mbps, remembered per profile once changed). The slider is **locked while connecting or live**.

| Preset | Encode size (typical) | Default video bitrate | Target delay |
|---|---|---|---|
| **Auto** | Up to 1080p60, then adapts | 8 Mbps | 200 ms |
| **High** | Capture size up to device max (1080p60 / 4K when allowed) | 12 Mbps | 200 ms |
| **Balanced** | 1080p | 8 Mbps | 200 ms |
| **Smooth** | 720p60 | 5 Mbps | 200 ms |
| **Game** | Locked adaptive resolution | 8 Mbps | 150 ms |
| **Cinema** | Capture size up to device max | 16 Mbps | 400 ms |

Default bitrates come from `QualityPresetDefaultBitrateKbps` (`core/include/castcore/types.h`): Cinema 16000, High 12000, Game 8000, Smooth 5000, Auto/Balanced 8000 kbps. Game and Cinema also set `target_delay_ms` to 150 / 400 and toggle `adaptive_resolution_enabled` in `CastTab::OnPresetChanged` (`app/gui/cast_tab.cc`).

**Host audio:** while a session captures the default sink monitor, CastMirror mutes that sink so the PC speakers do not double the TV. Mute is restored on Stop.

---

## Capture sources: screen vs window

CastMirror supports two capture source kinds:

| Kind | X11 | Wayland (portal) |
|---|---|---|
| **Screen (monitor)** | XRandR per-monitor crop from root window | Portal `SelectSources` with type bitmask `1` (monitor) |
| **Window** | Direct window capture via XGetImage/XShmGetImage on the window drawable, with XComposite `CompositeRedirectManual` for occluded-window correctness. Window geometry is tracked via `ConfigureNotify`; window destruction/unmap is detected via `DestroyNotify`/`UnmapNotify` and signals `source_lost` | Portal `SelectSources` with type bitmask `2` (window). The compositor's native picker handles selection |

### X11 window capture notes

- Windows are enumerated by walking the toplevel tree and filtering to viewable, managed windows (`WM_STATE == NormalState`) with non-empty titles. Override-redirect windows (popups, docks, tooltips) are excluded.
- Window IDs are X11 XIDs (cast to `int`). They are **not stable across application restarts** — CastMirror persists the window title and re-resolves by name when restoring the last source. If the window is no longer open, it falls back to the last monitor.
- XComposite redirection (`CompositeRedirectManual`) is used so occluded windows still capture correctly. If XComposite is unavailable, capture proceeds but may tear for occluded windows.
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

The ladder is defined in exactly one place — `AdaptiveController::BuildLadder()` (`core/src/adaptive_controller.cc`) — and each rung is only a resolution, a framerate and a target bitrate (`struct LadderRung`, `core/include/castcore/adaptive_controller.h`). There are no per-rung min/max bitrate fields and no RTT-threshold table; the controller reacts to observed RTCP loss/RTT at runtime.

| Rung | Resolution | FPS | Target bitrate |
|---|---|---|---|
| **0 (4K60)** | 3840 x 2160 | 60 | 25,000 kbps |
| **1 (4K30)** | 3840 x 2160 | 30 | 16,000 kbps |
| **2 (1440p60)** | 2560 x 1440 | 60 | 12,000 kbps |
| **3 (1080p60)** | 1920 x 1080 | 60 | 8,000 kbps |
| **4 (1080p30)** | 1920 x 1080 | 30 | 5,000 kbps |
| **5 (720p60)** | 1280 x 720 | 60 | 3,500 kbps |
| **6 (720p30)** | 1280 x 720 | 30 | 2,000 kbps |
| **7 (540p30)** | 960 x 540 | 30 | 1,200 kbps |
