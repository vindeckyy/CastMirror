CastMirror 1.0.3 for Linux fixes A/V sync drift on long sessions. Audio and video RTP timestamps now share one steady clock: PulseAudio frames use a fixed 10 ms timeline anchored on the first real PCM frame, X11 frames use pacing with a content timestamp from the middle of the X read, and idle re-sends no longer step the video timeline forward. Wayland uses PipeWire presentation timestamps when available and carries the previous timestamp on cursor-only updates.

## Download

| File | Use it for |
|---|---|
| `castmirror_1.0.3_x86_64.deb` | Ubuntu 24.04, Debian 12, and derivatives. Built on Ubuntu 24.04 against FFmpeg 6. |
| `castmirror_1.0.3_debian13_x86_64.deb` | Debian 13 (trixie) and derivatives. Built inside a Debian 13 container against FFmpeg 7. |
| `castmirror_1.0.3_x86_64.rpm` | Fedora and other RPM distributions. Built best-effort on an Ubuntu runner, so treat it as less tested than the .deb. |
| `SHA256SUMS` | Checksums for the packages. |

The two .debs are not interchangeable. Pick the one that matches your distro, or dpkg will refuse to install it for missing FFmpeg libraries.

```bash
sudo apt install ./castmirror_1.0.3_debian13_x86_64.deb   # Debian 13
castmirror-gui              # GTK 4 app
castmirror --device 192.168.0.164 --display 0 --preset High
```

Your TV and PC must be on the same subnet. Control traffic goes to TCP 8009 on the TV, and media goes over UDP to the port the TV returns. `scripts/setup_firewall.sh` opens what is needed.

## Fixed in 1.0.3

- Audio no longer drifts away from video over a long film. PulseAudio frames advance on a fixed 10 ms timeline anchored on the first real PCM frame, so latency-estimate jitter, silence, corked streams and scheduling gaps cannot move the audio RTP clock.
- Idle X11 desktops no longer step video out of sync. Frames are paced, and a re-send carries an advanced timestamp instead of a wall-clock jump.
- Wayland cursor-only updates no longer push the video clock forward. PipeWire presentation timestamps are used when present, with a carried timestamp when they are not.
- A receiver's playout-delay request on the audio stream is now honoured, keeping both streams at the same buffer depth.

## How it was tested

- The full unit and integration suite passes locally: 226 tests, with two platform-specific cases skipped. The release workflows run the same suite on Ubuntu, in the Debian 13 container, and on Windows.
- A deterministic two-hour simulation with jitter and a 2-second suspend checks that A/V skew stays bounded and that the 48 kHz and 90 kHz RTP tick rates stay exact.
- An end-to-end test against the simulated receiver asserts bounded A/V RTP epoch skew, including through a muted interval.

## Known limits

- **Not yet verified on a physical-device matrix.** The Linux GUI and Wayland path were built and unit-tested for this release but not exercised on a live desktop.
- This .deb is built without the system tray icon. The tray needs the `ayatana-appindicator-glib` library, which Ubuntu 24.04, the build image, does not package. A build from source on a distribution that has the library gets the tray.
- Cast keeps a playout buffer, so expect about 150 to 400 ms of delay depending on the preset.

## Licence

CastMirror's own source is Apache-2.0. The binaries link FFmpeg with libx264, which is GPL, so the package as a whole is distributed under the GPL. The details and the source offer are in `NOTICE` and `THIRD-PARTY-LICENSES.txt`, installed under `/usr/share/doc/castmirror`.

CastMirror is not affiliated with Google. Chromecast, Google Cast and Google TV are trademarks of Google LLC.
