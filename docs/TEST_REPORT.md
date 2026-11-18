# CastMirror Test & Verification Report

**Date:** 2026-11-18  
**Target:** CastMirror v1.0.4 (`castcore` + GTK 4 GUI + WinUI 3 GUI + CLI + Tools)  
**Test Suite:** Google Test + CTest (<!-- testcount -->271<!-- /testcount --> cases) + WinUI xUnit (`tests/winui/`) + libFuzzer (`tests/fuzz/`) + CI Sanitizers & Soak Gates  

---

## 1. Executive Summary

All phases of the CastMirror implementation plan defined in `ARCHITECTURE.md` have been audited against `v1.0.4`:
- **Unit & Integration Tests:** <!-- testcount -->271<!-- /testcount --> test cases, counted from the `TEST(`/`TEST_F(`/`TEST_P(` macros in the sources registered in `tests/CMakeLists.txt`. The per-suite table below is verified by `python3 scripts/count_tests.py --check`; run `ctest --test-dir <build> --output-on-failure` for the live pass/fail list.
  - **Registered vs. counted is not the same number, by design.** `count_tests.py` counts macros across all platform files in `TEST_SOURCES`; `ctest` reports the cases `gtest_discover_tests` actually compiles and registers on *this* platform. `tests/CMakeLists.txt` removes `test_portal_source.cc` (3 Wayland portal cases) on Windows and `test_wgc_window_source.cc` (5 WGC cases) on non-Windows platforms, and `#ifdef _WIN32` / `#if !defined(_WIN32)` guards select platform-specific cases inside shared files (on Linux, **255** cases register under `ctest --test-dir build-swarm -N`, where `WindowCaptureTest.WindowsOnly` reports `Skipped`). Skipped platform cases are passes, not failures.
- **Latency Budget:** (historical lab measurements from `docs/bench/baseline.csv` — see Section 3 below)
  - 1080p60 Video Encode Latency: **9.74 ms - 12.48 ms** (Budget <= 16.6 ms) -> **PASSED (baseline)**
  - Playout Teardown & Stop Latency: **93.6 ms - 121.6 ms** (Budget <= 500 ms) -> **PASSED (baseline)**
  - Session Start to Negotiated Streaming: **< 100 ms** (Budget <= 8.0 s) -> **PASSED (baseline)**
- **Protocol Compliance:**
  - Cast V2 TLS 1.2 port 8009 with OpenSSL 3.x + DeviceAuth CA validation -> **VERIFIED**
  - JSON OFFER/ANSWER negotiation with AES-128-CTR key exchange -> **VERIFIED**
  - Cast RTP packetization (12-byte RTP + 7-byte Cast header + optional 4-byte playout delay extension) and MTU fragmentation (`max_packet_size=1460`) -> **VERIFIED**
  - Compound RTCP `CAST` / `CST2` ACK parsing and `PLI` (`PT 206, FMT 1`) IDR generation -> **VERIFIED**
  - 8-Rung Dynamic Adaptive Ladder Controller -> **VERIFIED**
- **Media, Capture & Transport Verification:**
  - X11 Screen & Window Capture (`X11DisplayCapture`, XRandR / XComposite / XDamage / XFixes) -> **VERIFIED**
  - Wayland ScreenCast Portal Integration (`PipeWirePortalCapture`, DMA-BUF NV12 + `mmap`) -> **VERIFIED**
  - Windows DXGI Desktop Duplication + `WgcWindowSource` + WASAPI per-process audio (`tests/winui/` + native WGC tests) -> **VERIFIED**
  - Hardware H.264 (`h264_vaapi` / `MediaFoundationVideoEncoder`) with `libx264` / `libopenh264` fallback -> **VERIFIED**
  - Multi-slice encoding & periodic intra-refresh support -> **VERIFIED**
  - PulseAudio / PipeWire & WASAPI loopback + UDP transport pacing (`10 pkts / 2 ms`) and 80 ms duplicate NACK suppression -> **VERIFIED**

---

## 1a. Historical Windows release-readiness baseline (v1.0.0)

Recorded during the initial Windows client release-readiness pass (MSYS2 UCRT64 GCC, Windows 11, real display, a real audio endpoint, no hardware H.264 MFT on the test machine):

| Suite | Result |
|---|---|
| Native (`ctest`, `build\`) | 192 of 192 passed, 2 X11-only cases skipped |
| Client unit tests (`dotnet test tests\winui`) | 18 of 18 passed |
| Per-app audio (`scripts\verify_process_audio.ps1`) | passed: a process playing a tone is audible, a silent process is not |
| Window capture (`WindowCaptureTest`) | passed against real windows: a covered window is still captured in its own colour; closing it ends the capture |
| UI smoke test (`scripts\smoke_winui.ps1`) | passed: main window, Settings, Logs and About open, a second launch wakes the first, no new exceptions in the error log |
| Packaged app (`package.ps1`) | passed its checks; the published `CastMirror.exe` starts with MSYS2 off `PATH` |

What this run did not cover, and why it matters:

- **Hardware H.264 encoders.** The Media Foundation path for asynchronous GPU encoders (NVIDIA, AMD, Intel) is written to Microsoft's contract and unit-neutral here; this machine has no hardware MFT, so the software fallback ran. It needs a run on each vendor's hardware.
- **HDR and non-BGRA surfaces.** Microsoft documents desktop duplication as BGRA only, so the FP16, 10-bit and RGBA conversions are defensive and tested on synthetic pixels only.
- **A live cast.** The end-to-end tests use the simulated receiver in `tests/test_cast_e2e.cc`. No session was started against a physical Chromecast in this pass.
- **Long sessions.** No 8-hour soak.
- **Sanitizers.** ThreadSanitizer and AddressSanitizer are not available for the MinGW toolchain, so the threading fixes are covered by regression tests, not by a sanitizer run.

## 2. Detailed Test Results

<!-- testsuite:start -->
| Test source | Cases | Coverage |
|---|---|---|
| `test_state_machine.cc` | 6 | State transitions, callback notification, retry limits |
| `test_config.cc` | 4 | Defaults, save/load round-trip |
| `test_capability_model.cc` | 5 | Model classification, preset recommendations, capture-fps passthrough |
| `test_offer_answer.cc` | 8 | OFFER JSON, ANSWER parsing, custom audio bitrate, status query |
| `test_crypto.cc` | 2 | AES-128-CTR round-trip, per-frame ciphertext variance |
| `test_rtp_rtcp.cc` | 14 | 7-byte Cast RTP header, MTU slicing, CAST/CST2 feedback, checkpoint expansion |
| `test_adaptive.cc` | 22 | Loss downshifts, RTT/jitter delay scaling, bitrate ramp-up, cooldown |
| `test_encoders.cc` | 21 | H.264 Annex-B output, Reconfigure, multi-slice, intra-refresh, Opus cadence |
| `test_cast_e2e.cc` | 16 | Full TLS + OFFER/ANSWER + media streaming + Stop against the simulated receiver |
| `test_device_selection.cc` | 2 | Last-device resolution by id then IP, display index |
| `test_logger.cc` | 4 | Severity filter, callback delivery |
| `test_display_capture.cc` | 14 | Synthetic/X11 enumerate and start, DMA-BUF metadata, WGC pacing (`_WIN32` only) |
| `test_frame_pacer.cc` | 15 | Frame pacing cadence |
| `test_media_timeline.cc` | 4 | MediaTimeline tick stepping, due-frame math, reset handling, 48 kHz audio RTP ticks |
| `test_av_clock_drift.cc` | 1 | Long-run A/V clock drift bound under jitter and suspend |
| `test_session_recovery.cc` | 3 | Recovery state and the 30 s timeout ceiling |
| `test_cast_transport.cc` | 12 | RTCP source filter, rolling FPS, duplicate-NACK suppression, pacing |
| `test_capture_source.cc` | 9 | CaptureSource kind round-trip, config v2 to v3 migration, legacy Start forwarding |
| `test_portal_source.cc` | 3 | Portal source-type mapping (Linux only) |
| `test_source_selection.cc` | 5 | Window support reporting, geometry, config persistence |
| `test_device_auth.cc` | 7 | Device-auth challenge/response |
| `test_c_api.cc` | 12 | C ABI structs, string truncation and NUL-termination contract |
| `test_device_discovery.cc` | 27 | mDNS TXT parsing, device list, subnet probe |
| `test_net_platform.cc` | 7 | Interface enumeration, socket init idempotence, error classification |
| `test_gpu_processor.cc` | 7 | Letterbox geometry, YUV/NV12 conversion correctness, invalid args |
| `test_pixel_convert.cc` | 8 | Half-float, BGRA/RGBA, RGB10A2 unpacking, HDR-to-SDR tone mapping |
| `test_parser_robustness.cc` | 4 | Fuzz-corpus and mutation robustness for RTCP and mDNS TXT parsers |
| `test_audio_sessions.cc` | 5 | Per-process audio session enumeration and WASAPI loopback isolation |
| `test_window_capture.cc` | 5 | Window capture isolation, occlusion rejection, desktop-crop fallback, close handling |
| `test_audio_capture.cc` | 14 | 10 ms cadence, monotonic timestamps, join-or-detach worker cleanup |
| `test_wgc_window_source.cc` | 5 | WGC content-instant tracking, missing/negative instant fallback, drift prevention (_WIN32 only) |
| **Total** | <!-- testcount -->271<!-- /testcount --> | 31 files registered in `tests/CMakeLists.txt` |
<!-- testsuite:end -->

---

## 2b. CI, Fuzzers, Sanitizers & Quality Gates (v1.0.4)

Defined in `.github/workflows/ci.yml` and `.github/workflows/cast-app-id-drift.yml`:

| Gate / Job | Scope & Tooling | Target / Script |
|---|---|---|
| **WinUI 3 xUnit Suite** | C# localization & service tests in `tests/winui/CastMirror.Tests.csproj` (`LocalizationTests.cs`: 9 `[Fact]` + 1 `[Theory]` w/ 4 `[InlineData]`; `ServiceTests.cs`: 15 `[Fact]` + 2 `[Theory]` w/ 8 `[InlineData]`) | `dotnet test tests/winui/CastMirror.Tests.csproj` (`windows` CI job) |
| **libFuzzer Harnesses** | Dedicated fuzz targets (`CASTMIRROR_BUILD_FUZZERS=ON` in `tests/fuzz/CMakeLists.txt`) | `fuzz_rtcp_parser` (`tests/fuzz/fuzz_rtcp_parser.cc`), `fuzz_mdns_parser` (`tests/fuzz/fuzz_mdns_parser.cc`) |
| **Linux Sanitizers** | Memory, undefined behavior, and thread race checks (`tests/tsan_suppressions.txt`) | `Ubuntu ASan / UBSan Debug`, `Ubuntu TSan Debug` |
| **Windows UBSan** | Clang UBSan in trap mode | `scripts/run_ubsan.ps1` |
| **Code Coverage** | Line coverage floor across `core/` | `gcovr --filter 'core/' --fail-under-line 55` |
| **Fedora Codec Fallback** | Container build validating `ffmpeg-free-devel` + `libopenh264` software fallback | `Fedora Latest Container` CI job |
| **Formatting & Lint** | Formatting, static analysis, and tray lint | `clang-format==22.1.8`, `clang-tidy` (incl. `app/gui/tray.cc` on `Ubuntu 26.04`) |
| **Accessibility Audit** | 12 automated GTK 4 / WinUI accessibility surface checks | `scripts/audit_a11y.sh` |
| **License & Drift Hygiene** | License header/surface scan, script syntax, doc test/matrix markers, and weekly Cast app ID check | `scripts/check_license_surface.sh`, `scripts/count_tests.py --check`, `scripts/render_device_matrix.sh --check`, `scripts/fetch_cast_appids.sh --check` |
| **Fixture & Windows Soaks** | Automated fixture soak & Windows WGC/MF leak regression soak | `scripts/run_soak_test.sh --fixture`, `scripts/soak_windows.ps1` |

---

## 3. Historical Benchmark Measurements (`docs/bench/baseline.csv`)

Historical lab baseline measurements recorded in `docs/bench/baseline.csv` (for example commit `449a7c5`: `12.480 ms` p50 encode, `56.33 fps`, `121.617 ms` stop on `h264_vaapi` `Synthetic`):

### Media Capture & Encode Pipeline (`tools/poc-encode`)
- **Video Format:** 1920x1080 @ 60 FPS
- **Video Encoder:** Hardware VAAPI (`h264_vaapi`) with fallback to FFmpeg `libx264` (`superfast`, `zerolatency`, 0 B-frames, Annex-B NALUs)
- **Audio Encoder:** Opus 48000 Hz, 2 channels, 10 ms frame packets
- **Measured Frame Rate:** 56.3 - 57.7 FPS
- **Average Encode Latency:** 9.75 ms - 12.48 ms
- **Audio Output:** 100 packets/sec (exact 10 ms cadence)

### End-to-End Join & Streaming (`tools/poc-join`)
- **Control Handshake:** TLS 8009 connection + `LAUNCH` `0F5096E8` -> ~12 ms
- **OFFER/ANSWER Negotiation:** ~3 ms
- **Media Delivery:** UDP Cast RTP/RTCP transport active with pacing and NACK retransmission
- **Clean Session Teardown:** ~93.6 ms - 121.6 ms (Budget <= 500 ms)

## 4. Windows Memory & Handle Soak (10 minutes, 1080p desktop to in-process receiver)

The first run showed the working set climbing about 110 MB a minute with handles flat. Cause: the Media Foundation encoder took an extra reference on each sample the MFT allocated itself, so one sample leaked per frame. After the fix a 10-minute run stays within 12 MB of baseline (117 to 129 MB) and 6 handles. Run it with `scripts/soak_windows.ps1`; use `-Minutes 480` for a release soak.

