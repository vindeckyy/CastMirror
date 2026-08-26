# Security Policy

## Scope

This policy covers the **CastMirror** repository: `castcore`, the Linux GTK GUI, the CLI, tests, and tools.

CastMirror is a **LAN sender**. It does not operate a cloud service and does not accept untrusted internet clients. Security reports should be about this software, not about attacking Chromecast firmware or third-party devices.

## Supported versions

The default branch is the only supported line until tagged releases exist.

## Reporting a vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Email the maintainer using the address on the GitHub profile that owns this repository, with:

- A description of the issue and impact
- Steps to reproduce (PoC against **this** codebase, not against others’ devices)
- Affected commit SHA or version
- Any logs from `~/.config/castmirror/castmirror.log` that do not contain secrets you care about

You should receive an acknowledgement within 7 days. We will work with you on a fix and coordinated disclosure.

## What we will not accept as a “vulnerability”

- Public write-ups whose primary purpose is to attack Cast devices, steal sessions, or bypass device authentication on hardware you do not own
- Reports that require a malicious device on the same LAN presented as a remote RCE in CastMirror
- Social-engineering Google accounts or Cast developer consoles

## Security properties (honest)

- Control plane: TLS to Cast port **8009**. Device certificate verification is a goal; treat untrusted networks accordingly.
- Media: Cast Streaming **AES-128-CTR** per the protocol (keys in the OFFER). This protects the UDP media path the way Chrome mirroring does; it is not a substitute for a trusted LAN.
- No telemetry is sent off-LAN by CastMirror itself.
- Capture runs only while a session is streaming and should stop within 500 ms of Stop.
