# Building CastMirror

The Windows app is the primary product, so its steps come first. The Linux build follows.

## Windows

### What you need

| Tool | Version | Notes |
|---|---|---|
| Windows | 10 2004 or later, 64-bit | Windows 11 is fine |
| [MSYS2](https://www.msys2.org/) | current | UCRT64 environment, installed to `C:\msys64` |
| .NET SDK | 8.0 | `winget install Microsoft.DotNet.SDK.8` |
| Inno Setup | 6 | Only to build the installer |

Install the native dependencies from an MSYS2 UCRT64 shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,protobuf,openssl,opus,ffmpeg,nlohmann-json,gtest,libx264}
```

### Build and run

```bat
build-all.bat
```

That configures `build\` on first run, builds `castcore.dll`, the CLI and the tests with the MSYS2 toolchain, then builds the WinUI app with `dotnet build`. `CastMirror.exe` ends up in `app\winui\bin\x64\Debug\net8.0-windows10.0.22621.0\win-x64\` with `castcore.dll` beside it.

The DLL's MinGW dependencies (libstdc++, OpenSSL, FFmpeg and so on) are not copied into the debug output. Either launch from a shell where `C:\msys64\ucrt64\bin` is on `PATH`, or use `run-gui.bat`, which sets it. The packaged build (below) carries them.

To use a different MSYS2 or .NET location, set `CASTMIRROR_MSYS2_BIN` and `CASTMIRROR_DOTNET_ROOT`.

### Tests

```bat
ctest --test-dir build --output-on-failure
dotnet test tests\winui\CastMirror.Tests.csproj
```

The native suite has about 200 cases. Cases that need an interactive desktop skip themselves on a runner that has none. The .NET tests cover the client's pure logic (update check, diagnostics bundle, crash-file handling) and run anywhere.

`CASTMIRROR_ALLOW_SYNTHETIC_CAPTURE=1` lets a session use generated video and audio when real capture is unavailable. The test binary sets it. Real sessions never fall back to it.

### Sanitizers and fuzzing

MinGW has no AddressSanitizer or ThreadSanitizer runtime, and libFuzzer does not link on it. Two things fill the gap:

- `scripts\run_ubsan.ps1` builds the suite with clang's UndefinedBehaviorSanitizer in trap mode (needs `pacman -S mingw-w64-ucrt-x86_64-clang`) and runs it. CI runs it as the `windows-ubsan` job.
- `tests\test_parser_robustness.cc` mutates and generates RTCP and mDNS packets with a fixed seed and feeds them to the parsers on every platform. The libFuzzer harnesses in `tests\fuzz` still run on Linux CI.

### Package and installer

```bat
package.ps1
```

It builds, runs the whole native suite, publishes a self-contained x64 build to `publish\`, copies the MinGW runtime DLLs beside it, adds the licence files, removes debug symbols and unused language folders, and runs `scripts\check_windows_package.ps1`. The check fails on a missing licence file, leftover symbols, a package over 400 MB, or a libx264 build without the GPL notice.

Build the installer from that folder:

```bat
iscc installer\CastMirror.iss /DAppVersion=1.0.0
```

The result is `dist\CastMirror-Setup-1.0.0-x64.exe`. Release builds run the same steps in `.github/workflows/release-windows.yml`, and sign `CastMirror.exe`, `castcore.dll` and the installer when the `SIGN_CERT_BASE64` and `SIGN_CERT_PASSWORD` repository secrets exist. Without them the artifacts are unsigned and SmartScreen will warn.

### Cutting a release

1. Set the same version in `CMakeLists.txt` (`project(CastMirror VERSION ...)`) and `app\winui\CastMirrorApp.csproj` (`<Version>`, `<AssemblyVersion>`, `<FileVersion>`).
2. `scripts\check_version.ps1` confirms they agree.
3. Move the Unreleased notes in `CHANGELOG.md` under the new version.
4. Add `docs/release-notes/windows-X.Y.Z.md` and `docs/release-notes/linux-X.Y.Z.md`. Each becomes the description of its GitHub release.
5. Windows and Linux release separately, each from its own tag. Push `windows-vX.Y.Z` to run `release-windows.yml` (installer, zip, checksums) and `linux-vX.Y.Z` to run `release-linux.yml` (.deb, .rpm, checksums). Each workflow refuses a tag that doesn't match the version files, and fails if its notes file is missing.

### Files and folders

| Path | Contents |
|---|---|
| `%APPDATA%\CastMirror\config.json` | Settings |
| `%APPDATA%\CastMirror\castmirror.log` | Engine log |
| `%APPDATA%\CastMirror\gui-errors.log` | App log |
| `%APPDATA%\CastMirror\crashes\` | Crash minidumps, newest five |

`CASTMIRROR_CONFIG_DIR` redirects the config file. The test binary uses it so a test run never touches your settings.

### Firewall

The installer allows `CastMirror.exe` on private and domain networks in both directions. For a portable copy, run `scripts\setup_firewall.ps1 -Program <path to CastMirror.exe>` from an elevated PowerShell, and `-Remove` to undo it. Public networks stay closed on purpose.

## Linux

Linux builds exist but get fixes rather than features. You need a C++20 compiler, CMake 3.20+, Ninja, and the libraries below.

## Packages (Debian / Ubuntu / Kali)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config protobuf-compiler libprotobuf-dev \
    libssl-dev libopus-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxfixes-dev \
    libxcomposite-dev libxdamage-dev \
    libva-dev libdrm-dev libavcodec-dev libswscale-dev libavutil-dev nlohmann-json3-dev libgtest-dev \
    libgtk-4-dev libadwaita-1-dev
```

`libgtk-4-dev` and `libadwaita-1-dev` are required for `castmirror-gui`. The CLI (`castmirror`) links only `castcore`.

`libxcomposite-dev` and `libxdamage-dev` are optional but recommended — they enable occluded-window capture and efficient damage tracking for X11 window sharing. The build succeeds without them (window capture degrades gracefully).

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
- `CASTMIRROR_CONFIG_DIR=/path/to/dir`: Read and write `config.json` in this directory instead of the default per-user location (used by the test binary to keep test runs off your real settings).

## Network

Cast control uses **TCP 8009** (TLS) to the device. Media uses **UDP** to the port in the ANSWER. The sender machine must be on the same LAN. If the picture never appears, allow outbound UDP and inbound RTCP on the host firewall. Helper scripts: `scripts/setup_firewall.sh`.

## Audio

Capture uses the PulseAudio/PipeWire **default sink monitor**. While mirroring with audio on, the default sink is muted so host speakers do not play the same stream; previous mute state is restored on Stop.
