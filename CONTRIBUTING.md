# Contributing to CastMirror

Thanks for helping. CastMirror is a native C++20 Cast Streaming sender. Keep changes small, honest, and tested.

## Development setup

See [docs/building.md](docs/building.md) for the tools and packages. On Windows, the usual loop is:

```bat
build-all.bat
ctest --test-dir build --output-on-failure
dotnet test tests\winui\CastMirror.Tests.csproj
run-gui.bat
```

On Linux:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The Windows app is the primary product. Linux builds get fixes, not new features.

## The Windows client

`app/winui` is a WinUI 3 app on .NET 8 that talks to `castcore.dll` through `Services/CastCoreBridge.cs`. A few rules keep it maintainable:

- **Logic that doesn't need XAML goes in `Services/` and gets a test** in `tests/winui`. That project (`tests/winui/CastMirror.Tests.csproj`) compiles the service files directly, so the tests run without the Windows App SDK.
- **Changing the C API?** Bump `CASTMIRROR_ABI_VERSION` in `core/include/castcore/c_api.h` (currently `3u`), update the struct mirrors and the `VerifyNativeAbi` check in `app/winui/Services/CastCoreBridge.cs`, and rebuild both sides together.
- **New setting?** Add the key in three places: `core/include/castcore/config.h` and `core/src/config.cc` (load and save), `core/src/c_api.cc` (get and set JSON), and `CastMirrorSettings` in `app/winui/Services/SettingsService.cs`. Unknown keys are ignored, so a client and engine from different builds still talk.
- **Dialogs** go through `Services/Dialogs.cs`, which applies the forced theme and prevents two dialogs at once.
- **Layout and colour** come from `App.xaml`: the `Shell*` brushes and the shared styles (`ShellCardStyle`, `FlushListItemStyle`, the button styles). Don't hard-code a colour or an inset in a view. Check the window in dark, light and high contrast, and line new controls up with the 16 px card inset the existing cards use.
- **Buttons that change colour** need a template-safe style (override the `ButtonBackground*` resources) rather than a code-set `Background`, which hover and pressed states overwrite.
- **User-facing text** is plain and specific. Say what happened and what to do. No filler, no "seamless", no exclamation marks. Read new strings aloud before you commit them.
- **Translations.** English is the source text, in the XAML and in `Localizer.T(...)` calls. `app\winui\Strings\translations.tsv` holds one row per string (English, es, de, fr); edit it and run `scripts\build_translations.ps1` to regenerate the JSON files. A missing translation shows the English text. New UI text needs a row in every language, and `tests\winui` fails if a XAML string has none or a row matches nothing. To add a language, add a column and its code to the script. Check longer languages (German) for clipped text; let headings wrap.
- **Accessibility.** Give controls an accessible name, mark status text that changes as a live region, and check the window with Tab only.

Smoke-test the real app after UI changes. `scripts\smoke_winui.ps1` starts the exe, opens Settings, Logs and About through UI Automation, checks that a second launch wakes the first, and fails if the error log gets a new exception. It needs an interactive desktop, so it is a local check, not a CI job.

## Code style

- C++20, match surrounding `castcore` style
- Compiler flags already include `-Wall -Wextra`
- No drive-by clang-format of unrelated files
- Prefer existing types in `core/include/castcore/types.h`
- Do not add debug NDJSON / agent instrumentation
- `.clang-format` and `.clang-tidy` are run by the `style` CI job (and `ubuntu-tray` runs `clang-tidy -p build-tray app/gui/tray.cc` with `CASTMIRROR_HAVE_TRAY=1`). `clang-format` is a hard gate pinned to **`clang-format==22.1.8`** (`find core app tools tests \( -name '*.cc' -o -name '*.h' \) | xargs clang-format --dry-run -Werror`); `clang-tidy` is reported in `style` after generating `core/cast_channel.pb.h`, `core/cast_channel.pb.cc`, and `app/castmirror-resources.c`.
- **Line endings:** `.gitattributes` forces LF except for `*.bat`/`*.cmd`/`*.ps1`. Do not commit CRLF into a `.sh` or a workflow — it breaks the POSIX shell and every `run: |` block.

## Tests

- Add or extend Google Test cases under `tests/` when you change protocol, crypto, RTP/RTCP, config, or adaptation (set `-DBUILD_TESTING=OFF` only for packaging environments without GTest).
- GUI-only tweaks still need a manual pass of discover → idle controls → (if you have a device) start/stop.
- Do not weaken tests to make CI green.
- **`ctest` count vs. macro count is not a discrepancy.** `scripts/count_tests.py` counts `TEST(`/`TEST_F(`/`TEST_P(` macros in the 31 files registered in `tests/CMakeLists.txt` (271 cases). `ctest` reports what `gtest_discover_tests` actually compiles and registers on the current platform (for example, 255 on Linux where `test_wgc_window_source.cc` is excluded, and fewer on Windows where `test_portal_source.cc` is excluded because there is no Wayland portal).
- **Keep doc counters and generated tables in sync.** Run `python3 scripts/count_tests.py` after adding or removing test cases (CI checks `python3 scripts/count_tests.py --check`), `./scripts/render_device_matrix.sh` after updating `docs/bench/device_runs/` (`--check` in CI), `./scripts/audit_a11y.sh` after GTK 4 UI changes, `./scripts/check_license_surface.sh --build-dir build` for linkage hygiene, and `scripts/check_version.ps1` when changing versions.
- **Windows client & UBSan tests.** Run `dotnet test tests/winui/CastMirror.Tests.csproj` (covers client services and localization key parity across `en`/`es`/`de`/`fr`) and `scripts\run_ubsan.ps1` (`windows-ubsan` CI job).
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

Use the [PR template](.github/PULL_REQUEST_TEMPLATE.md). Include:

- Why the change exists
- How you verified it (`ctest`, GUI, real Cast device)
- Screenshots for GUI layout or copy changes. The Windows ones in `docs/assets` show a 1280x800 window with placeholder device names, so retake them when the layout moves

## Protocol work

Cast Streaming is a Chromium-compatible protocol, not a Google-supported desktop SDK. Document behavior; do not add exploit-oriented samples. Firmware app IDs (`0F5096E8`, audio-only `85CDB22F`) can change — note that when you touch launch logic.

Historical lab numbers live in [docs/TEST_REPORT.md](docs/TEST_REPORT.md); do not treat them as a live CI badge.

## License

Contributions are accepted under the Apache License 2.0 (see `LICENSE`). If your change links additional third-party code, update `NOTICE`.
