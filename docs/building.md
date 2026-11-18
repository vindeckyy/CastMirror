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

That configures `build\` on first run (with `-DCASTMIRROR_BUILD_GUI=OFF` defaulted on Windows), builds `castcore.dll`, the CLI (`castmirror.exe`) and the tests with the MSYS2 toolchain, then builds the WinUI app with `dotnet build`. `CastMirror.exe` ends up in `app\winui\bin\x64\Debug\net8.0-windows10.0.22621.0\win-x64\` with `castcore.dll` beside it.

During the WinUI build, `app\winui\CastMirrorApp.csproj` runs `scripts\copy_native_deps.ps1` so `CastMirror.exe` carries `castcore.dll`'s MinGW runtime dependencies (libstdc++, OpenSSL, FFmpeg and so on) in its output directory. For the standalone native binaries (`build\app\castmirror.exe`, `build\tests\castmirror_tests.exe`), launch from a shell where `C:\msys64\ucrt64\bin` is on `PATH`, or use `run-gui.bat`, `run-cli.bat`, and `run-tests.bat`, which set it.

To use a different MSYS2 or .NET location, set `CASTMIRROR_MSYS2_BIN` and `CASTMIRROR_DOTNET_ROOT`.

### Tests

```bat
ctest --test-dir build --output-on-failure
dotnet test tests\winui\CastMirror.Tests.csproj
```

The native suite defines 271 cases across 31 source files in `tests/CMakeLists.txt` (on Windows, `test_portal_source.cc` is excluded because it is Linux-only, and cases that need an interactive desktop skip themselves on a runner that has none). The .NET tests cover the client's pure logic (update check, diagnostics bundle, crash-file handling, settings DTO, and translation parity across `en`/`es`/`de`/`fr`) and run anywhere.

`CASTMIRROR_ALLOW_SYNTHETIC_CAPTURE=1` lets a session use generated video and audio when real capture is unavailable. The test binary sets it. Real sessions never fall back to it.

### Sanitizers and fuzzing

MinGW has no AddressSanitizer or ThreadSanitizer runtime, and libFuzzer does not link on it. Two things fill the gap:

- `scripts\run_ubsan.ps1` builds the suite with clang's UndefinedBehaviorSanitizer in trap mode (needs `pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-compiler-rt`) and runs it. CI runs it as the `windows-ubsan` job.
- `tests\test_parser_robustness.cc` mutates and generates RTCP and mDNS packets with a fixed seed and feeds them to the parsers on every platform. The libFuzzer harnesses in `tests\fuzz` run on Linux CI.

### Package and installer

```bat
package.ps1
```

It builds, runs the whole native suite, publishes a self-contained x64 build to `publish\`, copies the MinGW runtime DLLs beside it, adds the licence files, removes debug symbols and unused language folders, and runs `scripts\check_windows_package.ps1`. The check fails on a missing licence file, leftover symbols, a package over 400 MB, or a libx264 build without the GPL notice.

Build the installer from that folder:

```bat
iscc installer\CastMirror.iss /DAppVersion=1.0.4
```

The result is `dist\CastMirror-Setup-1.0.4-x64.exe`. Release builds run the same steps in `.github/workflows/release-windows.yml`, and sign `CastMirror.exe`, `castcore.dll` and the installer when the `SIGN_CERT_BASE64` and `SIGN_CERT_PASSWORD` repository secrets exist. Without them the artifacts are unsigned and SmartScreen will warn.

### Cutting a release

1. Set the same version in `CMakeLists.txt` (`project(CastMirror VERSION ...)`) and `app\winui\CastMirrorApp.csproj` (`<Version>`, `<AssemblyVersion>`, `<FileVersion>`).
2. `scripts\check_version.ps1` confirms they agree.
3. Move the Unreleased notes in `CHANGELOG.md` under the new version (and update `app\io.github.vindeckyy.CastMirror.metainfo.xml`).
4. Add `docs/release-notes/windows-X.Y.Z.md` and `docs/release-notes/linux-X.Y.Z.md`. Each becomes the description of its GitHub release.
5. Windows and Linux release separately, each from its own tag. Push `windows-vX.Y.Z` to run `release-windows.yml` (installer, zip, checksums) and `linux-vX.Y.Z` to run `release-linux.yml` (.deb, .rpm, checksums). Each workflow refuses a tag that doesn't match the version files, and fails if its notes file is missing.

### Files and folders

| Path | Contents |
|---|---|
| `%APPDATA%\CastMirror\config.json` | Settings |
| `%APPDATA%\CastMirror\castmirror.log` | Rotating engine log |
| `%APPDATA%\CastMirror\castmirror-session.log` | Per-session debug log |
| `%APPDATA%\CastMirror\castmirror.ndjson` | Verbose JSON diagnostics (when `CASTMIRROR_VERBOSE_JSON=1` or `verbose_json_logging` is enabled) |
| `%APPDATA%\CastMirror\gui-errors.log` | App log |
| `%APPDATA%\CastMirror\crashes\` | Crash minidumps, newest five |

`CASTMIRROR_CONFIG_DIR` redirects the config and log files. The test binary uses it so a test run never touches your settings.

### Firewall

The installer allows `CastMirror.exe` on private and domain networks in both directions. For a portable copy, run `scripts\setup_firewall.ps1 -Program <path to CastMirror.exe>` from an elevated PowerShell, and `-Remove` to undo it. Public networks stay closed on purpose.

## Linux

Linux builds exist but get fixes rather than features. You need a C++20 compiler, CMake 3.20+, Ninja, and the libraries below.

### Packages (Debian / Ubuntu / Kali)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config protobuf-compiler libprotobuf-dev \
    libssl-dev libopus-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxfixes-dev \
    libxcomposite-dev libxdamage-dev \
    libva-dev libdrm-dev libpipewire-0.3-dev libglib2.0-dev libavcodec-dev libswscale-dev libavutil-dev \
    nlohmann-json3-dev libgtest-dev libgtk-4-dev libadwaita-1-dev
```

`libgtk-4-dev` and `libadwaita-1-dev` are required for `castmirror-gui` (GTK >= 4.12, libadwaita >= 1.5). The CLI (`castmirror`) links only `castcore` and can be built without GTK via `-DCASTMIRROR_BUILD_GUI=OFF`.

`libpipewire-0.3-dev` and `libglib2.0-dev` enable Wayland capture via the `xdg-desktop-portal` ScreenCast API over PipeWire. `libxcomposite-dev` and `libxdamage-dev` are optional but recommended — they enable occluded-window capture and efficient damage tracking for X11 window sharing. `libayatana-appindicator-glib-dev` (available on Debian Testing / 13+ and Ubuntu 25.04+) is optional and enables the system tray icon; pass `-DCASTMIRROR_REQUIRE_TRAY=ON` to fail configuration if it is missing. The build succeeds without optional packages.

On Ubuntu 24.04, `libgtest-dev` installs source under `/usr/src/googletest` without prebuilt CMake targets; if `find_package(GTest)` fails, either build and install `/usr/src/googletest` or configure with `-DBUILD_TESTING=OFF`.

On Fedora, install the equivalent `-devel` packages (`gcc-c++ cmake ninja-build pkgconf-pkg-config protobuf-compiler protobuf-devel openssl-devel opus-devel pulseaudio-libs-devel libX11-devel libXext-devel libXrandr-devel libXfixes-devel libXcomposite-devel libXdamage-devel libva-devel libdrm-devel pipewire-devel glib2-devel ffmpeg-free-devel json-devel gtest-devel gtk4-devel libadwaita-devel`).

### Configure and build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

#### CMake options

| Option | Default | Effect |
|---|---|---|
| `BUILD_TESTING` | `ON` | Build the Google Test suite under `tests/` (`OFF` skips GTest, e.g. for Flatpak) |
| `CASTMIRROR_BUILD_GUI` | `ON` on Linux, `OFF` on Windows | Build the GTK 4 + libadwaita desktop app (`castmirror-gui`). Explicitly setting `ON` on Windows fails configuration. |
| `CASTMIRROR_ENABLE_TRAY` | `ON` | Build Ayatana AppIndicator system tray support when `libayatana-appindicator-glib-dev` is found |
| `CASTMIRROR_REQUIRE_TRAY` | `OFF` | Fail configuration if `CASTMIRROR_BUILD_GUI=ON` and `CASTMIRROR_ENABLE_TRAY=ON` but the Ayatana AppIndicator library is missing |
| `CASTMIRROR_BUILD_FUZZERS` | `OFF` | Build `tests/fuzz/` libFuzzer harnesses (`fuzz_rtcp_parser`, `fuzz_mdns_parser`); requires Clang (`-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`) |
| `CASTMIRROR_PACKAGE_DISTRO_SUFFIX` | `""` | Insert a distro tag into CPack `.deb`/`.rpm` filenames (e.g. `-DCASTMIRROR_PACKAGE_DISTRO_SUFFIX=debian13`) |

Binaries:

| Path | Role |
|---|---|
| `build/app/castmirror-gui` | GTK 4 + libadwaita desktop app |
| `build/app/castmirror` | Interactive / flag CLI |
| `build/tests/castmirror_tests` | Google Test suite (when `BUILD_TESTING=ON`) |
| `build/tools/poc-*` | Protocol and encode benches (`poc-control`, `poc-streaming`, `poc-encode`, `poc-join`) |
| `build/tools/fake-receiver` | Simulated Cast receiver |
| `build/tests/fuzz/fuzz_*` | libFuzzer harnesses (`fuzz_rtcp_parser`, `fuzz_mdns_parser`, when `CASTMIRROR_BUILD_FUZZERS=ON`) |

A `.desktop` launcher lives at [`app/io.github.vindeckyy.CastMirror.desktop`](../app/io.github.vindeckyy.CastMirror.desktop).

### Tests and fuzzing

```bash
ctest --test-dir build --output-on-failure
# or
./build/tests/castmirror_tests
```

On Linux, `test_wgc_window_source.cc` (Windows-only) is excluded, leaving 255 registered CTest cases. To build and run the libFuzzer harnesses on Linux:

```bash
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCASTMIRROR_BUILD_GUI=OFF -DCASTMIRROR_ENABLE_TRAY=OFF -DCASTMIRROR_BUILD_FUZZERS=ON
cmake --build build-fuzz -j"$(nproc)"
./build-fuzz/tests/fuzz/fuzz_rtcp_parser -max_total_time=60
```

To build `.deb` or `.rpm` packages locally with CPack (requires `dpkg-dev` or `rpm`), run `cpack -G DEB` (or `RPM`) from `build/`, and verify license linkage with `./scripts/check_license_surface.sh --build-dir build`.

### Run

```bash
./build/app/castmirror-gui
./build/app/castmirror
./build/app/castmirror --device 192.168.1.150 --display 0 --preset High
./build/app/castmirror --device 192.168.1.150 --no-audio
```

Config and logs: `~/.config/castmirror/config.json`, `~/.config/castmirror/castmirror.log`, `~/.config/castmirror/castmirror-session.log`, and (when verbose JSON logging is enabled) `~/.config/castmirror/castmirror.ndjson`.

## Environment variables

- `CASTMIRROR_FORCE_SOFTWARE_ENCODE=1`: Force FFmpeg `libx264` software encoding even on systems where VAAPI (Linux) or Media Foundation (Windows) hardware encoding is available.
- `CASTMIRROR_VAAPI_DEVICE=/dev/dri/renderD129`: Linux: use one specific VAAPI render node instead of probing. Without it, CastMirror tries libva's autodetected device and then every `/dev/dri/renderD*` until one can open an H.264 encoder.
- `CASTMIRROR_FORCE_X11=1`: Linux: force X11 display capture instead of the PipeWire portal when running under a Wayland compositor via XWayland.
- `CASTMIRROR_CONFIG_DIR=/path/to/dir`: Read and write `config.json` and log files in this directory instead of the default per-user location (used by the test binary to keep test runs off your real settings).
- `CASTMIRROR_VERBOSE_JSON=1`: Enable structured NDJSON diagnostics in `castmirror.ndjson` (accepts `1` or `true`).
- `CASTMIRROR_CSS=/path/to/style.css`: Linux GTK 4 GUI: load a custom stylesheet instead of the bundled theme CSS.
- `CASTMIRROR_LANG=de`: Windows WinUI app: override the UI language (`en`, `es`, `de`, `fr`).
- `CASTMIRROR_DISABLE_WGC=1`: Windows: bypass Windows.Graphics.Capture for window sharing and fall back to DXGI desktop cropping.
- `CASTMIRROR_ALLOW_SYNTHETIC_CAPTURE=1`: Allow generated synthetic video/audio when real capture is unavailable (set automatically by `castmirror_tests`).
- `CASTMIRROR_SOAK_MINUTES=10`: Enable the long-running E2E soak test in `castmirror_tests` for the given duration in minutes.
- `CASTMIRROR_MSYS2_BIN` / `CASTMIRROR_DOTNET_ROOT`: Windows: override the MSYS2 UCRT64 `bin` and .NET SDK directories used by `build-all.bat`, `package.ps1`, `run-*.bat`, and `scripts\run_ubsan.ps1`.

## Network

Cast control uses **TCP 8009** (TLS) to the device, and mDNS discovery uses **UDP 5353**. Media uses **UDP** to the port in the ANSWER. The sender machine must be on the same LAN. If the picture never appears, allow outbound UDP and inbound RTCP on the host firewall. Helper scripts: [`scripts/setup_firewall.sh`](../scripts/setup_firewall.sh) (Linux) and [`scripts/setup_firewall.ps1`](../scripts/setup_firewall.ps1) (Windows).

## Audio

On Windows, capture uses WASAPI loopback (system-wide or filtered to a single executable via `audio_process_name`). On Linux, capture uses the PulseAudio/PipeWire **default sink monitor**; while mirroring with audio on, the default sink is muted so host speakers do not play the same stream, and the previous mute state is restored on Stop.
