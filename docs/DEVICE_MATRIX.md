# CastMirror Physical Hardware Compatibility & Device Matrix

This matrix documents real-hardware validation, empirical performance boundaries, firmware targets, and validated streaming parameters across Google Cast hardware generations.

> **Status for 1.0.0.** The rows in the table below predate the 1.0.0 release and were not re-measured for it. The 1.0.0 release was tested against a simulated receiver, not against a physical-device matrix, so read every row as unverified until it is re-run with `scripts/soak_real_device.sh`. If you test a device, an issue or pull request with the firmware string and the `soak_parse.py` output is welcome.

## How to read this table

Two different kinds of claim appear below, and they are **not** equally strong:

- **Tested Firmware** is a specific build string. If present, the row records an
  actual session against that hardware.
- **Playout Delay (p50 / p95)** and **Validation Status** are *measurements*, not
  predictions. They come from real-device soak runs
  (`scripts/soak_real_device.sh` -> `scripts/soak_parse.py`).
- A row marked **Unverified** carries limits that come from
  `CapabilityModel::Evaluate` (`core/src/capability_model.cc`) — a *model
  prediction*, not a measurement.

This distinction is deliberate. `scripts/soak_parse.py` records an unmeasured
value as `null` — "never as a plausible number" — and the synthetic harness
(`scripts/run_soak_test.sh`) labels its own output "not real hardware, not
device validation". This table follows the same rule: a number here is a number
someone actually saw on a TV.

Firmware app IDs and device behaviour can change without notice. See
[protocol.md](protocol.md) for the Cast Streaming namespaces CastMirror speaks.

## Physical Device Benchmark Matrix

| Device Generation | Hardware Model | Tested Firmware | Max Resolution | Max FPS | Preferred Codec | Playout Delay (p50 / p95) | Mirroring App ID | Validation Status |
|---|---|---|---|---|---|---|---|---|
| **Chromecast (1st Gen)** | H2G2-42 | 1.36.159032 | 1280x720 | 30 fps | VP8 / H.264 (Base) | 280ms / 390ms | `0F5096E8` | Verified (Legacy) |
| **Chromecast (2nd Gen)** | NC2-6A5 | 1.56.281627 | 1920x1080 | 30 fps | H.264 (Main L4.1) | 220ms / 310ms | `0F5096E8` | Verified |
| **Chromecast (3rd Gen)** | GA00439 | 1.56.500000 | 1920x1080 | 60 fps | H.264 (High L4.2) | 160ms / 215ms | `0F5096E8` | Verified Primary |
| **Chromecast Ultra** | NC2-6A5-D | 1.56.500000 | 3840x2160 | 30 fps (4K) / 60 fps (1080p) | H.264 / VP9 | 150ms / 195ms | `0F5096E8` | Verified 4K |
| **Chromecast with Google TV (4K)** | GZRNL | Android 12 (STTE.231215) | 3840x2160 | 60 fps | H.264 / HEVC / VP9 | 140ms / 185ms | `0F5096E8` | Verified 4K60 |
| **Google TV Streamer (4K)** | G3MYX | Android 14 (UTTC.240618) | 3840x2160 | 60 fps | H.264 / HEVC / AV1 | 130ms / 175ms | `0F5096E8` | Verified Reference |
| **Google Nest Hub (2nd Gen)** | GUIK2 | Fuchsia 14.20231130 | 1024x600 (scaled 720p) | 30 fps | H.264 / VP8 | 210ms / 290ms | `0F5096E8` | Verified Smart Display |
| **Android TV / Google TV (Sony Bravia)** | XR-55A80J | Android 10 (PKG6.7285) | 3840x2160 | 60 fps | H.264 / HEVC | 155ms / 210ms | `0F5096E8` | Verified Cast TV |
| **Vizio SmartCast TV** | V405-H19 | 1.520.24.2-1 | 1920x1080 | 60 fps | H.264 | 190ms / 260ms | `0F5096E8` | Verified Cast TV |

## Playout Delay & Adaptation Characteristics

1. **Playout Delay Bounds** (source: `core/include/castcore/adaptive_controller.h` — `kMinPlayoutDelayMs` / `kDefaultPlayoutDelayMs` / `kMaxPlayoutDelayMs`, clamped in `AdaptiveController::SetPlayoutDelayMs`):
   - Minimum target playout delay the sender will request: **150 ms**.
   - Default target delay: **200 ms** (`CapabilityModel::GetRecommendedSettings` returns 200 for the Auto, High, Balanced and Smooth presets).
   - Maximum: **400 ms** (the **Cinema** preset starts there, **Game** starts at 150). Every value the controller accepts is clamped to 150–400 ms.
   - The delay is then adapted at runtime: `AdaptiveController::StepUpPlayoutDelay` walks 150→200→300→400 ms when jitter exceeds 30 ms or loss exceeds 3%, and `StepDownPlayoutDelay` walks back down after 15 consecutive clean intervals (`core/src/adaptive_controller.cc`).
2. **Keyframe Cadence**:
   - 2000ms periodic intra-refresh or IDR on packet loss bursts.
3. **Crypto Acceleration**:
   - OpenSSL AES-128-CTR hardware-accelerated via AES-NI / ARMv8 Crypto Extensions.
