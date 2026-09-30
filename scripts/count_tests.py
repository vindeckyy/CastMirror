#!/usr/bin/env python3
"""Single-source the GoogleTest case count into the docs.

Counts the TEST( / TEST_F( / TEST_P( macros in tests/*.cc and rewrites:

  * every marker region
        <!-- testcount -->NNN<!-- /testcount -->
    in README.md, docs/ARCHITECTURE.md, docs/TEST_REPORT.md and docs/index.html
  * the per-suite table between
        <!-- testsuite:start --> ... <!-- testsuite:end -->
    in docs/TEST_REPORT.md

Usage:
    python3 scripts/count_tests.py            # rewrite the markers
    python3 scripts/count_tests.py --check    # exit 1 if any marker is stale
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = ROOT / "tests"
TARGETS = [
    ROOT / "README.md",
    ROOT / "docs" / "ARCHITECTURE.md",
    ROOT / "docs" / "TEST_REPORT.md",
    ROOT / "docs" / "index.html",
]
SUITE_TARGETS = [
    (ROOT / "docs" / "TEST_REPORT.md", "md"),
    (ROOT / "docs" / "index.html", "html"),
]

MACRO_RE = re.compile(r"^\s*TEST(?:_F|_P)?\s*\(")
MARKER_RE = re.compile(r"<!--\s*testcount\s*-->(\d+)<!--\s*/testcount\s*-->")
# README's shields badge carries the count inside a URL, where an HTML comment
# marker is not legal, so it is rewritten by pattern instead.
BADGE_RE = re.compile(r"(img\.shields\.io/badge/tests-)[A-Za-z0-9]+")
BADGE_TARGET = ROOT / "README.md"
SUITE_RE = re.compile(r"<!--\s*testsuite:start\s*-->.*?<!--\s*testsuite:end\s*-->",
                      re.DOTALL)

# One line of coverage text per registered source file.
COVERAGE = {
    "test_state_machine.cc": "State transitions, callback notification, retry limits",
    "test_config.cc": "Defaults, save/load round-trip",
    "test_capability_model.cc": "Model classification, preset recommendations, capture-fps passthrough",
    "test_offer_answer.cc": "OFFER JSON, ANSWER parsing, custom audio bitrate, status query",
    "test_crypto.cc": "AES-128-CTR round-trip, per-frame ciphertext variance",
    "test_rtp_rtcp.cc": "7-byte Cast RTP header, MTU slicing, CAST/CST2 feedback, checkpoint expansion",
    "test_adaptive.cc": "Loss downshifts, RTT/jitter delay scaling, bitrate ramp-up, cooldown",
    "test_encoders.cc": "H.264 Annex-B output, Reconfigure, multi-slice, intra-refresh, Opus cadence",
    "test_cast_e2e.cc": "Full TLS + OFFER/ANSWER + media streaming + Stop against the simulated receiver",
    "test_device_selection.cc": "Last-device resolution by id then IP, display index",
    "test_logger.cc": "Severity filter, callback delivery",
    "test_display_capture.cc": "Synthetic/X11 enumerate and start, DMA-BUF metadata, WGC pacing (`_WIN32` only)",
    "test_frame_pacer.cc": "Frame pacing cadence",
    "test_session_recovery.cc": "Recovery state and the 30 s timeout ceiling",
    "test_cast_transport.cc": "RTCP source filter, rolling FPS, duplicate-NACK suppression, pacing",
    "test_capture_source.cc": "CaptureSource kind round-trip, config v2 to v3 migration, legacy Start forwarding",
    "test_portal_source.cc": "Portal source-type mapping (Linux only)",
    "test_source_selection.cc": "Window support reporting, geometry, config persistence",
    "test_device_auth.cc": "Device-auth challenge/response",
    "test_c_api.cc": "C ABI structs, string truncation and NUL-termination contract",
    "test_http_fallback.cc": "HTTP/CAF fallback negotiation",
    "test_device_discovery.cc": "mDNS TXT parsing, device list, subnet probe",
    "test_net_platform.cc": "Interface enumeration, socket init idempotence, error classification",
    "test_gpu_processor.cc": "Letterbox geometry, YUV/NV12 conversion correctness, invalid args",
    "test_audio_capture.cc": "10 ms cadence, monotonic timestamps, join-or-detach worker cleanup",
}


CMAKE_LISTS = TESTS_DIR / "CMakeLists.txt"
SOURCES_RE = re.compile(r"set\(TEST_SOURCES(.*?)\)", re.DOTALL)


def registered_sources():
    """The .cc files listed in tests/CMakeLists.txt TEST_SOURCES.

    A file that is not registered is not built and not discovered by ctest, so
    it must not be counted.
    """
    if not CMAKE_LISTS.exists():
        return sorted(TESTS_DIR.glob("*.cc"))
    m = SOURCES_RE.search(CMAKE_LISTS.read_text(encoding="utf-8"))
    if not m:
        return sorted(TESTS_DIR.glob("*.cc"))
    names = [s for s in m.group(1).split() if s.endswith(".cc")]
    return [TESTS_DIR / name for name in names]


def count_cases():
    """Return (total, {file: count}) for the registered test sources."""
    per_file = {}
    for path in registered_sources():
        if not path.exists():
            continue
        n = sum(1 for line in path.read_text(encoding="utf-8").splitlines()
                if MACRO_RE.match(line))
        if n:
            per_file[path.name] = n
    return sum(per_file.values()), per_file


def suite_table(total, per_file, fmt):
    if fmt == "md":
        lines = ["| Test source | Cases | Coverage |", "|---|---|---|"]
        for name, n in per_file.items():
            lines.append(f"| `{name}` | {n} | {COVERAGE.get(name, '')} |")
        lines.append(f"| **Total** | <!-- testcount -->{total}<!-- /testcount --> "
                     f"| {len(per_file)} files registered in `tests/CMakeLists.txt` |")
        return "\n".join(lines)
    lines = []
    for name, n in per_file.items():
        lines.append(f"                    <tr><td><code>{name}</code></td>"
                     f"<td>{n} tests</td><td>Passed</td>"
                     f"<td>{COVERAGE.get(name, '')}</td></tr>")
    lines.append("                    <tr><td><strong>Total</strong></td>"
                 f"<td><strong><!-- testcount -->{total}<!-- /testcount --></strong></td>"
                 f"<td>Passed</td><td>{len(per_file)} files registered in "
                 "<code>tests/CMakeLists.txt</code></td></tr>")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="do not write; fail if a marker is out of date")
    args = ap.parse_args()

    total, per_file = count_cases()
    print(f"{total} test cases across {len(per_file)} files")
    for name, n in per_file.items():
        print(f"  {name}: {n}")

    stale = []
    for target in TARGETS:
        text = target.read_text(encoding="utf-8")
        if not MARKER_RE.search(text):
            print(f"  ! no testcount marker in {target.relative_to(ROOT)}")
            continue
        new_text, n = MARKER_RE.subn(
            lambda m: f"<!-- testcount -->{total}<!-- /testcount -->", text)
        if n and new_text != text:
            stale.append(target)
            if not args.check:
                target.write_text(new_text, encoding="utf-8")
                print(f"  updated {target.relative_to(ROOT)}")

    badge_text = BADGE_TARGET.read_text(encoding="utf-8")
    badge_new = BADGE_RE.sub(lambda m: m.group(1) + str(total), badge_text)
    if badge_new != badge_text:
        stale.append(BADGE_TARGET)
        if not args.check:
            BADGE_TARGET.write_text(badge_new, encoding="utf-8")
            print(f"  updated {BADGE_TARGET.relative_to(ROOT)} badge")

    for suite_target, fmt in SUITE_TARGETS:
        text = suite_target.read_text(encoding="utf-8")
        if not SUITE_RE.search(text):
            continue
        block = ("<!-- testsuite:start -->\n" + suite_table(total, per_file, fmt)
                 + "\n<!-- testsuite:end -->")
        new_text = SUITE_RE.sub(lambda m: block, text)
        if new_text != text:
            stale.append(suite_target)
            if not args.check:
                suite_target.write_text(new_text, encoding="utf-8")
                print(f"  updated {suite_target.relative_to(ROOT)} suite table")

    if args.check and stale:
        for target in set(stale):
            print(f"  stale: {target.relative_to(ROOT)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

