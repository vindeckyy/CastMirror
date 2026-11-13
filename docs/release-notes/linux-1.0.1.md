CastMirror 1.0.1 for Linux fixes the Debian 13 install failure reported in [#1](https://github.com/vindeckyy/CastMirror/issues/1). The 1.0.0 .deb was built on Ubuntu 24.04, so it links FFmpeg 6 sonames (libavcodec60, libavutil58, libswscale7) that do not exist on Debian 13. Debian 13 ships FFmpeg 7, so I added a second package built inside a Debian 13 container.

## Download

| File | Use it for |
|---|---|
| `castmirror_1.0.1_x86_64.deb` | Ubuntu 24.04, Debian 12, and derivatives. Built on Ubuntu 24.04 against FFmpeg 6. |
| `castmirror_1.0.1_debian13_x86_64.deb` | Debian 13 (trixie) and derivatives. Built inside a Debian 13 container against FFmpeg 7. |
| `castmirror_1.0.1_x86_64.rpm` | Fedora and other RPM distributions. Built best-effort on an Ubuntu runner, so treat it as less tested than the .deb. |
| `SHA256SUMS` | Checksums for the packages. |

The two .debs are not interchangeable. Pick the one that matches your distro, or dpkg will refuse to install it for missing FFmpeg libraries.

```bash
sudo apt install ./castmirror_1.0.1_debian13_x86_64.deb   # Debian 13
castmirror-gui              # GTK 4 app
castmirror --device 192.168.0.164 --display 0 --preset High
```

## Fixed in 1.0.1

- The Debian 13 package in this release is built against FFmpeg 7 (libavcodec61, libavutil59, libswscale8). I verified it installs cleanly on Debian 13.7, `castmirror --help` runs, and no libraries are missing. The release workflow now has a regression gate that checks the new package Depends on every tag run.
- I fixed the build on OpenSSL 4.0 (Fedora 45). `X509_get_subject_name` and `X509_get_issuer_name` now return `const X509_NAME*`, so the device auth code and the test and fake receiver cert helpers are const-correct. This also compiles on OpenSSL 3.

## How it was tested

- The full unit and integration suite runs in CI on Ubuntu Release, ASan and UBSan, TSan, and a Fedora container, and it is green on this release.
- I built the Debian 13 package in a Debian 13.7 chroot and installed it there before tagging.

## Known limits

- This .deb is built without the system tray icon. The tray needs the `ayatana-appindicator-glib` library, which Ubuntu 24.04, the build image, does not package. A build from source on a distribution that has the library gets the tray.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset.

## Licence

CastMirror's own source is Apache-2.0. The binaries link FFmpeg with libx264, which is GPL, so the package as a whole is distributed under the GPL. The details and the source offer are in `NOTICE` and `THIRD-PARTY-LICENSES.txt`, installed under `/usr/share/doc/castmirror`.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
