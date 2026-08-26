# Cast Streaming protocol notes

CastMirror is a **native sender** that speaks the same control + media path Chrome uses for “Cast screen”. This is an interoperability description, not a Google product SDK and not an exploit guide.

CastMirror is **not affiliated with Google**. Firmware app IDs and offer JSON can change; re-check Chromium / Open Screen when devices stop launching.

Primary references: Chromium `//components/mirroring`, `//media/cast`, and Google Open Screen `libcast` (BSD-3-Clause).

## Discovery

Devices advertise **mDNS/DNS-SD** `_googlecast._tcp.local`. TXT keys commonly include `id`, `fn` (friendly name), `md` (model), `ca` (capability bits), `st`, `ve`. CastMirror also allows adding a device by IP (port **8009**).

## Control plane (Cast V2)

1. TLS to **port 8009**.
2. Protobuf `CastMessage` namespaces:
   - `urn:x-cast:com.google.cast.tp.connection`
   - `urn:x-cast:com.google.cast.tp.heartbeat` (PING/PONG)
   - `urn:x-cast:com.google.cast.receiver` (LAUNCH / STOP / RECEIVER_STATUS)
   - `urn:x-cast:com.google.cast.webrtc` (name is historical; this is **not** ICE/DTLS WebRTC)
3. Launch the firmware mirroring app. Historical IDs:
   - Audio+video: `0F5096E8`
   - Audio-only: `85CDB22F`
4. `RECEIVER_STATUS` yields a `transportId`. Open a virtual connection and send JSON **OFFER**.
5. Receiver **ANSWER** returns `udpPort`, accepted streams, receiver SSRCs.

Device authentication (`urn:x-cast:com.google.cast.tp.deviceauth`) proves a licensed Cast receiver. Senders may skip verification; Chrome does not. Treat unknown devices on hostile networks as untrusted.

## Media plane (Cast RTP/RTCP)

- **UDP** to `answer.udpPort`.
- Profile `rtpProfile: "cast"` — custom RTP header after the standard RTP header (referenced frame id, frame id, packet id, latency fields).
- Video: H.264 Annex-B (VP8 possible in the protocol; v1 sender uses H.264).
- Audio: Opus 48 kHz, 10 ms frames.
- Per-session **AES-128-CTR** keys (`aesKey`, `aesIvMask`) in the OFFER. IV mixes `frame_id` with the mask (offset as implemented in `frame_crypto`).
- Compound **RTCP**: Sender Reports, CAST checkpoints, NACK / CST2 loss, **PLI** for keyframes.

`targetDelay` is the receiver playout buffer (CastMirror currently offers **200 ms**). That is why this is Chrome-class latency, not Sunshine/Moonlight.

## Session teardown

STOP on the receiver namespace, flush capture/encode, restore host sink mute, close UDP. Stop is budgeted **under 500 ms**.

## What this is not

- Not HLS/DASH “fling” to the Default Media Receiver
- Not a CAF custom receiver as the primary path (`receiver-fallback/` exists only as compatibility research)
- Not a documented Google desktop API
