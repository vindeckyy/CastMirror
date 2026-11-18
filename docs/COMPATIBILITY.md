# CastMirror Platform & Device Compatibility

## 1. Supported Operating Systems & Packaging Targets

| Platform | Target / Baseline | Notes |
|---|---|---|
| **Linux (Ubuntu 24.04+)** | FFmpeg 6, GTK `>= 4.12`, `libadwaita-1 >= 1.5` | `.deb` uses `dpkg-shlibdeps` and recommends `xdg-desktop-portal`; official release `.deb` builds omit tray (`-DCASTMIRROR_ENABLE_TRAY=OFF`). |
| **Linux (Debian 13 trixie)** | FFmpeg 7, GTK `>= 4.12`, `libadwaita-1 >= 1.5` | Official release `.deb` built with `-DCASTMIRROR_ENABLE_TRAY=OFF`. |
| **Linux (Fedora)** | `ffmpeg-free` (`libopenh264` fallback), GTK `>= 4.12`, `libadwaita-1 >= 1.5` | RPM packaging uses `GPL-2.0-or-later`; built and verified in CI on `fedora:latest`. |
| **Windows 10 (1903+)** | Build 18362+ | Required for **Windows.Graphics.Capture (`WgcWindowSource`)** single-window capture (`CASTMIRROR_DISABLE_WGC=1` disables WGC and falls back to crop). |
| **Windows 10 (2004+) / Windows 11** | SDK / MinVersion `10.0.19041.0` | Required by **WASAPI per-process audio loopback** (`audio_sessions.cc` / `audio_capture_wasapi.cc`) and the WinUI 3 desktop app (`app/winui/CastMirrorApp.csproj`: `win-x64` shipped; `win-arm64` defined). |

---

## 2. Capture & Audio Backends

### Video Capture
- **Linux X11 (`X11DisplayCapture`, `core/src/display_capture.cc`):** Uses XRandR monitor enumeration, `XShmGetImage` (with `XGetImage` fallback), and XComposite + XDamage + XFixes (including cursor composition).
- **Linux Wayland (`PipeWirePortalCapture`, `core/src/display_capture_wayland.cc`):** Uses `org.freedesktop.portal.ScreenCast` (`xdg-desktop-portal`) + PipeWire `pw_stream`, supporting zero-copy DMA-BUF NV12 buffers with `mmap` fallback. Setting `CASTMIRROR_FORCE_X11=1` forces X11 capture instead of Wayland portal.
- **Windows (`DisplayCaptureWgc` + `WgcWindowSource` + `FramePacer`, `core/src/display_capture_wgc.cc`, `core/src/wgc_window_source.cc`, `core/include/castcore/frame_pacer.h`):**
  - Primary monitor/region capture uses **DXGI Desktop Duplication**, supporting `BGRA8`, `RGBA8`, `RGB10A2`, and `RGBAF16` (HDR tone-mapped to SDR in `pixel_convert.cc` via CPU LUT), with `10 x 500 ms` `DXGI_ERROR_ACCESS_LOST` recovery rebuilds.
  - Single-window capture uses **`WgcWindowSource`** (Windows.Graphics.Capture), with desktop-crop fallback when `CASTMIRROR_DISABLE_WGC=1` is set.
  - GPU scaling, format conversion (`BGRA → NV12`), and window-capture upscaling run in `gpu_processor.cc`.

### Audio Capture & Sinks
- **Linux (`core/src/audio_capture.cc`):** PulseAudio / PipeWire-Pulse loopback (`libpulse`), 48 kHz stereo PCM encoded to Opus (`audio_encoder.cc`, 64–256 kbps, default **192 kbps**). Host sink auto-muting (`mute_host_audio`) is supported on Linux PulseAudio/PipeWire.
- **Windows (`core/src/audio_capture_wasapi.cc`, `core/src/audio_sessions.cc`):** WASAPI system loopback and Windows 10 2004+ per-process audio loopback (`CastMirror` process-filtered capture). Host speaker auto-muting does not apply on Windows WASAPI and is ignored there.

---

## 3. Video Encoders, Codecs & Fallback Behavior

| Codec | Backend Selection | Notes / Fallback Behavior |
|---|---|---|
| **H.264** (primary) | Linux: `h264_vaapi` → `libx264` → `libopenh264`; Windows: `MediaFoundationVideoEncoder` (HW MFT) → `FFmpegVideoEncoder` (`libx264`) | **Linux VAAPI:** `video_encoder.cc` probes `/dev/dri/renderD*` nodes (overridable via `CASTMIRROR_VAAPI_DEVICE`; forced software via `CASTMIRROR_FORCE_SOFTWARE_ENCODE=1`). On Fedora `ffmpeg-free` builds without `libx264`, FFmpeg falls back to `libopenh264`.<br>**Windows Media Foundation:** `video_encoder_mf.cc` enforces hardware-only MFT selection (`MFT_ENUM_FLAG_HARDWARE`) and automatically fails over to `FFmpegVideoEncoder` (`libx264`) after **90** consecutive initial failures or **300** consecutive steady-state failures. |
| **VP8** | `FFmpegVideoEncoder` software init (`libvpx`) | Exposed via CLI (`--codec vp8`) and config; `InitEncoder()` only enters `TryInitVaapi()` for `VideoCodec::kH264`, so VP8 uses `InitSoftware()` (`libvpx`). |
| **VP9 / HEVC / AV1** | Coerced to **H.264** | Neither Cast V2 screen mirroring receivers nor `CastSession::Start()` enable VP9/HEVC/AV1: `CastSession::Start()` and `CodecFallbackEncoder` coerce unsupported codec selections to `"h264"`. |

Neither the GTK 4 GUI (`app/gui/`) nor the WinUI 3 GUI (`app/winui/`) exposes a video codec dropdown; `--codec <h264|vp8>` is exposed in the CLI (`app/cli/main.cc`).

---

## 4. System Tray Requirements

- **Linux (`app/gui/tray.cc`, `app/CMakeLists.txt`):** Optional dependency on `ayatana-appindicator-glib-2.0` (or `ayatana-appindicator-glib`). Guarded by `CASTMIRROR_ENABLE_TRAY` and `CASTMIRROR_REQUIRE_TRAY` (`CASTMIRROR_HAVE_TRAY`). Disabled in Ubuntu 24.04 and Debian 13 `.deb` release builds (`-DCASTMIRROR_ENABLE_TRAY=OFF`), and verified enabled on Ubuntu 26.04 in CI (`CASTMIRROR_REQUIRE_TRAY=ON`).
- **Windows (`app/winui/Services/TrayIconService.cs`):** Built-in Win32 notification icon using `Shell_NotifyIconW`; no external dependency needed.

---

## 5. Cast Receiver Capability Profiles (`capability_model.cc`)

`CapabilityModel` (`core/include/castcore/capability_model.h`, `core/src/capability_model.cc`) resolves hardware profiles from the receiver's mDNS `md=` model string:

| Profile (`DeviceClass`) | Matched Receivers (mDNS `md=`) | Max Video | Preferred Codec | Default Target Delay | Min/Max Delay Clamp |
|---|---|---|---|---|---|
| `kChromecast4K` (`Chromecast 4K`) | `Chromecast with Google TV`, `Chromecast Ultra`, `Google TV Streamer` | 1920x1080 @ 60 fps (`16,000 kbps` cap) | H.264 | 200 ms | 150–400 ms |
| `kChromecastGen3` (`Chromecast HD / Gen 3`) | `Chromecast`, `Chromecast HD` | 1920x1080 @ 60 fps (`10,000 kbps` cap) | H.264 | 200 ms | 150–400 ms |
| `kAndroidTv` (`Android TV / Google TV`) | `SHIELD`, `BRAVIA`, `Mi Box`, `Android TV`, `Google TV`, `Hisense`, `TCL` | 1920x1080 @ 60 fps (`12,000 kbps` cap) | H.264 | 200 ms | 150–400 ms |
| `kSmartDisplay` (`Nest Hub / Smart Display`) | `Nest Hub`, `Google Home Hub`, `Smart Display` | 1280x720 @ 30 fps (`4,000 kbps` cap) | H.264 | 200 ms | 150–400 ms |
| `kNestAudio` (`Cast Audio / Nest Speaker`) | `Chromecast Audio`, `Nest Audio`, `Nest Mini`, `Google Home` (audio capability `cap_audio && !cap_video`) | Audio-only (Opus 48 kHz stereo; App ID `85CDB22F`) | Opus | 200 ms | 150–400 ms |
| `kGenericCast` (`Generic Cast Receiver`) | Unmatched mDNS devices & manual IPs | 1920x1080 @ 30 fps (`8,000 kbps` cap) | H.264 | 200 ms | 150–400 ms |

### Adaptive Playout Delay & Quality Presets
Every profile defaults to a **200 ms** target playout delay (`DeviceCapabilities::default_target_delay_ms`). At runtime, `AdaptiveController` (`core/src/adaptive_controller.cc`) clamps delay to **150–400 ms**: `StepUpPlayoutDelay()` steps `150 → 200 → 300 → 400 ms` when jitter exceeds **30 ms**, packet loss exceeds **3%**, or on a NACK burst / PLI; `StepDownPlayoutDelay()` steps back down after **15** consecutive clean intervals.

The 6 quality presets (`QualityPreset` in `core/include/castcore/types.h` and `CastTab` / `MainWindow` preset pickers):

| Preset | Default Video Bitrate (`QualityPresetDefaultBitrateKbps`) | Notes |
|---|---|---|
| **Auto / Balanced** | `8,000 kbps` | Default adaptive resolution/bitrate profile |
| **Smooth** | `5,000 kbps` | Favors frame pacing over peak detail |
| **High** | `12,000 kbps` | Higher bitrate target within device cap |
| **Game** | `8,000 kbps` | Sets `target_delay_ms = 150` and disables `adaptive_resolution_enabled` in GUI (`CastTab::OnPresetChanged`) |
| **Cinema** | `16,000 kbps` | Sets `target_delay_ms = 400` |
| **Custom** | User-configured | Uses manual bitrate, target delay, and resolution cap from Settings |

---

## 6. Supported Capture Sources & Audio

| Capture Mode | Linux Wayland | Linux X11 | Windows (10 / 11) |
|---|---|---|---|
| **Single Display** | Yes (via `xdg-desktop-portal` picker) | Yes (`X11DisplayCapture`, XRandR + `XShm`) | Yes (`DisplayCaptureWgc`, DXGI Desktop Duplication) |
| **Single Window** | Yes (via `xdg-desktop-portal` picker) | Yes (XComposite + XDamage + `XFixes`) | Yes (`WgcWindowSource` on Win10 1903+; crop fallback via `CASTMIRROR_DISABLE_WGC=1`) |
| **Region Selection** | Cropped from selected portal stream | Native region capture | Cropped from DXGI desktop capture |
| **System Audio** | Yes (PipeWire-Pulse loopback) | Yes (PulseAudio / PipeWire-Pulse) | Yes (WASAPI system loopback) |
| **Per-Application Audio** | PulseAudio/PipeWire stream matching when active | PulseAudio/PipeWire stream matching when active | Yes (WASAPI per-process loopback, Windows 10 2004+) |
| **Audio-Only Cast** | Yes (`85CDB22F`) | Yes (`85CDB22F`) | Yes (`85CDB22F`) |

---

## 7. Adaptive 8-Rung Bitrate & Resolution Ladder

`AdaptiveController` scales resolution and bitrate dynamically across 8 rungs (`core/src/adaptive_controller.cc`):

| Rung | Resolution | Framerate | Bitrate Floor / Target |
|---|---|---|---|
| **0 (Max / 1080p60)** | 1920 x 1080 | 60 | 12,000 kbps |
| **1 (1080p60)** | 1920 x 1080 | 60 | 8,000 kbps |
| **2 (1080p30)** | 1920 x 1080 | 30 | 6,000 kbps |
| **3 (900p60)** | 1600 x 900 | 60 | 5,000 kbps |
| **4 (900p30)** | 1600 x 900 | 30 | 3,500 kbps |
| **5 (720p60)** | 1280 x 720 | 60 | 3,000 kbps |
| **6 (720p30)** | 1280 x 720 | 30 | 2,000 kbps |
| **7 (540p30)** | 960 x 540 | 30 | 1,200 kbps |

---

## 8. Known Limits

- **IPv4 discovery and transport only:** mDNS socket setup (`224.0.0.251:5353`), `/24` subnet probing, and Cast UDP RTP sockets operate over IPv4.
- **H.264 / VP8 runtime video codecs only:** VP9, HEVC, and AV1 configurations fall back to H.264.
- **No DRM-protected content capture:** Protected swapchains/surfaces return black frames or blocked captures per OS compositor/WGC rules.
- **Single active receiver session at a time:** Multi-room speaker groups (`_googlezone._tcp.local`) can be discovered and cast to as a single Cast endpoint, not concurrent independent video sessions.
- **No host speaker muting on Windows WASAPI:** Automatic host mute during mirroring works on Linux PulseAudio/PipeWire sinks only.
