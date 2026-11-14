CastMirror 1.0.2 for Linux fixes the device list refreshing every few seconds. The list used to rebuild itself about every 3 seconds, so devices jumped around, disappeared and reappeared, and the TV you had selected was lost. Discovery now settles after about 30 seconds while still listening for new devices, so the list stays still and your selection stays put.

## Download

| File | Use it for |
|---|---|
| `castmirror_1.0.2_x86_64.deb` | Ubuntu 24.04, Debian 12, and derivatives. Built on Ubuntu 24.04 against FFmpeg 6. |
| `castmirror_1.0.2_debian13_x86_64.deb` | Debian 13 (trixie) and derivatives. Built inside a Debian 13 container against FFmpeg 7. |
| `castmirror_1.0.2_x86_64.rpm` | Fedora and other RPM distributions. Built best-effort on an Ubuntu runner, so treat it as less tested than the .deb. |
| `SHA256SUMS` | Checksums for the packages. |

The two .debs are not interchangeable. Pick the one that matches your distro, or dpkg will refuse to install it for missing FFmpeg libraries.

```bash
sudo apt install ./castmirror_1.0.2_debian13_x86_64.deb   # Debian 13
castmirror-gui              # GTK 4 app
castmirror --device 192.168.0.164 --display 0 --preset High
```

## Fixed in 1.0.2

- The device list no longer refreshes every 3 seconds. Discovery now settles after about 30 seconds while still listening for new devices, so the list stops churning.
- Devices keep a stable identity across discovery updates, so they stop jumping around and disappearing from the list.
- The selected TV stays selected when the list updates.

## How it was tested

- I added new unit tests covering the fix, and the full unit and integration suite runs in CI on Ubuntu Release, ASan and UBSan, TSan, and a Fedora container. CI on the pull request was green, run 43635037364, all 9 checks passing.
- The Linux GUI and Wayland path were built and unit-tested for 1.0.2 but not exercised on a live desktop.

## Known limits

- This .deb is built without the system tray icon. The tray needs the `ayatana-appindicator-glib` library, which Ubuntu 24.04, the build image, does not package. A build from source on a distribution that has the library gets the tray.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset.

## Licence

CastMirror's own source is Apache-2.0. The binaries link FFmpeg with libx264, which is GPL, so the package as a whole is distributed under the GPL. The details and the source offer are in `NOTICE` and `THIRD-PARTY-LICENSES.txt`, installed under `/usr/share/doc/castmirror`.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
