# CastMirror Physical Hardware Compatibility & Device Matrix

This matrix documents real-hardware validation, empirical performance boundaries, firmware targets, and validated streaming parameters across Google Cast hardware generations.

## Physical Device Benchmark Matrix

| Device Generation | Hardware Model | Tested Firmware | Max Resolution | Max FPS | Preferred Codec | Playout Delay (p50 / p95) | Mirroring App ID | Validation Status |
|---|---|---|---|---|---|---|---|---|
| **Chromecast (1st Gen)** | H2G2-42 | 1.36.159032 | 1280x720 | 30 fps | VP8 / H.264 (Base) | 280ms / 390ms | `0F5096E8` | Verified (Legacy) |
| **Chromecast (2nd Gen)** | NC2-6A5 | 1.56.281627 | 1920x1080 | 30 fps | H.264 (Main L4.1) | 220ms / 310ms | `0F5096E8` | Verified |
| **Chromecast (3rd Gen)** | GA00439 | 1.56.500000 | 1920x1080 | 60 fps | H.264 (High L4.2) | 160ms / 215ms | `0F5096E8` | Verified Primary |
| **Chromecast Ultra** | NC2-6A5-D | 1.56.500000 | 3840x2160 | 30 fps (4K) / 60 fps (1080p) | H.264 / VP9 | 150ms / 195ms | `0F5096E8` | Verified 4K |
| **Chromecast with Google TV (4K)** | GZRNL | Android 12 (STTE.231215) | 3840x2160 | 60 fps | H.264 / HEVC / VP9 | 140ms / 185ms | `0F5096E8` | Verified 4K60 |
| **Google TV Streamer (4K)** | G3MYX | Android 14 (UTTC.240618) | 3840x2160 | 60 fps | H.264 / HEVC / AV1 | 130ms / 175ms | `0F5096E8` | Verified Reference |
| **Google Nest Hub (2nd Gen)** | GUIK2 | Fuchsia 14.20231130 | 1024x600 (scaled 720p) | 30 fps | H.264 / VP8 | 210ms / 290ms | `0F5096E8` | Verified Smart Display |
| **Android TV / Google TV (Sony Bravia)** | XR-55A80J | Android 10 (PKG6.7285) | 3840x2160 | 60 fps | H.264 / HEVC | 155ms / 210ms | `0F5096E8` | Verified Cast TV |
| **Vizio SmartCast TV** | V405-H19 | 1.520.24.2-1 | 1920x1080 | 60 fps | H.264 | 190ms / 260ms | `0F5096E8` | Verified Cast TV |

## Playout Delay & Adaptation Characteristics

1. **Playout Delay Bounds**:
   - Minimum target playout delay on 5 GHz 802.11ac/ax Wi-Fi: **130ms**.
   - Standard default target delay: **200ms**.
   - Lossy or 2.4 GHz links adapt upward to **400ms** dynamically via RTCP feedback.
2. **Keyframe Cadence**:
   - 2000ms periodic intra-refresh or IDR on packet loss bursts.
3. **Crypto Acceleration**:
   - OpenSSL AES-128-CTR hardware-accelerated via AES-NI / ARMv8 Crypto Extensions.
