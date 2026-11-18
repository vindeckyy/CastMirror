CastMirror 1.0.4 for Linux hardens hardware-encoder probing on hybrid-graphics machines, mDNS/TLS/session concurrency, and the GTK 4 desktop app and CLI, while keeping the Windows and Linux release lines aligned.

## Download

| File | Use it for |
|---|---|
| `castmirror_1.0.4_x86_64.deb` | Ubuntu 24.04, Debian 12, and derivatives. Built on Ubuntu 24.04 against FFmpeg 6. |
| `castmirror_1.0.4_debian13_x86_64.deb` | Debian 13 (trixie) and derivatives. Built inside a Debian 13 container against FFmpeg 7. |
| `castmirror_1.0.4_x86_64.rpm` | Fedora and other RPM distributions. Built best-effort on an Ubuntu runner, so treat it as less tested than the .deb. |
| `SHA256SUMS` | Checksums for the packages. |

The two .debs are not interchangeable. Pick the one that matches your distro, or dpkg will refuse to install it for missing FFmpeg libraries.

```bash
sudo apt install ./castmirror_1.0.4_debian13_x86_64.deb   # Debian 13
castmirror-gui              # GTK 4 app
castmirror --device 192.168.0.164 --display 0 --preset High
```

Your TV and PC must be on the same subnet. Control traffic goes to TCP 8009 on the TV, and media goes over UDP to the port the TV returns. `scripts/setup_firewall.sh` opens what is needed.

## Fixed in 1.0.4

- Hybrid-graphics machines no longer fall back to libx264 when a decode-only VAAPI node opens before the encoding GPU. CastMirror now probes `/dev/dri/renderD*` through a full H.264 encoder open, and `CASTMIRROR_VAAPI_DEVICE` selects a specific node when needed.
- Discovery and session startup/shutdown are hardened against races and malformed LAN packets: truncated mDNS TXT records no longer fall back to byte-scanning, `Stop()` joins the subnet-probe thread cleanly, `CastEngine` and `CastSession` guard concurrent `Start()`/`Stop()` cancellation (`stop_epoch_` and `channel_mutex_`), and secret redaction handles non-string JSON values.
- PulseAudio idle loops, adaptive bitrate ceiling/trend-margin accounting, Wayland stride bounds, and GTK 4 GUI option wiring (`BuildSessionOptions`, tray handling, and CLI `--window` handling) are fixed.

## How it was tested

- The full unit and integration suite passes locally: 255 CTest cases (254 passed, 1 Windows-only test skipped; 271 `TEST()` definitions across 31 test files) under both GCC Debug and ASan + UBSan. The release workflows run the same suite on Ubuntu, in the Debian 13 container, and on Windows.
- LibFuzzer (`fuzz_rtcp_parser` and `fuzz_mdns_parser`) and the script, doc-marker, soak-fixture, and accessibility checks pass in CI.

## Known limits

- **Not yet verified on a physical-device matrix.** The Linux GUI and Wayland path were built and unit-tested for this release but not exercised on a live desktop.
- This .deb is built without the system tray icon. The tray needs the `ayatana-appindicator-glib` library, which Ubuntu 24.04, the build image, does not package. A build from source on a distribution that has the library gets the tray.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset.

## Licence

CastMirror's own source is Apache-2.0. The binaries link FFmpeg with libx264, which is GPL, so the package as a whole is distributed under the GPL. The details and the source offer are in `NOTICE` and `THIRD-PARTY-LICENSES.txt`, installed under `/usr/share/doc/castmirror`.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
