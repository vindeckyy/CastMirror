# Contributing to CastMirror

Thanks for helping. CastMirror is a native C++20 Cast Streaming sender. Keep changes small, honest, and tested.

## Development setup

See [docs/building.md](docs/building.md) for packages and CMake. Typical loop:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure
```

Run the GUI after UI changes:

```bash
./build/app/castmirror-gui
```

## Code style

- C++20, match surrounding `castcore` style
- Compiler flags already include `-Wall -Wextra`
- No drive-by clang-format of unrelated files
- Prefer existing types in `core/include/castcore/types.h`
- Do not add debug NDJSON / agent instrumentation

## Tests

- Add or extend Google Test cases under `tests/` when you change protocol, crypto, RTP/RTCP, config, or adaptation
- GUI-only tweaks still need a manual pass of discover → idle controls → (if you have a device) start/stop
- Do not weaken tests to make CI green

## Pull requests

Use the PR template. Include:

- Why the change exists
- How you verified it (`ctest`, GUI, real Cast device)
- Screenshots for GUI layout or copy changes

## Protocol work

Cast Streaming is a Chromium-compatible protocol, not a Google-supported desktop SDK. Document behavior; do not add exploit-oriented samples. Firmware app IDs (`0F5096E8`, audio-only `85CDB22F`) can change — note that when you touch launch logic.

Historical lab numbers live in [docs/TEST_REPORT.md](docs/TEST_REPORT.md); do not treat them as a live CI badge.

## License

Contributions are accepted under the Apache License 2.0 (see `LICENSE`). If your change links additional third-party code, update `NOTICE`.
