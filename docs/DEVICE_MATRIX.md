# CastMirror Physical Hardware Compatibility & Device Matrix

This matrix documents real-hardware validation, empirical performance boundaries, firmware targets, and validated streaming parameters across Google Cast hardware generations.

> **Status for 1.0.4.** No real-device soak runs have been recorded in `docs/bench/device_runs/` yet, so every row in the generated matrix below reads `_not measured_`. Releases through 1.0.4 were validated against a simulated receiver, not against physical hardware. Run `scripts/soak_real_device.sh` (which runs `scripts/fetch_cast_appids.sh --check` preflight before driving a physical device) and regenerate with `scripts/render_device_matrix.sh` to populate it.

## How to read this table

Two different kinds of claim appear in CastMirror's documentation, and they are **not** equally strong:

- **Recorded run metrics** (`Firmware / app version`, `Glass-to-glass`, throughput, loss, NACK/PLI) are *measurements*, not predictions. They come exclusively from real-device soak runs under `docs/bench/device_runs/` (`scripts/soak_real_device.sh` -> `scripts/soak_parse.py` -> `scripts/render_device_matrix.sh`).
- **Device capability limits** in [COMPATIBILITY.md](COMPATIBILITY.md) come from `CapabilityModel::Evaluate` (`core/src/capability_model.cc`) — a *model prediction*, not a measurement.

This distinction is deliberate. `scripts/soak_parse.py` records an unmeasured value as `null` — never as a plausible number — `scripts/render_device_matrix.py` renders `null` as `not measured`, and the synthetic harness (`scripts/run_soak_test.sh`) keeps its records in a separate, loudly-labelled section ("NOT real-hardware evidence").

Firmware app IDs and device behaviour can change without notice. See
[protocol.md](protocol.md) for the Cast Streaming namespaces CastMirror speaks.

<!-- BEGIN GENERATED:DEVICE_MATRIX -->
<!-- This table is GENERATED. Do not hand-edit it. -->
<!-- Regenerate with: scripts/render_device_matrix.sh -->

## Physical Device Benchmark Matrix

Every value below is copied from a recorded run in `docs/bench/device_runs/`.
A cell reading `not measured` means exactly that: no run has been recorded
for it. CastMirror does **not** synthesise plausible numbers here.

> **No real-device runs have been recorded.** The CastMirror repository
> contains no `castmirror.device_soak.run.v1` record with
> `"synthetic": false`, so the real-device matrix is empty. Run
> `scripts/soak_real_device.sh` against physical hardware to populate it.

| Device | `md` | Firmware / app version | Wi-Fi band | Preset | Target bitrate | targetDelay | Capture source | Duration | TTFF | Glass-to-glass | Peak bitrate | Avg bitrate | Packet loss (peak) | NACK | PLI |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Chromecast (1st Gen) | `H2G2-42` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Chromecast (2nd Gen) | `NC2-6A5` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Chromecast (3rd Gen) | `GA00439` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Chromecast Ultra | `NC2-6A5-D` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Chromecast with Google TV (4K) | `GZRNL` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Google TV Streamer (4K) | `G3MYX` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Google Nest Hub (2nd Gen) | `GUIK2` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Android TV / Google TV (Sony Bravia) | `XR-55A80J` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |
| Vizio SmartCast TV | `V405-H19` | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ | _not measured_ |

### Synthetic / harness self-test runs

_None recorded._
<!-- END GENERATED:DEVICE_MATRIX -->

## Playout Delay & Adaptation Characteristics

1. **Playout Delay Bounds** (source: `core/include/castcore/adaptive_controller.h` — `kMinPlayoutDelayMs` / `kDefaultPlayoutDelayMs` / `kMaxPlayoutDelayMs`, clamped in `AdaptiveController::SetPlayoutDelayMs`):
   - Minimum target playout delay the sender will request: **150 ms**.
   - Default target delay: **200 ms** (`CapabilityModel::GetRecommendedSettings` returns 200 for the Auto, High, Balanced and Smooth presets).
   - Maximum: **400 ms** (the **Cinema** preset starts there, **Game** starts at 150). Every value the controller accepts is clamped to 150–400 ms.
   - The delay is then adapted at runtime: `AdaptiveController::StepUpPlayoutDelay` walks 150→200→300→400 ms when jitter exceeds 30 ms, loss exceeds 3%, or on a NACK burst / PLI, and `StepDownPlayoutDelay` walks back down after 15 consecutive clean intervals (`core/src/adaptive_controller.cc`).
2. **Keyframe Cadence (`core/src/video_encoder.cc`):**
   - Software H.264 defaults to periodic intra-refresh (`intra_refresh = true`, `gop_size = 10000`; or `framerate * 5` when intra-refresh is disabled), forcing immediate IDR keyframes on frame 0, RTCP PLI, `Reconfigure`, `SetLiveVideoBitrateKbps`, and >400 ms video stalls.
3. **Crypto Acceleration**:
   - OpenSSL AES-128-CTR hardware-accelerated via AES-NI / ARMv8 Crypto Extensions.
