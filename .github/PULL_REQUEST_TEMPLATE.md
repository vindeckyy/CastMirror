## Summary

- 

## Test plan

- [ ] `cmake --build build` succeeds
- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] `dotnet test tests/winui/CastMirror.Tests.csproj` passes (Windows client / localization changes, or N/A)
- [ ] C++ style check passes with `clang-format==22.1.8` (`clang-format --dry-run -Werror`), or N/A
- [ ] Doc & UI checks pass as applicable (`python3 scripts/count_tests.py --check`, `./scripts/render_device_matrix.sh --check`, `./scripts/audit_a11y.sh`, `./scripts/check_version.ps1`)
- [ ] Windows UI change verified with `scripts\smoke_winui.ps1` and by eye in light and dark (screenshot if layout/copy changed), or N/A
- [ ] Linux GUI change verified with `./build/app/castmirror-gui`, or N/A
- [ ] Real Cast device tested, or N/A because the change cannot affect a session

## Notes

<!-- Protocol, license/NOTICE, or follow-up work -->
