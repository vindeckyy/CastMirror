CastMirror 1.0.0 for Linux is the first stable release of the GTK 4 app and the command-line tool. It mirrors a display or a single window, with sound, to a Chromecast, Google TV or Cast-enabled TV on the same network, without Chrome.

The Windows app is the main product and has its own release (`windows-v1.0.0`). The Linux build uses the same `castcore` engine, so it gets the same protocol, security and reliability fixes, but the desktop capture, audio and GUI layers on Linux are separate code, and they get less testing.

## Download

| File | Use it for |
|---|---|
| `castmirror_1.0.0_x86_64.deb` | Debian, Ubuntu and derivatives. |
| `castmirror_1.0.0_x86_64.rpm` | Fedora and other RPM distributions. Built best-effort on an Ubuntu runner, so treat it as less tested than the .deb. |
| `SHA256SUMS` | Checksums for the packages. |

```bash
sudo apt install ./castmirror_1.0.0_x86_64.deb
castmirror-gui              # GTK 4 app
castmirror --device 192.168.0.164 --display 0 --preset High
```

Your TV and PC must be on the same subnet. Control traffic goes to TCP 8009 on the TV, and media goes over UDP to the port the TV returns. `scripts/setup_firewall.sh` opens what is needed.

## What you get

- Discovery over mDNS, a subnet scan for networks that block it, and manual IP entry.
- Whole-display capture on X11 (XRandR with MIT-SHM), single-window capture on X11 including windows that are covered (XComposite), and Wayland capture through the xdg-desktop-portal ScreenCast picker over PipeWire.
- System audio from the PulseAudio or PipeWire default sink monitor, encoded as Opus.
- H.264 through VAAPI when a GPU is available, with an automatic fall back to libx264.
- Six quality presets, live bitrate changes, freeze and mute controls, an adaptive bitrate ladder, and reconnect after a dropped connection.
- A GTK 4 and libadwaita app with a live view of frame rate, bitrate, round-trip time and loss, plus a self-test wizard, and a CLI for scripting.

## Fixed since the pre-release builds

These come from the shared engine and apply on Linux too:

- The per-session encryption key and IV mask were written to the log. They are now redacted everywhere.
- Starting and stopping a cast at the same moment could free the session while it was still connecting. Stop now cancels an in-flight start.
- A state callback could deadlock the whole connection attempt.
- A TV that answered `error` to the offer made the app wait out the full timeout. It now fails at once.
- Pressing Back on the TV ended the app on the receiver and CastMirror relaunched it for 30 seconds. It now ends the cast.
- Frame ids advanced when the encoder produced nothing, so the TV stalled until the next key frame.
- Discovery listed phantom devices from unrelated mDNS traffic, and a TV that was switched off stayed in the list forever.
- Adaptive-bitrate and reconnect state were shared between threads without locks.
- Config saves could interleave and corrupt the file.
- The Game and Cinema presets did nothing different from Auto. They now set their own delay and bitrate.
- The HTTP/CAF fallback server is gone. It bound every interface without authentication.
- Colours are converted and tagged as BT.709, which is what TVs decode HD video as.

The complete list is in the [changelog](https://github.com/vindeckyy/CastMirror/blob/master/CHANGELOG.md).

## How it was tested

- The full unit and integration suite (TLS, offer and answer, and media streaming against a simulated receiver) runs in CI on Ubuntu Release, ASan and UBSan, TSan, and a Fedora container.
- The RTCP and mDNS parsers run under libFuzzer in CI.

## Known limits

- **Not yet verified on a physical-device matrix.** The device rows in `docs/DEVICE_MATRIX.md` predate this release and were not re-measured for it.
- The maintainers' hands-on testing this cycle was on Windows. The GTK app, the Wayland portal path and the VAAPI encoder were built and unit-tested in CI but not exercised on a live desktop for this release. Bug reports with the output of the in-app **Logs** tab are welcome.
- Tray support needs the Ayatana AppIndicator library at runtime.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset.
- Cast receivers accept H.264 and VP8 for mirroring, so there is no HEVC or VP9 option.

## Licence

CastMirror's own source is Apache-2.0. The binaries link FFmpeg with libx264, which is GPL, so the package as a whole is distributed under the GPL. The details and the source offer are in `NOTICE` and `THIRD-PARTY-LICENSES.txt`, installed under `/usr/share/doc/castmirror`.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
