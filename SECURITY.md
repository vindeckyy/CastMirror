# Security policy

## Scope

This policy covers the CastMirror repository: the `castcore` engine, the Windows app (`app/winui`), the CLI, the installer and packaging scripts, tests, and tools. The Linux GTK app is in scope too, on a best-effort basis.

CastMirror is a sender on your local network. It runs no cloud service and accepts no connections from the internet. Report problems in this software, not in Chromecast firmware or other people's devices.

## Supported versions

The latest release and the default branch get security fixes. Older releases don't.

## Reporting a vulnerability

Please don't open a public issue for a vulnerability.

Use GitHub's private reporting: [Report a vulnerability](https://github.com/vindeckyy/CastMirror/security/advisories/new). If that page isn't available to you, open an issue that says only "security report, please contact me" and no details, and a maintainer will reach out.

Include:

- What the problem is and what an attacker gains
- Steps to reproduce it against this code, on devices you own
- The version (About dialog) or commit
- Log lines from **Logs, Copy for bug report** with anything sensitive removed. The bundle masks IP addresses already, and the AES session key is never logged.

You should get an acknowledgement within 7 days. We'll agree on a fix and a disclosure date with you.

## Out of scope

- Write-ups meant to attack Cast devices, take over sessions, or bypass device authentication on hardware you don't own
- A malicious device on the same LAN presented as remote code execution in CastMirror, without a memory-safety flaw in our parsing of its traffic
- Social engineering aimed at Google accounts or Cast developer consoles
- Findings that need an attacker who already has administrator rights on the PC

## Security properties

- **Control connection.** TLS to port 8009. Each device signs a fresh challenge, and CastMirror checks the signature and certificate chain against the Cast root certificates before it sends anything. The TLS certificate itself is self-signed by design, so the challenge is what proves identity.
- **Media.** Cast Streaming's per-frame AES-128-CTR, with a new key for every session. It protects the UDP stream the way Chrome mirroring does. It doesn't replace a trusted network, and RTCP feedback is only checked by source address.
- **Untrusted input.** mDNS responses, RTCP feedback and Cast channel messages come straight from the LAN. The mDNS and RTCP parsers have fuzz harnesses (`tests/fuzz`), bounds-checked loops, and a message size cap.
- **Secrets in logs.** The session key and IV mask are redacted from every log sink. Logs stay on your PC.
- **Network exposure.** The installer opens Windows Firewall for CastMirror on private and domain networks only.
- **Telemetry.** None. The only outbound request to a non-Cast host is the update check, and only when you press the button.
- **Crash files.** Minidumps stay in `%APPDATA%\CastMirror\crashes`. They contain process memory, so treat them like a password manager export before you share one.
- **Capture.** Screen and audio capture run only during a session, and stop within about 500 ms of Stop.

## Signing

Release binaries and the installer are code-signed when the maintainers' certificate is configured for that release. Every release also lists SHA-256 checksums in `SHA256SUMS-windows.txt`. Verify a download with `Get-FileHash <file> -Algorithm SHA256`.
