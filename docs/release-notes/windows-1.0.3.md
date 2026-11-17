CastMirror 1.0.3 for Windows keeps the Windows release line aligned with the shared engine. This cycle fixes Linux A/V sync drift; Windows behaviour is unchanged, and the version shown in the app now reports 1.0.3.

## Download

| File | Use it for |
|---|---|
| `CastMirror-Setup-1.0.3-x64.exe` | The installer. Adds a Start menu entry and Windows Firewall rules for private and domain networks. |
| `CastMirror-win-x64.zip` | Portable copy. Run `scripts\setup_firewall.ps1` once from an elevated PowerShell, or allow CastMirror when Windows asks. |
| `SHA256SUMS-windows.txt` | Checksums for both files. |

Needs Windows 10 version 2004 or later, 64-bit. The binaries are not code-signed yet, so SmartScreen will warn on first run: choose **More info**, then **Run anyway**. Compare the file against `SHA256SUMS-windows.txt` first if you want to be sure.

## Fixed in 1.0.3

- The shared engine now keeps Linux A/V RTP timestamps on one steady clock, so long sessions no longer drift. This does not change the Windows capture or encoder paths.
- The About dialog, the version footer and the update-check fallback all report 1.0.3.

## How it was tested
- The full unit and integration suite passes locally: 226 tests, with two platform-specific cases skipped. The release workflow runs the Windows native suite before packaging and publishes the installer and portable zip from that build.
- The new A/V clock tests are deterministic and run in the shared suite.

## Known limits

- **Not yet verified on a physical-device matrix.** The Windows app was not exercised on a live desktop for this release, so if you try it on a real TV, a report in an issue is welcome.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset. This is not a game-streaming tool.
- Cast receivers accept H.264 and VP8 for mirroring, so there is no HEVC or VP9 option.

## Licence

CastMirror's own source is Apache-2.0. The Windows build includes FFmpeg and libx264, which are GPL, so the download as a whole is distributed under the GPL. The notice and the source offer are in `THIRD-PARTY-LICENSES.txt` inside the download.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
