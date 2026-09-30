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
- `.clang-format` and `.clang-tidy` are enforced by the `style` CI job. clang-format is a hard gate; clang-tidy is reported but not yet a gate while the existing backlog is worked down.
- **Line endings:** `.gitattributes` forces LF except for `*.bat`/`*.cmd`/`*.ps1`. Do not commit CRLF into a `.sh` or a workflow — it breaks the POSIX shell and every `run: |` block.

## Tests

- Add or extend Google Test cases under `tests/` when you change protocol, crypto, RTP/RTCP, config, or adaptation
- GUI-only tweaks still need a manual pass of discover → idle controls → (if you have a device) start/stop
- Do not weaken tests to make CI green
- **`ctest` count vs. macro count is not a discrepancy.** `scripts/count_tests.py` counts `TEST(`/`TEST_F(` macros in the registered sources (182). `ctest` reports what `gtest_discover_tests` actually registered on the current platform: 179 on Windows, because `test_portal_source.cc` is dropped when there is no Wayland portal.
- **New parsers get fuzz coverage.** Anything reading bytes off the LAN (RTCP, mDNS TXT, protobuf) should have a libFuzzer harness under `tests/fuzz/` wired into the `fuzz` CI job. `CASTMIRROR_BUILD_FUZZERS` is OFF by default so an ordinary build needs no Clang.
- Coverage is gated by `CASTMIRROR_MIN_COVERAGE` in the `coverage` job. Raise the ratchet when you add tests; do not lower it to make a build pass.

## Invariants

"Capture must not run except while a live session is active" is a documented
success bar, so it is checked at runtime in **all** build types
(`CheckCaptureInvariant` in `core/include/castcore/types.h`). It deliberately
does not rely on `assert()`, which `NDEBUG` removes from Release builds. If you
add a lifecycle invariant, follow the same pattern and report through the
installed reporter rather than trapping.

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
