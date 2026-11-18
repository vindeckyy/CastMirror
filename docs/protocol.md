# CastMirror Streaming Protocol Notes

CastMirror is a **native sender** (`castcore`) that speaks Cast V2 control, TLS device authentication, mirroring `OFFER`/`ANSWER` negotiation, and encrypted Cast RTP/RTCP media transport directly over IPv4 without external receiver-side SDK binaries.

---

## 1. Discovery (`device_discovery.cc`)

`DeviceDiscovery` (`core/include/castcore/device_discovery.h`, `core/src/device_discovery.cc`) discovers receivers via active mDNS and an optional IPv4 subnet probe:

### mDNS (`224.0.0.251:5353`)
- Queries `_googlecast._tcp.local` and `_googlezone._tcp.local` using both `QM` (`0x0001`) and `QU` (`0x8001`) PTR queries across all active non-loopback IPv4 interfaces plus `INADDR_ANY`.
- Re-queries every **3 seconds**; after `kSettleUnchangedRounds = 10` (~30 s) of unchanged results with at least one device discovered, active probing settles into passive listening until `TriggerScan()` resets the settle state.
- **Expiry:** mDNS-discovered entries expire after **10 minutes** (`ExpireStaleDevices()`, checked every 30 s). Manual entries and subnet-probed entries do not expire on the mDNS timer.
- **Parser & TXT bounds:**
  - Maximum TXT entry length `255`, key length `<= 64`, value length `<= 255` (first-key-wins).
  - Fallback raw key scan (`fn=`, `id=`, `md=`, `ca=`, `st=`, `ve=`, `rs=`, `bs=`) runs only when no structured TXT RR was parsed.
  - Responses missing `id` or `fn` are discarded.
  - SRV ports are restricted to `8009` and `8008` (any other advertised SRV port normalizes to `8009`).

### Optional `/24` Subnet Probe (`subnet_scan_enabled`)
When enabled (`Config::subnet_scan_enabled`), `DeviceDiscovery` additionally probes local `/24` IPv4 subnets:
- **TCP port `8009`:** 350 ms connect timeout, batches of 32 hosts, rate-limited to `<= 64 hosts/s`.
- **HTTP Eureka info (`:8008`):** `GET /setup/eureka_info?params=name,device_info HTTP/1.1` with a 400 ms timeout, up to 3 attempts, capped at 64 KiB response payload.

---

## 2. Control Plane (`cast_channel.cc`, `cast_channel.proto`, `cast_app_ids.h`)

`CastChannel` (`core/include/castcore/cast_channel.h`, `core/src/cast_channel.cc`) manages the TLS control socket to port `8009`:

- **TLS & Socket Options:**
  - TLS **1.2 pinned** (`SSL_OP_NO_TLSv1_3`) with `SSL_OP_IGNORE_UNEXPECTED_EOF`, `TCP_NODELAY`, non-blocking connect timeout **5000 ms**, and 6 s `SO_RCVTIMEO` / `SO_SNDTIMEO`.
  - `SSL_VERIFY_PEER` defers self-signed receiver certificate failures in `SslVerifyCallback` so trust is enforced cryptographically by Cast `DeviceAuth` rather than host PKI.
- **Wire Framing:** 4-byte big-endian length prefix followed by `castcore.proto.CastMessage` (`core/src/proto/cast_channel.proto`), `protocol_version = CASTV2_1_0`. Incoming messages are capped at **1 MiB** (`1024 * 1024` bytes).
- **Redaction:** `RedactSecrets()` scrubs `aesKey` and `aesIvMask` fields before control payloads are logged.

### Namespaces & Session Sequence
| Namespace | Purpose | Source / Destination |
|---|---|---|
| `urn:x-cast:com.google.cast.tp.deviceauth` | Binary challenge/response (`DeviceAuthMessage`) | `sender-0` → `receiver-0` |
| `urn:x-cast:com.google.cast.tp.connection` | Virtual `CONNECT` / `CLOSE` (`userAgent: "CastMirror/1.0"`) | `sender-0` → `receiver-0`, then `"streaming_sender"` → `app_transport_id_` |
| `urn:x-cast:com.google.cast.tp.heartbeat` | `PING` / `PONG` liveness | Every **2.0 s**; disconnects after **>6000 ms** without `PONG`; auto-replies `PONG` to incoming `PING` |
| `urn:x-cast:com.google.cast.receiver` | `LAUNCH`, `RECEIVER_STATUS`, `STOP` | `sender-0` → `receiver-0` |
| `urn:x-cast:com.google.cast.webrtc` | Mirroring `OFFER`, `ANSWER`, `GET_STATUS`, `STATUS` | `"streaming_sender"` ↔ `app_transport_id_` |

### Receiver App IDs (`core/include/castcore/cast_app_ids.h`)
- **Audio + Video Mirroring (`0F5096E8`):** Default Chrome Mirroring receiver app ID used whenever video output is active.
- **Audio-Only Mirroring (`85CDB22F`):** Used automatically when `enable_audio_ && !HasVideoOut()`.
- `LAUNCH` payload sends `appId`, `language: "en-US"`, and `supportedAppTypes: ["WEB"]`, waiting up to `launch_timeout_s` (default **8 s**). `OFFER` → `ANSWER` waits up to `answer_timeout_s` (default **5 s**).

---

## 3. Device Authentication (`device_auth.cc`)

Before sending `CONNECT`/`LAUNCH`, `CastChannel::AuthenticateDevice()` (`core/src/device_auth.cc`) performs cryptographic proof-of-possession:
1. Generates a 32-byte random challenge nonce (`RAND_bytes`).
2. Sends a binary `DeviceAuthMessage` challenge on `urn:x-cast:com.google.cast.tp.deviceauth`.
3. Verifies the echoed nonce in constant time (`CRYPTO_memcmp`).
4. Validates the device certificate chain (`X509_V_FLAG_PARTIAL_CHAIN`) against the embedded **Cast Root CA** and **Eureka Root CA** certificates.
5. Verifies the signature over `echoed_nonce || peer_cert_DER` (trying **SHA-256** first, then **SHA-1** fallback for older receivers).

---

## 4. Offer / Answer Negotiation (`mirroring_negotiator.cc`, `cast_session.cc`)

`MirroringNegotiator` (`core/include/castcore/mirroring_negotiator.h`, `core/src/mirroring_negotiator.cc`) exchanges JSON messages on `urn:x-cast:com.google.cast.webrtc`:

### `OFFER` Schema
```json
{
  "type": "OFFER",
  "seqNum": 1001,
  "offer": {
    "castMode": "mirroring",
    "receiverGetStatus": true,
    "supportedStreams": [
      {
        "index": 0,
        "type": "audio_source",
        "codecName": "opus",
        "rtpProfile": "cast",
        "rtpPayloadType": 127,
        "ssrc": 1,
        "bitRate": 192000,
        "timeBase": "1/48000",
        "channels": 2,
        "samplingRate": 48000,
        "targetDelay": 200,
        "aesKey": "<32-hex>",
        "aesIvMask": "<32-hex>"
      },
      {
        "index": 1,
        "type": "video_source",
        "codecName": "h264",
        "rtpProfile": "cast",
        "rtpPayloadType": 96,
        "ssrc": 2,
        "bitRate": 8000000,
        "maxBitRate": 8000000,
        "timeBase": "1/90000",
        "maxFrameRate": "60/1",
        "targetDelay": 200,
        "profile": "high",
        "level": "4.2",
        "resolutions": [{ "width": 1920, "height": 1080 }],
        "aesKey": "<32-hex>",
        "aesIvMask": "<32-hex>"
      }
    ]
  }
}
```
- **Codec coercion:** `codecName` for video is `"h264"` or `"vp8"`. If configuration requests VP9, HEVC, or AV1, `CastSession::Start()` coerces it to `"h264"` (`CodecFallbackEncoder`). `profile: "high"` and `level: "4.2"` are emitted for H.264; `resolutions` carries up to 2 rungs.
- **`ANSWER` validation:** Matches `seqNum` against `offer_seq_num_`, requires `udpPort` in `1..65535`, and verifies matching `sendIndexes[]` and `ssrcs[]`.
- **`GET_STATUS`:** Receiver status polls on `urn:x-cast:com.google.cast.webrtc` are answered with `{"type":"STATUS","result":"ok","status":[...]}`.

---

## 5. Frame Encryption (`frame_crypto.cc`)

`FrameCrypto` (`core/include/castcore/frame_crypto.h`, `core/src/frame_crypto.cc`) encrypts each encoded video or audio access unit with **AES-128-CTR** (`EVP_aes_128_ctr()`, no padding) using the per-stream 16-byte `aesKey` and `aesIvMask` negotiated in `OFFER`:

- **16-byte Counter Nonce Construction:**
  - Bytes `0..7`: `0x00`
  - Bytes `8..11`: Big-endian 32-bit `frame_id`
  - Bytes `12..15`: `0x00`
  - XORed byte-for-byte with `cast_iv_mask`
- Both temporary nonces and key material are cleared with `OPENSSL_cleanse()` in the destructor and after encryption.

---

## 6. Cast RTP Packetization (`rtp_packetizer.cc`)

`RtpPacketizer` (`core/include/castcore/rtp_packetizer.h`, `core/src/rtp_packetizer.cc`) splits each encrypted frame into UDP datagrams up to `max_packet_size` (default **1460 bytes**, up to **65536** packets per frame):

- **Fixed 12-byte RTP Header:** `V=2`, marker bit `0x80` set on the final packet of each frame, payload type `96` (video) or `127` (audio), monotonic 16-bit sequence number, 32-bit RTP timestamp (90 kHz video / 48 kHz audio), and stream SSRC (`2` video / `1` audio).
- **7-byte Cast Header (`kBaseHdrSize = 19` total bytes):**
  - Byte `12` (flags): `K` (`0x80`) for keyframes, `R` (`0x40`, always set), and `EXT_count = 1` on packet `0` when `playout_delay_ms > 0`.
  - Byte `13`: `frame_id & 0xFF`.
  - Bytes `14..15`: Big-endian `packet_id`.
  - Bytes `16..17`: Big-endian `max_packet_id`.
  - Byte `18`: `referenced_frame_id & 0xFF`.
- **Optional 4-byte Adaptive Latency Extension:** Attached on packet `0` when `playout_delay_ms > 0` (`uint16` header `(1 << 10) | 2` followed by big-endian 16-bit `playout_delay_ms`, clamped to `65535`).

---

## 7. RTCP Feedback, Transport Pacing & Playout Delay (`rtcp_parser.cc`, `cast_transport.cc`, `adaptive_controller.cc`)

### Compound RTCP Parsing (`rtcp_parser.cc`)
`RtcpParser` parses incoming UDP packets (capped at **64** RTCP blocks per packet; `CastTransport` drops packets from non-peer IPs):
- **PT 201 (`RR`):** Computes RFC 3550 RTT from `LSR`/`DLSR` (clamped at **3000 ms**) and converts 90 kHz interarrival jitter to milliseconds via `(jitter + 45) / 90`.
- **PT 202 (`SDES`):** Skipped.
- **PT 206 (`PSFB`, FMT 1 `PLI`):** Triggers an immediate IDR keyframe request (`RequestKeyframe()`).
- **PT 204 (`APP`) / PT 206 (`PSFB`) `'CAST'` (`0x43415354`):** Extracts 8-bit expanded `checkpoint_frame_id`, receiver `playout_delay_ms`, `0..255` selective NACK entries (including `0xFFFF` full-frame NACK), and optional `'CST2'` (`0x43535432`) ACK bitvectors (`2..254` octets encoding `checkpoint + 2 + oct*8 + bit`).
- **PT 207 (`XR`, BT 4):** Receiver Reference Time report block parsed for feedback metrics.

### Transport Pacing & Retransmission (`cast_transport.cc`)
- **Socket tuning:** `IP_TOS = 0x28` (DSCP AF41), `SO_SNDBUF = 4 MB`, `SO_RCVBUF = 2 MB`.
- **Pacing:** Token bucket releasing **10 packets per 2 ms** interval.
- **Sender Reports (`SR`):** Sent at most every **40 ms** per active stream.
- **Retransmission cache:** Retains **30** video frames and **50** audio frames; rejects stale erasures via `SafeCacheEraseLimit` (frames >32 ahead of checkpoint are ignored) and deduplicates repeated NACKs within **80 ms**.

### Playout Delay Adaptation (`adaptive_controller.cc`)
`AdaptiveController` clamps runtime target playout delay to **150–400 ms** (`StepUpPlayoutDelay` walks `150 → 200 → 300 → 400 ms` when jitter exceeds **30 ms**, loss exceeds **3%**, or a NACK burst / PLI occurs; `StepDownPlayoutDelay` steps back down after **15** consecutive clean intervals).

---

## 8. Reconnect & Teardown Semantics (`cast_session.cc`, `session_recovery.cc`)

- **Receiver-initiated end:** A `CLOSE` message received from `app_transport_id_` terminates the session cleanly (`"The cast was ended on the TV"`).
- **Automatic recovery:** TLS channel drops, heartbeat timeouts (>6 s), video capture/encode stalls (`>= 5 s`), or encoder reconfigure failures invoke `RequestReconnect()` within `Config::reconnect_window_s` (default **30 s**, valid range **10–600 s**).
- **Backoff (`session_recovery.cc`):** `ReconnectBackoffSeconds(attempt)` computes exponential sleep `min(8, 1 << (attempt - 1))` seconds.
- **Bounded shutdown:** `CastSession::Stop()` budgets teardown within **<= 500 ms** across transport, encoder, capture, and control threads.

---

## 9. Scope Boundaries

CastMirror's primary path is **not**:
- HLS/DASH URL "flinging" to the Default Media Receiver
- A CAF custom receiver app (`receiver-fallback/` exists solely for fallback/compatibility research and is not launched in production)
- A documented Google desktop SDK
