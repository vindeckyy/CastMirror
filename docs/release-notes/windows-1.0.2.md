CastMirror 1.0.2 for Windows fixes the device list refreshing every few seconds. The list used to rebuild itself about every 3 seconds, so devices jumped around, disappeared and reappeared, and the TV you had selected was lost. Discovery now settles after about 30 seconds while still listening for new devices, so the list stays still and your selection stays put. This release also fixes the version shown in the About dialog, which still said 1.0.0 in 1.0.1.

## Download

| File | Use it for |
|---|---|
| `CastMirror-Setup-1.0.2-x64.exe` | The installer. Adds a Start menu entry and Windows Firewall rules for private and domain networks. |
| `CastMirror-win-x64.zip` | Portable copy. Run `scripts\setup_firewall.ps1` once from an elevated PowerShell, or allow CastMirror when Windows asks. |
| `SHA256SUMS-windows.txt` | Checksums for both files. |

Needs Windows 10 version 2004 or later, 64-bit. The binaries are not code-signed yet, so SmartScreen will warn on first run: choose **More info**, then **Run anyway**. Compare the file against `SHA256SUMS-windows.txt` first if you want to be sure.

## Fixed in 1.0.2

- The device list no longer refreshes every 3 seconds. Discovery now settles after about 30 seconds while still listening for new devices, so the list stops churning.
- Devices keep a stable identity across discovery updates, so they stop jumping around and disappearing from the list.
- The selected TV stays selected when the list updates.
- The About dialog and the version shown in the app now report 1.0.2. In 1.0.1 they still showed 1.0.0 because the assembly and file versions were left stale.

## How it was tested

- I added new unit tests covering the fix, and CI on the pull request was green, run 43635037364, all 9 checks passing.
- The Windows app was not exercised on a live desktop for this release, so if you try it on a real TV, a report in an issue is welcome.

## Known limits

- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset. This is not a game-streaming tool.
- Cast receivers accept H.264 and VP8 for mirroring, so there is no HEVC or VP9 option.

## Licence

CastMirror's own source is Apache-2.0. The Windows build includes FFmpeg and libx264, which are GPL, so the download as a whole is distributed under the GPL. The notice and the source offer are in `THIRD-PARTY-LICENSES.txt` inside the download.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
