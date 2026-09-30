#!/usr/bin/env python3
"""Regenerate the CastMirror device matrix from recorded soak runs.

Reads every `castmirror.device_soak.run.v1` JSON run record under --runs-dir and
renders the markdown block that belongs between the markers

    <!-- BEGIN GENERATED:DEVICE_MATRIX -->
    <!-- END GENERATED:DEVICE_MATRIX -->

inside docs/DEVICE_MATRIX.md.

Rules enforced by this generator (they are the whole point of it):

  * Every number in the real-device table comes from a recorded run JSON.
  * A device that has no recorded run is listed with "not measured" -- the
    generator never emits a plausible-looking value.
  * Runs with `"synthetic": true` are rendered in a SEPARATE, loudly-labelled
    section so they can never be mistaken for real-hardware evidence.

Usage:
  render_device_matrix.py --runs-dir docs/bench/device_runs --stdout
  render_device_matrix.py --runs-dir docs/bench/device_runs --out docs/DEVICE_MATRIX.md
  render_device_matrix.py --runs-dir DIR --out FILE --check   # exit 1 if stale
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

BEGIN = "<!-- BEGIN GENERATED:DEVICE_MATRIX -->"
END = "<!-- END GENERATED:DEVICE_MATRIX -->"

# Devices we know exist in the field but which have NO recorded run yet. They are
# listed so the "not measured" state is explicit rather than a silently absent
# row. Model strings follow the corrected docs/COMPATIBILITY.md (Google TV
# Streamer `md` = G3MYX per the concurrent docs truth pass).
KNOWN_DEVICES = [
    ("Chromecast (1st Gen)", "H2G2-42"),
    ("Chromecast (2nd Gen)", "NC2-6A5"),
    ("Chromecast (3rd Gen)", "GA00439"),
    ("Chromecast Ultra", "NC2-6A5-D"),
    ("Chromecast with Google TV (4K)", "GZRNL"),
    ("Google TV Streamer (4K)", "G3MYX"),
    ("Google Nest Hub (2nd Gen)", "GUIK2"),
    ("Android TV / Google TV (Sony Bravia)", "XR-55A80J"),
    ("Vizio SmartCast TV", "V405-H19"),
]


def fmt(v, suffix="", dash="not measured"):
    if v is None:
        return dash
    if isinstance(v, float):
        if v.is_integer():
            return "%d%s" % (int(v), suffix)
        return "%.2f%s" % (v, suffix)
    return "%s%s" % (v, suffix)


def load_runs(runs_dir):
    runs = []
    for path in sorted(glob.glob(os.path.join(runs_dir, "*.json"))):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                rec = json.load(fh)
        except Exception as exc:  # noqa: BLE001 - a bad run must not break render
            sys.stderr.write("[render_device_matrix] WARN: skipping %s (%s)\n" % (path, exc))
            continue
        if rec.get("schema") != "castmirror.device_soak.run.v1":
            sys.stderr.write("[render_device_matrix] WARN: skipping %s (unknown schema %r)\n"
                             % (path, rec.get("schema")))
            continue
        rec["_path"] = os.path.relpath(path).replace("\\", "/")
        runs.append(rec)
    return runs


def row_for(rec):
    d = rec["device"]
    s = rec["session"]
    t = rec["throughput"]
    q = rec["quality"]
    lat = rec["latency"]
    g2g = fmt(lat["glass_to_glass_ms"], " ms")
    if lat["glass_to_glass_ms"] is not None and lat["uncertainty_ms"] is not None:
        g2g += " +/- %s ms" % fmt(lat["uncertainty_ms"])
    if lat["method"]:
        g2g += " (%s)" % lat["method"]
    status = rec.get("notes") or ""
    if rec.get("synthetic"):
        status = ("**SYNTHETIC -- NOT REAL HARDWARE**" + ("; " + status if status else ""))
    else:
        status = ("recorded run; app-ID preflight passed"
                  + ("; " + status if status else ""))
    return "| %s | `%s` | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
        fmt(d["model"], dash="_not measured_"),
        d["md"] or "?",
        fmt(d["firmware"], dash="_not measured_"),
        fmt(rec["link"]["wifi_band"], dash="_not measured_"),
        fmt(s["quality_preset"]),
        fmt(s["target_bitrate_kbps"], " kbps"),
        fmt(s["target_delay_ms"], " ms"),
        fmt(s["capture_source"], dash="_not measured_"),
        fmt(s["duration_s"], " s"),
        fmt(s["time_to_first_frame_ms"], " ms"),
        g2g,
        fmt(t["peak_bitrate_kbps"], " kbps"),
        fmt(t["average_bitrate_kbps"], " kbps"),
        fmt(q["packet_loss_fraction_peak"]),
        fmt(rec["feedback"]["nack_count"]),
        fmt(rec["feedback"]["pli_count"]),
    )


HEADER = ("| Device | `md` | Firmware / app version | Wi-Fi band | Preset | "
          "Target bitrate | targetDelay | Capture source | Duration | TTFF | "
          "Glass-to-glass | Peak bitrate | Avg bitrate | Packet loss (peak) | "
          "NACK | PLI |")
SEP = "|" + "---|" * 16


def render_block(runs):
    real = [r for r in runs if not r.get("synthetic")]
    synth = [r for r in runs if r.get("synthetic")]
    out = []
    w = out.append

    w("<!-- This table is GENERATED. Do not hand-edit it. -->")
    w("<!-- Regenerate with: scripts/render_device_matrix.sh -->")
    w("")
    w("## Physical Device Benchmark Matrix")
    w("")
    w("Every value below is copied from a recorded run in `docs/bench/device_runs/`.")
    w("A cell reading `not measured` means exactly that: no run has been recorded")
    w("for it. CastMirror does **not** synthesise plausible numbers here.")
    w("")
    if not real:
        w("> **No real-device runs have been recorded.** The CastMirror repository")
        w("> contains no `castmirror.device_soak.run.v1` record with")
        w("> `\"synthetic\": false`, so the real-device matrix is empty. Run")
        w("> `scripts/soak_real_device.sh` against physical hardware to populate it.")
        w("")
    w(HEADER)
    w(SEP)
    if real:
        for rec in real:
            w(row_for(rec))
    else:
        for model, md in KNOWN_DEVICES:
            w("| %s | `%s` | _not measured_ | _not measured_ | _not measured_ | "
              "_not measured_ | _not measured_ | _not measured_ | _not measured_ | "
              "_not measured_ | _not measured_ | _not measured_ | _not measured_ | "
              "_not measured_ | _not measured_ | _not measured_ |" % (model, md))
    w("")

    if real:
        w("### Recorded run detail")
        w("")
        for rec in real:
            w(render_run_detail(rec))

    w("### Synthetic / harness self-test runs")
    w("")
    if not synth:
        w("_None recorded._")
    else:
        w("> **These runs are NOT real-hardware evidence.** They come from the")
        w("> local synthetic harness (`scripts/run_soak_test.sh`) and exist only to")
        w("> prove the protocol's output format round-trips. Do not cite them as")
        w("> device validation.")
        w("")
        w(HEADER)
        w(SEP)
        for rec in synth:
            w(row_for(rec))
        w("")
        for rec in synth:
            w(render_run_detail(rec))
    return "\n".join(out).rstrip() + "\n"


def render_run_detail(rec):
    out = []
    w = out.append
    label = "SYNTHETIC" if rec.get("synthetic") else "real device"
    w("#### `%s` (%s)" % (rec["run_id"], label))
    w("")
    w("- source record: `%s`" % rec["_path"])
    w("- recorded_at: %s by %s on %s" % (
        rec["recorded_at"], rec["recorded_by"], rec["host"]))
    w("- app ID: `%s`" % (rec["device"]["app_id"] or "not measured"))
    w("- adaptive rung transitions: %d" % len(rec["adaptive_rung_transitions"]))
    for r in rec["adaptive_rung_transitions"]:
        w("  - t=%s ms %s -> rung %s @ %s kbps %s" % (
            r["t_ms"], r["kind"], r["to_rung"], r["bitrate_kbps"], r["detail"]))
    w("- disconnects / reconnects: %d" % len(rec["disconnects"]))
    for r in rec["disconnects"]:
        w("  - t=%s ms %s (recovered=%s): %s" % (
            r["t_ms"], r["kind"], r["recovered"], r["reason"]))
    if rec["not_measured"]:
        w("- not measured: %s" % ", ".join(rec["not_measured"]))
    w("")
    return "\n".join(out)


def apply_to_file(out_path, block):
    with open(out_path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if BEGIN not in text or END not in text:
        raise SystemExit(
            "ERROR: %s does not contain the markers\n  %s\n  %s\n"
            "Add them once by hand, then this generator owns everything between "
            "them." % (out_path, BEGIN, END))
    pre, rest = text.split(BEGIN, 1)
    _old, post = rest.split(END, 1)
    new = pre + BEGIN + "\n" + block + END + post
    if new == text:
        return False
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new)
    return True


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs-dir", default="docs/bench/device_runs")
    ap.add_argument("--out", default=None, help="markdown file with the markers")
    ap.add_argument("--stdout", action="store_true", help="print the block only")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if --out is stale (nothing is written)")
    args = ap.parse_args(argv)

    runs = load_runs(args.runs_dir)
    block = render_block(runs)

    if args.stdout or not args.out:
        sys.stdout.write(block)
        return 0

    if args.check:
        with open(args.out, "r", encoding="utf-8") as fh:
            text = fh.read()
        if BEGIN not in text or END not in text:
            sys.stderr.write("[render_device_matrix] %s has no markers\n" % args.out)
            return 1
        current = text.split(BEGIN, 1)[1].split(END, 1)[0]
        if current.strip() != block.strip():
            sys.stderr.write("[render_device_matrix] %s is STALE\n" % args.out)
            return 1
        sys.stderr.write("[render_device_matrix] %s is up to date\n" % args.out)
        return 0

    changed = apply_to_file(args.out, block)
    sys.stderr.write("[render_device_matrix] %s %s (%d runs)\n" % (
        args.out, "updated" if changed else "unchanged", len(runs)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
