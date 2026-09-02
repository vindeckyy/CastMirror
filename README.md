<p align="center">
  <img src="docs/assets/logo.svg" width="72" alt="CastMirror">
</p>

<h1 align="center">CastMirror</h1>

<p align="center">
  Native Chromecast display mirroring.<br>
  Low latency. No Chrome.
</p>

<p align="center">
  <a href="https://github.com/vindeckyy/CastMirror/actions/workflows/ci.yml"><img src="https://github.com/vindeckyy/CastMirror/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/tests-ctest-4caf50" alt="tests">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C" alt="C++20">
  <img src="https://img.shields.io/badge/platform-Linux-0078d4" alt="Linux">
  <img src="https://img.shields.io/badge/license-Apache--2.0-8c93a0" alt="Apache-2.0">
  <img src="https://img.shields.io/badge/latency-~200ms_playout-00d2ff" alt="latency">
</p>

<p align="center">
  <img src="docs/assets/screenshot-ready.png" width="720" alt="CastMirror GTK app">
</p>

## What it is

CastMirror is a native **C++20** sender for Google Cast display mirroring. It discovers devices on the LAN, captures the Linux desktop and system audio, encodes **H.264** (libx264) and **Opus**, and streams **Cast RTP/RTCP** over UDP — the same media path Chrome uses for “Cast screen,” without embedding Chrome.

The shipping UI is a **GTK 4 + libadwaita** desktop app. A CLI is included.

## What it is not

- Not a Chrome / CEF wrapper
- Not Sunshine or Moonlight (those are ~20–50 ms game streams; Cast keeps a **playout delay**, about **200 ms** here)
- Not an HLS/DASH “fling” to the Default Media Receiver
- **Not affiliated with Google.** Chromecast, Google Cast, and Google TV are Google trademarks
- `app/winui/` is a **blueprint**, not the shipping Windows product

Official Cast sender SDKs cannot mirror a desktop. Chrome’s mirroring path is private as a product and open as a protocol. CastMirror implements that protocol as a LAN sender.

## Features

- One-click Cast to Chromecast, Google TV, and Cast TVs (or add a device by IP)
- **Screen or window sharing** — cast an entire monitor or pick a single application window. On Wayland the system portal picker handles selection; on X11 CastMirror enumerates and captures windows directly with XComposite redirection
- Quality presets **Auto / High / Balanced / Smooth** plus a **per-profile bitrate slider** (1–25 Mbps). The slider is locked while connecting or live
- Host speakers **mute** while audio is mirrored; previous mute state is restored on Stop
- Adaptive bitrate from RTCP (CAST checkpoints, NACK / CST2, PLI keyframes)
- AES-128-CTR per-frame media crypto as required by Cast Streaming
- TLS control plane on port **8009**
- Stop budget under **500 ms** (capture does not run except during a live session)

## Quick start (Linux)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config protobuf-compiler libprotobuf-dev \
    libssl-dev libopus-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxfixes-dev \
    libxcomposite-dev libxdamage-dev \
    libavcodec-dev libswscale-dev libavutil-dev nlohmann-json3-dev libgtest-dev \
    libgtk-4-dev libadwaita-1-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

./build/app/castmirror-gui
./build/app/castmirror
```

Full package notes, firewall, and audio behavior: [docs/building.md](docs/building.md).

## Usage

1. Start `castmirror-gui`
2. Pick a Cast device (Rescan or Add IP)
3. Choose what to share — a **Screen** or a **Window** (toggle at the top of the "What to share" section), then pick the specific monitor or window from the list. On Wayland the system portal picker handles selection when you press Cast
4. Choose quality preset and bitrate (defaults are the profile normal until you move the slider)
5. Cast — host speakers stay quiet if audio mirroring is on
6. Stop restores local playback

CLI:

```bash
# Cast a full display
./build/app/castmirror --device 192.168.1.150 --display 0 --preset High

# List available windows, then cast one
./build/app/castmirror --list-windows
./build/app/castmirror --device 192.168.1.150 --window 12345

# Audio-only
./build/app/castmirror --device 192.168.1.150 --no-audio
```

Desktop launcher: [app/io.github.vindeckyy.CastMirror.desktop](app/io.github.vindeckyy.CastMirror.desktop).

## Architecture

```mermaid
flowchart LR
  X11[X11 screen/window capture] --> x264[libx264]
  Portal[Wayland portal+PipeWire] --> x264
  Pulse[Pulse/PipeWire monitor] --> Opus[Opus]
  x264 --> AES[AES-128-CTR]
  Opus --> AES
  AES --> RTP[Cast RTP]
  RTP --> UDP[UDP to device]
  Device[Cast device] -->|RTCP CAST NACK PLI| Adapt[Adaptive controller]
  Adapt --> x264
  TLS[Cast V2 TLS 8009] --- Device
```

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/protocol.md](docs/protocol.md).

## Compatibility

Chromecast 3rd gen, Ultra, Google TV / Streamer, and built-in Cast TVs. **Nest Hub is 720p-class.** Matrix and preset table: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Project layout

```
CastMirror/
├── app/gui/                 # Shipping GTK 4 + libadwaita UI
├── app/cli/                 # Interactive / flag CLI
├── app/winui/               # Windows UI blueprint (not v1 shipping)
├── core/                    # castcore C++20 library
├── tests/                   # Google Test
├── tools/                   # poc-control, poc-streaming, poc-encode, poc-join, fake-receiver
├── docs/                    # Pages site + architecture / protocol / building
└── receiver-fallback/       # CAF research fallback, not the primary path
```

## Development

```bash
cd build && ctest --output-on-failure
./tests/castmirror_tests
./tools/poc-encode
./tools/fake-receiver 28009 53533
```

See [CONTRIBUTING.md](CONTRIBUTING.md). Historical lab notes: [docs/TEST_REPORT.md](docs/TEST_REPORT.md).

## Security

LAN-only. TLS to :8009. Media AES as the protocol requires. No cloud. Report privately: [SECURITY.md](SECURITY.md).

## License

Source in this repository is **Apache License 2.0** — see [LICENSE](LICENSE).

**Binaries linked against GPL libx264 generally must be treated as GPL.** Read [NOTICE](NOTICE) before you distribute builds.

## Disclaimer

CastMirror is an independent project. It is not affiliated with, endorsed by, or sponsored by Google LLC. It speaks a Chromium-compatible Cast Streaming protocol; firmware app IDs can change.
