# Building CastMirror

Linux is the supported v1 platform. You need a C++20 compiler, CMake 3.20+, Ninja, and the libraries below.

## Packages (Debian / Ubuntu / Kali)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config protobuf-compiler libprotobuf-dev \
    libssl-dev libopus-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxfixes-dev \
    libva-dev libavcodec-dev libswscale-dev libavutil-dev nlohmann-json3-dev libgtest-dev \
    libgtk-4-dev libadwaita-1-dev
```

`libgtk-4-dev` and `libadwaita-1-dev` are required for `castmirror-gui`. The CLI (`castmirror`) links only `castcore`.

## Configure and build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Binaries:

| Path | Role |
|---|---|
| `build/app/castmirror-gui` | GTK 4 + libadwaita desktop app |
| `build/app/castmirror` | Interactive / flag CLI |
| `build/tests/castmirror_tests` | Google Test suite |
| `build/tools/poc-*` | Protocol and encode benches |
| `build/tools/fake-receiver` | Simulated Cast receiver |

A `.desktop` launcher lives at [`app/io.github.vindeckyy.CastMirror.desktop`](../app/io.github.vindeckyy.CastMirror.desktop).

## Tests

```bash
cd build
ctest --output-on-failure
# or
./tests/castmirror_tests
```

## Run

```bash
./build/app/castmirror-gui
./build/app/castmirror
./build/app/castmirror --device 192.168.1.150 --display 0 --preset High
./build/app/castmirror --device 192.168.1.150 --no-audio
```

Config and logs: `~/.config/castmirror/config.json` and `~/.config/castmirror/castmirror.log`.

## Environment variables

- `CASTMIRROR_FORCE_SOFTWARE_ENCODE=1`: Force FFmpeg `libx264` software encoding even on systems where VAAPI hardware encoding is available.
- `CASTMIRROR_FORCE_X11=1`: Force X11 display capture instead of the PipeWire portal when running under a Wayland compositor via XWayland.

## Network

Cast control uses **TCP 8009** (TLS) to the device. Media uses **UDP** to the port in the ANSWER. The sender machine must be on the same LAN. If the picture never appears, allow outbound UDP and inbound RTCP on the host firewall. Helper scripts: `scripts/setup_firewall.sh`.

## Audio

Capture uses the PulseAudio/PipeWire **default sink monitor**. While mirroring with audio on, the default sink is muted so host speakers do not play the same stream; previous mute state is restored on Stop.

## Windows

`app/winui/` is a UI blueprint. There is no supported Windows CI or shipping DXGI/WASAPI GUI in v1.
